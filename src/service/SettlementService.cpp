#include "cofinance/service/SettlementService.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cofinance/db/ConnectionPool.h"
#include "cofinance/db/GroupDao.h"
#include "cofinance/db/TransactionGuard.h"
#include "cofinance/util/Logger.h"

namespace cofinance {
namespace service {
namespace {

constexpr std::int64_t kMaxSettlementCents = 9000000000000000LL;

void setError(ServiceError* error, int status, int code, std::string message) {
    if (error != nullptr) {
        error->http_status = status;
        error->code = code;
        error->message = std::move(message);
    }
}

void setDatabaseError(ServiceError* error, const char* operation, const std::string& detail) {
    LOG_ERROR("%s 失败：%s", operation, detail.c_str());
    setError(error, 500, 2001, "服务器内部错误");
}

bool addSigned(std::int64_t left, std::int64_t right, std::int64_t* result) {
    if ((right > 0 && left > INT64_MAX - right) ||
        (right < 0 && left < INT64_MIN - right)) {
        return false;
    }
    *result = left + right;
    return true;
}

bool findMembershipForRead(db::GroupDao& groups, std::uint64_t group_id,
                           std::uint64_t user_id, std::optional<db::MembershipRecord>* member,
                           ServiceError* error, std::string* db_error) {
    std::optional<db::GroupRecord> group;
    if (!groups.findGroup(group_id, &group, db_error)) {
        setDatabaseError(error, "读取财务组", *db_error);
        return false;
    }
    if (!group.has_value() || group->status != 0) {
        setError(error, 404, 1005, "组不存在或已解散");
        return false;
    }
    if (!groups.findMembership(group_id, user_id, member, db_error)) {
        setDatabaseError(error, "读取组成员", *db_error);
        return false;
    }
    if (!member->has_value() || member->value().status != 1) {
        setError(error, 403, 1004, "无权访问该组");
        return false;
    }
    return true;
}

bool lockGroupAndMember(db::GroupDao& groups, std::uint64_t group_id, std::uint64_t user_id,
                        std::optional<db::MembershipRecord>* member, ServiceError* error,
                        std::string* db_error) {
    std::optional<db::GroupRecord> group;
    if (!groups.lockGroup(group_id, &group, db_error)) {
        setDatabaseError(error, "锁定财务组", *db_error);
        return false;
    }
    if (!group.has_value() || group->status != 0) {
        setError(error, 404, 1005, "组不存在或已解散");
        return false;
    }
    if (!groups.lockMembership(group_id, user_id, member, db_error)) {
        setDatabaseError(error, "锁定组成员", *db_error);
        return false;
    }
    if (!member->has_value() || member->value().status != 1) {
        setError(error, 403, 1004, "无权访问该组");
        return false;
    }
    return true;
}

bool normalizeRange(const std::optional<std::string>& from, const std::optional<std::string>& to,
                    std::optional<std::string>* db_from, std::optional<std::string>* db_to,
                    ServiceError* error) {
    db_from->reset();
    db_to->reset();
    if (from.has_value()) {
        std::string detail;
        std::string normalized;
        if (!util::parseUtcBoundary(*from, false, &normalized, &detail)) {
            setError(error, 400, 1001, "from 参数错误：" + detail);
            return false;
        }
        *db_from = std::move(normalized);
    }
    if (to.has_value()) {
        std::string detail;
        std::string normalized;
        if (!util::parseUtcBoundary(*to, true, &normalized, &detail)) {
            setError(error, 400, 1001, "to 参数错误：" + detail);
            return false;
        }
        *db_to = std::move(normalized);
    }
    if (db_from->has_value() && db_to->has_value() && **db_from > **db_to) {
        setError(error, 400, 1001, "from 不能晚于 to");
        return false;
    }
    return true;
}

bool checkedAddToBalance(BalanceSummary* summary, std::int64_t delta) {
    return addSigned(summary->total_balance_cents, delta, &summary->total_balance_cents);
}

void appendGreedySuggestions(std::vector<db::BalanceRecord> balances,
                             std::vector<SettlementSuggestion>* suggestions) {
    std::vector<db::BalanceRecord> creditors;
    std::vector<db::BalanceRecord> debtors;
    for (const db::BalanceRecord& balance : balances) {
        if (balance.balance_cents > 0) {
            creditors.push_back(balance);
        } else if (balance.balance_cents < 0) {
            debtors.push_back(balance);
        }
    }
    std::sort(creditors.begin(), creditors.end(),
              [](const db::BalanceRecord& left, const db::BalanceRecord& right) {
                  if (left.balance_cents != right.balance_cents) {
                      return left.balance_cents > right.balance_cents;
                  }
                  return left.user_id < right.user_id;
              });
    std::sort(debtors.begin(), debtors.end(),
              [](const db::BalanceRecord& left, const db::BalanceRecord& right) {
                  if (left.balance_cents != right.balance_cents) {
                      return left.balance_cents < right.balance_cents;
                  }
                  return left.user_id < right.user_id;
              });

    std::size_t creditor_index = 0;
    std::size_t debtor_index = 0;
    while (creditor_index < creditors.size() && debtor_index < debtors.size()) {
        db::BalanceRecord& creditor = creditors[creditor_index];
        db::BalanceRecord& debtor = debtors[debtor_index];
        const std::int64_t amount = std::min(creditor.balance_cents, -debtor.balance_cents);
        SettlementSuggestion suggestion;
        suggestion.from_user_id = debtor.user_id;
        suggestion.from_username = debtor.username;
        suggestion.from_nickname = debtor.nickname;
        suggestion.to_user_id = creditor.user_id;
        suggestion.to_username = creditor.username;
        suggestion.to_nickname = creditor.nickname;
        suggestion.amount_cents = amount;
        suggestions->push_back(std::move(suggestion));
        creditor.balance_cents -= amount;
        debtor.balance_cents += amount;
        if (creditor.balance_cents == 0) {
            ++creditor_index;
        }
        if (debtor.balance_cents == 0) {
            ++debtor_index;
        }
    }
}

void buildOptimalSuggestions(const std::vector<db::BalanceRecord>& all_balances,
                             std::vector<SettlementSuggestion>* suggestions) {
    std::vector<db::BalanceRecord> active;
    for (const db::BalanceRecord& balance : all_balances) {
        if (balance.balance_cents != 0) {
            active.push_back(balance);
        }
    }
    const int count = static_cast<int>(active.size());
    if (count == 0) {
        return;
    }
    if (count > 20) {
        appendGreedySuggestions(active, suggestions);
        return;
    }

    const std::uint32_t state_count = 1U << static_cast<unsigned>(count);
    std::vector<std::int64_t> subset_sum(state_count, 0);
    for (std::uint32_t mask = 1; mask < state_count; ++mask) {
        const int bit = __builtin_ctz(mask);
        subset_sum[mask] = subset_sum[mask & (mask - 1)] + active[bit].balance_cents;
    }

    // dp[mask] = mask 内最多能拆成多少个互不相交的零和子集。
    // 每次移除一个元素，只有当前剩余集合的净额为零时才新增一个子集。
    std::vector<int> dp(state_count, std::numeric_limits<int>::min());
    std::vector<std::uint32_t> previous(state_count, 0);
    dp[0] = 0;
    for (std::uint32_t mask = 1; mask < state_count; ++mask) {
        int best_previous = std::numeric_limits<int>::min();
        std::uint32_t best_mask = 0;
        std::uint32_t bits = mask;
        while (bits != 0) {
            const int bit = __builtin_ctz(bits);
            const std::uint32_t parent = mask & ~(1U << static_cast<unsigned>(bit));
            if (dp[parent] > best_previous) {
                best_previous = dp[parent];
                best_mask = parent;
            }
            bits &= bits - 1;
        }
        dp[mask] = best_previous + (subset_sum[mask] == 0 ? 1 : 0);
        previous[mask] = best_mask;
    }

    std::vector<db::BalanceRecord> current_group;
    std::uint32_t mask = state_count - 1;
    while (mask != 0) {
        const std::uint32_t parent = previous[mask];
        const std::uint32_t removed = mask ^ parent;
        current_group.push_back(active[__builtin_ctz(removed)]);
        mask = parent;
        if (mask == 0 || subset_sum[mask] == 0) {
            appendGreedySuggestions(std::move(current_group), suggestions);
            current_group.clear();
        }
    }
}

}  // namespace

SettlementService::SettlementService(db::ConnectionPool& pool) : pool_(pool) {}

bool SettlementService::calculateBalances(db::MySqlConnection& connection,
                                          std::uint64_t group_id,
                                          std::vector<db::BalanceRecord>* balances,
                                          std::string* db_error) {
    if (balances == nullptr) {
        if (db_error != nullptr) {
            *db_error = "余额输出参数为空";
        }
        return false;
    }
    db::GroupDao groups(connection);
    std::vector<db::MembershipRecord> members;
    if (!groups.listJoinedMembers(group_id, &members, db_error)) {
        return false;
    }
    db::SettlementDao settlements(connection);
    std::vector<db::UserDelta> transaction_deltas;
    std::vector<db::UserDelta> settlement_deltas;
    if (!settlements.loadTransactionDeltas(group_id, &transaction_deltas, db_error) ||
        !settlements.loadSettlementDeltas(group_id, &settlement_deltas, db_error)) {
        return false;
    }
    std::map<std::uint64_t, std::int64_t> transaction_by_user;
    std::map<std::uint64_t, std::int64_t> settlement_by_user;
    for (const db::UserDelta& delta : transaction_deltas) {
        transaction_by_user[delta.user_id] = delta.delta_cents;
    }
    for (const db::UserDelta& delta : settlement_deltas) {
        settlement_by_user[delta.user_id] = delta.delta_cents;
    }

    balances->clear();
    balances->reserve(members.size());
    std::int64_t total_balance_cents = 0;
    for (const db::MembershipRecord& member : members) {
        db::BalanceRecord balance;
        balance.user_id = member.user_id;
        balance.username = member.username;
        balance.nickname = member.nickname;
        balance.avatar_url = member.avatar_url;
        const auto transaction = transaction_by_user.find(member.user_id);
        if (transaction != transaction_by_user.end()) {
            balance.transaction_balance_cents = transaction->second;
        }
        const auto settlement = settlement_by_user.find(member.user_id);
        if (settlement != settlement_by_user.end()) {
            balance.settlement_balance_cents = settlement->second;
        }
        if (!addSigned(balance.transaction_balance_cents, balance.settlement_balance_cents,
                       &balance.balance_cents)) {
            if (db_error != nullptr) {
                *db_error = "成员余额溢出";
            }
            return false;
        }
        if (!addSigned(total_balance_cents, balance.balance_cents, &total_balance_cents)) {
            if (db_error != nullptr) {
                *db_error = "组余额合计溢出";
            }
            return false;
        }
        balances->push_back(std::move(balance));
    }
    if (total_balance_cents != 0) {
        if (db_error != nullptr) {
            *db_error = "组内余额合计不为零，账目不一致";
        }
        return false;
    }
    return true;
}

bool SettlementService::getStats(std::uint64_t user_id, std::uint64_t group_id,
                                 const StatsInput& input, GroupStats* stats,
                                 ServiceError* error) {
    if (stats == nullptr) {
        setError(error, 500, 2001, "服务器内部错误");
        return false;
    }
    std::optional<std::string> db_from;
    std::optional<std::string> db_to;
    if (!normalizeRange(input.from, input.to, &db_from, &db_to, error)) {
        return false;
    }
    std::string db_error;
    auto lease = pool_.acquire(-1, &db_error);
    if (!lease) {
        setDatabaseError(error, "获取数据库连接", db_error);
        return false;
    }
    db::GroupDao groups(lease.connection());
    std::optional<db::MembershipRecord> member;
    if (!findMembershipForRead(groups, group_id, user_id, &member, error, &db_error)) {
        return false;
    }
    db::SettlementDao settlements(lease.connection());
    GroupStats loaded;
    if (!settlements.summarize(group_id, db_from, db_to, &loaded.summary, &db_error) ||
        !settlements.listCategoryStats(group_id, db_from, db_to, &loaded.categories, &db_error) ||
        !settlements.listMemberStats(group_id, db_from, db_to, &loaded.members, &db_error)) {
        setDatabaseError(error, "查询组统计", db_error);
        return false;
    }

    std::map<std::uint64_t, db::MemberStatRecord> by_user;
    for (const db::MemberStatRecord& item : loaded.members) {
        by_user[item.user_id] = item;
    }
    std::vector<db::MembershipRecord> current_members;
    if (!groups.listJoinedMembers(group_id, &current_members, &db_error)) {
        setDatabaseError(error, "读取统计成员", db_error);
        return false;
    }
    std::vector<db::MemberStatRecord> ordered;
    ordered.reserve(by_user.size() + current_members.size());
    std::map<std::uint64_t, bool> emitted;
    for (const db::MembershipRecord& current : current_members) {
        db::MemberStatRecord item;
        const auto found = by_user.find(current.user_id);
        if (found != by_user.end()) {
            item = found->second;
        } else {
            item.user_id = current.user_id;
            item.username = current.username;
            item.nickname = current.nickname;
            item.avatar_url = current.avatar_url;
        }
        emitted[current.user_id] = true;
        ordered.push_back(std::move(item));
    }
    for (const db::MemberStatRecord& item : loaded.members) {
        if (!emitted[item.user_id]) {
            ordered.push_back(item);
        }
    }
    loaded.from = input.from;
    loaded.to = input.to;
    loaded.members = std::move(ordered);
    *stats = std::move(loaded);
    return true;
}

bool SettlementService::getBalances(std::uint64_t user_id, std::uint64_t group_id,
                                    BalanceSummary* balances, ServiceError* error) {
    if (balances == nullptr) {
        setError(error, 500, 2001, "服务器内部错误");
        return false;
    }
    std::string db_error;
    auto lease = pool_.acquire(-1, &db_error);
    if (!lease) {
        setDatabaseError(error, "获取数据库连接", db_error);
        return false;
    }
    db::GroupDao groups(lease.connection());
    std::optional<db::MembershipRecord> member;
    if (!findMembershipForRead(groups, group_id, user_id, &member, error, &db_error)) {
        return false;
    }
    BalanceSummary loaded;
    if (!calculateBalances(lease.connection(), group_id, &loaded.items, &db_error)) {
        setDatabaseError(error, "计算组余额", db_error);
        return false;
    }
    for (const db::BalanceRecord& item : loaded.items) {
        if (!checkedAddToBalance(&loaded, item.balance_cents)) {
            setDatabaseError(error, "汇总组余额", "余额合计溢出");
            return false;
        }
    }
    *balances = std::move(loaded);
    return true;
}

bool SettlementService::suggestSettlements(std::uint64_t user_id, std::uint64_t group_id,
                                           std::vector<SettlementSuggestion>* suggestions,
                                           ServiceError* error) {
    if (suggestions == nullptr) {
        setError(error, 500, 2001, "服务器内部错误");
        return false;
    }
    std::string db_error;
    auto lease = pool_.acquire(-1, &db_error);
    if (!lease) {
        setDatabaseError(error, "获取数据库连接", db_error);
        return false;
    }
    db::GroupDao groups(lease.connection());
    std::optional<db::MembershipRecord> member;
    if (!findMembershipForRead(groups, group_id, user_id, &member, error, &db_error)) {
        return false;
    }
    std::vector<db::BalanceRecord> balances;
    if (!calculateBalances(lease.connection(), group_id, &balances, &db_error)) {
        setDatabaseError(error, "计算结算建议", db_error);
        return false;
    }

    suggestions->clear();
    buildOptimalSuggestions(balances, suggestions);
    return true;
}

bool SettlementService::confirmSettlement(std::uint64_t user_id, std::uint64_t group_id,
                                          std::uint64_t from_user_id, std::uint64_t to_user_id,
                                          std::int64_t amount_cents,
                                          db::SettlementRecord* settlement, ServiceError* error) {
    if (settlement == nullptr) {
        setError(error, 500, 2001, "服务器内部错误");
        return false;
    }
    if (from_user_id == 0 || to_user_id == 0 || from_user_id == to_user_id) {
        setError(error, 400, 1001, "结算双方必须是两个不同的有效用户");
        return false;
    }
    if (amount_cents <= 0 || amount_cents > kMaxSettlementCents) {
        setError(error, 400, 1001, "amount_cents 必须是合法正整数");
        return false;
    }
    std::string db_error;
    auto lease = pool_.acquire(-1, &db_error);
    if (!lease) {
        setDatabaseError(error, "获取数据库连接", db_error);
        return false;
    }
    db::TransactionGuard transaction(lease.connection(), &db_error);
    if (!transaction.active()) {
        setDatabaseError(error, "开启确认结算事务", db_error);
        return false;
    }
    db::GroupDao groups(lease.connection());
    std::optional<db::MembershipRecord> actor;
    if (!lockGroupAndMember(groups, group_id, user_id, &actor, error, &db_error)) {
        return false;
    }
    std::vector<db::BalanceRecord> balances;
    if (!calculateBalances(lease.connection(), group_id, &balances, &db_error)) {
        setDatabaseError(error, "重新计算结算余额", db_error);
        return false;
    }
    const db::BalanceRecord* from_balance = nullptr;
    const db::BalanceRecord* to_balance = nullptr;
    for (const db::BalanceRecord& balance : balances) {
        if (balance.user_id == from_user_id) {
            from_balance = &balance;
        }
        if (balance.user_id == to_user_id) {
            to_balance = &balance;
        }
    }
    if (from_balance == nullptr || to_balance == nullptr) {
        setError(error, 400, 1001, "结算双方必须都是组内正式成员");
        return false;
    }
    if (from_balance->balance_cents >= 0) {
        setError(error, 409, 1015, "付款方当前没有待付余额");
        return false;
    }
    if (to_balance->balance_cents <= 0) {
        setError(error, 409, 1015, "收款方当前没有待收余额");
        return false;
    }
    if (amount_cents > -from_balance->balance_cents ||
        amount_cents > to_balance->balance_cents) {
        setError(error, 409, 1015, "结算金额超过当前待付或待收余额，请刷新建议");
        return false;
    }

    db::SettlementRecord created;
    created.group_id = group_id;
    created.from_user_id = from_user_id;
    created.to_user_id = to_user_id;
    created.amount_cents = amount_cents;
    created.status = 1;
    created.created_by = user_id;
    db::SettlementDao settlements(lease.connection());
    if (!settlements.insertSettlement(created, &db_error)) {
        setDatabaseError(error, "写入结算记录", db_error);
        return false;
    }
    if (!settlements.findSettlement(created.id, &created, &db_error)) {
        setDatabaseError(error, "读取结算记录", db_error);
        return false;
    }
    if (!transaction.commit(&db_error)) {
        setDatabaseError(error, "提交确认结算事务", db_error);
        return false;
    }
    *settlement = std::move(created);
    return true;
}

bool SettlementService::listSettlements(std::uint64_t user_id, std::uint64_t group_id,
                                        std::vector<db::SettlementRecord>* settlements,
                                        ServiceError* error) {
    if (settlements == nullptr) {
        setError(error, 500, 2001, "服务器内部错误");
        return false;
    }
    std::string db_error;
    auto lease = pool_.acquire(-1, &db_error);
    if (!lease) {
        setDatabaseError(error, "获取数据库连接", db_error);
        return false;
    }
    db::GroupDao groups(lease.connection());
    std::optional<db::MembershipRecord> member;
    if (!findMembershipForRead(groups, group_id, user_id, &member, error, &db_error)) {
        return false;
    }
    db::SettlementDao settlements_dao(lease.connection());
    if (!settlements_dao.listSettlements(group_id, settlements, &db_error)) {
        setDatabaseError(error, "查询结算历史", db_error);
        return false;
    }
    return true;
}

}  // namespace service
}  // namespace cofinance
