#include "cofinance/service/LedgerService.h"

#include <cstdint>
#include <iomanip>
#include <optional>
#include <set>
#include <sstream>
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

constexpr std::int64_t kMaxAmountCents = 9000000000000000LL;
constexpr std::size_t kMaxParticipants = 20;

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

bool hasControlCharacters(const std::string& text) {
    for (char c : text) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 0x20 || uc == 0x7f) {
            return true;
        }
    }
    return false;
}

bool validCategoryText(const std::string& text, std::size_t max_length) {
    bool valid_utf8 = false;
    const std::size_t length = util::utf8Length(text, &valid_utf8);
    return valid_utf8 && length <= max_length && !hasControlCharacters(text);
}

bool validUuid(const std::string& text) {
    if (text.size() != 36) {
        return false;
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (text[i] != '-') {
                return false;
            }
            continue;
        }
        const bool digit = text[i] >= '0' && text[i] <= '9';
        const bool lower = text[i] >= 'a' && text[i] <= 'f';
        const bool upper = text[i] >= 'A' && text[i] <= 'F';
        if (!digit && !lower && !upper) {
            return false;
        }
    }
    return true;
}

bool parseFixedInt(const std::string& text, std::size_t pos, std::size_t length, int* value) {
    int parsed = 0;
    for (std::size_t i = 0; i < length; ++i) {
        const char c = text[pos + i];
        if (c < '0' || c > '9') {
            return false;
        }
        parsed = parsed * 10 + (c - '0');
    }
    *value = parsed;
    return true;
}

bool leapYear(int year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int daysInMonth(int year, int month) {
    static const int days[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && leapYear(year)) {
        return 29;
    }
    return days[month];
}

bool parseUtcBoundary(const std::string& raw, bool end_of_day, std::string* db_value,
                      std::string* detail) {
    if (raw.size() != 10 && raw.size() != 20 && raw.size() != 24) {
        *detail = "时间必须是 YYYY-MM-DD 或 YYYY-MM-DDTHH:MM:SSZ";
        return false;
    }
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (!parseFixedInt(raw, 0, 4, &year) || raw[4] != '-' ||
        !parseFixedInt(raw, 5, 2, &month) || raw[7] != '-' ||
        !parseFixedInt(raw, 8, 2, &day)) {
        *detail = "时间格式错误";
        return false;
    }
    if (raw.size() >= 20) {
        if (raw[10] != 'T' || raw[13] != ':' || raw[16] != ':') {
            *detail = "时间格式错误";
            return false;
        }
        if (!parseFixedInt(raw, 11, 2, &hour) || !parseFixedInt(raw, 14, 2, &minute) ||
            !parseFixedInt(raw, 17, 2, &second)) {
            *detail = "时间格式错误";
            return false;
        }
        if (raw.size() == 20 && raw[19] != 'Z') {
            *detail = "时间必须使用 UTC 的 Z 后缀";
            return false;
        }
        if (raw.size() == 24) {
            if (raw[19] != '.' || raw[23] != 'Z' || raw[20] < '0' || raw[20] > '9' ||
                raw[21] < '0' || raw[21] > '9' || raw[22] < '0' || raw[22] > '9') {
                *detail = "时间毫秒格式错误";
                return false;
            }
        }
    } else if (end_of_day) {
        hour = 23;
        minute = 59;
        second = 59;
    }
    if (year < 1970 || year > 9999 || month < 1 || month > 12 || day < 1 ||
        day > daysInMonth(year, month) || hour < 0 || hour > 23 || minute < 0 ||
        minute > 59 || second < 0 || second > 59) {
        *detail = "时间超出合法范围";
        return false;
    }
    std::ostringstream out;
    out << std::setfill('0') << std::setw(4) << year << '-' << std::setw(2) << month << '-'
        << std::setw(2) << day << ' ' << std::setw(2) << hour << ':' << std::setw(2)
        << minute << ':' << std::setw(2) << second;
    *db_value = out.str();
    return true;
}

bool buildShares(const TransactionInput& input, std::vector<std::int64_t>* shares,
                 ServiceError* error) {
    if (input.type != 0 && input.type != 1) {
        setError(error, 400, 1001, "type 只能是 0(支出) 或 1(收入)");
        return false;
    }
    if (input.split_type != 0 && input.split_type != 1) {
        setError(error, 400, 1001, "split_type 只能是 0(均分) 或 1(自定义)");
        return false;
    }
    if (input.amount_cents <= 0 || input.amount_cents > kMaxAmountCents) {
        setError(error, 400, 1001, "amount_cents 必须是合法正整数");
        return false;
    }
    if (input.payer_id == 0) {
        setError(error, 400, 1001, "payer_id 非法");
        return false;
    }
    if (input.participants.empty() || input.participants.size() > kMaxParticipants) {
        setError(error, 400, 1001, "participants 数量必须在 1-20 之间");
        return false;
    }
    if (!validCategoryText(input.note, 255)) {
        setError(error, 400, 1001, "note 不能超过 255 个字符且不能包含控制字符");
        return false;
    }
    if (input.client_token.has_value() && !validUuid(*input.client_token)) {
        setError(error, 400, 1001, "client_token 必须是 UUID 格式");
        return false;
    }

    std::set<std::uint64_t> seen;
    shares->clear();
    shares->reserve(input.participants.size());
    std::int64_t custom_total = 0;
    for (const ParticipantInput& participant : input.participants) {
        if (participant.user_id == 0 || !seen.insert(participant.user_id).second) {
            setError(error, 400, 1001, "participants 中存在空用户或重复用户");
            return false;
        }
        if (input.split_type == 1) {
            if (!participant.share_cents.has_value() || *participant.share_cents < 0) {
                setError(error, 400, 1001, "自定义分摊必须为每个参与人提供非负 share_cents");
                return false;
            }
            if (*participant.share_cents > kMaxAmountCents - custom_total) {
                setError(error, 400, 1001, "分摊金额过大");
                return false;
            }
            custom_total += *participant.share_cents;
            shares->push_back(*participant.share_cents);
        }
    }
    if (input.split_type == 1) {
        if (custom_total != input.amount_cents) {
            setError(error, 400, 1010, "分摊金额之和必须等于总金额");
            return false;
        }
        return true;
    }

    const std::int64_t base =
        input.amount_cents / static_cast<std::int64_t>(input.participants.size());
    const std::int64_t remainder =
        input.amount_cents % static_cast<std::int64_t>(input.participants.size());
    for (std::size_t i = 0; i < input.participants.size(); ++i) {
        const std::int64_t extra = static_cast<std::int64_t>(i) < remainder ? 1 : 0;
        shares->push_back(base + extra);
    }
    return true;
}

bool findActiveGroupAndMember(db::GroupDao& groups, std::uint64_t group_id,
                              std::uint64_t user_id, std::optional<db::GroupRecord>* group,
                              std::optional<db::MembershipRecord>* member, ServiceError* error,
                              std::string* db_error) {
    if (!groups.findGroup(group_id, group, db_error)) {
        setDatabaseError(error, "读取财务组", *db_error);
        return false;
    }
    if (!group->has_value() || group->value().status != 0) {
        setError(error, 404, 1005, "组不存在或已解散");
        return false;
    }
    if (!groups.findMembership(group_id, user_id, member, db_error)) {
        setDatabaseError(error, "读取组成员身份", *db_error);
        return false;
    }
    if (!member->has_value() || member->value().status != 1) {
        setError(error, 403, 1004, "无权访问该组");
        return false;
    }
    return true;
}

bool lockActiveGroupAndMember(db::GroupDao& groups, std::uint64_t group_id,
                              std::uint64_t user_id, std::optional<db::GroupRecord>* group,
                              std::optional<db::MembershipRecord>* member, ServiceError* error,
                              std::string* db_error) {
    if (!groups.lockGroup(group_id, group, db_error)) {
        setDatabaseError(error, "锁定财务组", *db_error);
        return false;
    }
    if (!group->has_value() || group->value().status != 0) {
        setError(error, 404, 1005, "组不存在或已解散");
        return false;
    }
    if (!groups.lockMembership(group_id, user_id, member, db_error)) {
        setDatabaseError(error, "锁定组成员身份", *db_error);
        return false;
    }
    if (!member->has_value() || member->value().status != 1) {
        setError(error, 403, 1004, "无权访问该组");
        return false;
    }
    return true;
}

bool validateLedgerRelations(db::GroupDao& groups, db::CategoryDao& categories,
                             std::uint64_t group_id, const TransactionInput& input,
                             ServiceError* error, std::string* db_error) {
    if (input.category_id != 0) {
        std::optional<db::CategoryRecord> category;
        if (!categories.findById(input.category_id, &category, db_error)) {
            setDatabaseError(error, "读取交易分类", *db_error);
            return false;
        }
        if (!category.has_value() ||
            (category->group_id != 0 && category->group_id != group_id)) {
            setError(error, 400, 1001, "分类不存在或不属于该组");
            return false;
        }
    }

    std::optional<db::MembershipRecord> payer;
    if (!groups.findMembership(group_id, input.payer_id, &payer, db_error)) {
        setDatabaseError(error, "读取付款人身份", *db_error);
        return false;
    }
    if (!payer.has_value() || payer->status != 1) {
        setError(error, 400, 1013, "付款人必须是组内正式成员");
        return false;
    }
    for (const ParticipantInput& participant : input.participants) {
        std::optional<db::MembershipRecord> member;
        if (!groups.findMembership(group_id, participant.user_id, &member, db_error)) {
            setDatabaseError(error, "读取参与人身份", *db_error);
            return false;
        }
        if (!member.has_value() || member->status != 1) {
            setError(error, 400, 1013, "参与人必须都是组内正式成员");
            return false;
        }
    }
    return true;
}

bool loadDetail(db::TransactionDao& transactions, std::uint64_t group_id,
                std::uint64_t transaction_id, db::TransactionDetail* detail,
                ServiceError* error, std::string* db_error) {
    std::optional<db::TransactionRecord> header;
    if (!transactions.findHeaderById(group_id, transaction_id, false, &header, db_error)) {
        setDatabaseError(error, "读取交易详情", *db_error);
        return false;
    }
    if (!header.has_value()) {
        setError(error, 404, 1005, "交易不存在");
        return false;
    }
    std::vector<db::ParticipantRecord> participants;
    if (!transactions.listParticipants(transaction_id, &participants, db_error)) {
        setDatabaseError(error, "读取交易分摊", *db_error);
        return false;
    }
    detail->transaction = std::move(*header);
    detail->participants = std::move(participants);
    return true;
}

bool loadExistingDetail(db::TransactionDao& transactions, db::TransactionRecord header,
                        db::TransactionDetail* detail, ServiceError* error,
                        std::string* db_error) {
    std::vector<db::ParticipantRecord> participants;
    if (!transactions.listParticipants(header.id, &participants, db_error)) {
        setDatabaseError(error, "读取幂等交易分摊", *db_error);
        return false;
    }
    detail->transaction = std::move(header);
    detail->participants = std::move(participants);
    return true;
}

bool insertShares(db::TransactionDao& transactions, std::uint64_t transaction_id,
                  const TransactionInput& input, const std::vector<std::int64_t>& shares,
                  ServiceError* error, std::string* db_error) {
    for (std::size_t i = 0; i < input.participants.size(); ++i) {
        if (!transactions.insertParticipant(transaction_id, input.participants[i].user_id,
                                            shares[i], db_error)) {
            setDatabaseError(error, "写入交易分摊", *db_error);
            return false;
        }
    }
    return true;
}

db::TransactionRecord makeTransaction(std::uint64_t user_id, std::uint64_t group_id,
                                      const TransactionInput& input) {
    db::TransactionRecord record;
    record.group_id = group_id;
    record.payer_id = input.payer_id;
    record.type = input.type;
    record.amount_cents = input.amount_cents;
    record.currency = "CNY";
    record.category_id = input.category_id;
    record.note = input.note;
    record.split_type = input.split_type;
    record.client_token = input.client_token;
    record.created_by = user_id;
    return record;
}

}  // namespace

LedgerService::LedgerService(db::ConnectionPool& pool) : pool_(pool) {}

bool LedgerService::createTransaction(std::uint64_t user_id, std::uint64_t group_id,
                                      const TransactionInput& input,
                                      db::TransactionDetail* detail, ServiceError* error) {
    if (detail == nullptr) {
        setError(error, 500, 2001, "服务器内部错误");
        return false;
    }
    std::vector<std::int64_t> shares;
    if (!buildShares(input, &shares, error)) {
        return false;
    }
    TransactionInput normalized_input = input;
    if (normalized_input.client_token.has_value()) {
        normalized_input.client_token = util::toLower(*normalized_input.client_token);
    }
    std::string db_error;
    auto lease = pool_.acquire(-1, &db_error);
    if (!lease) {
        setDatabaseError(error, "获取数据库连接", db_error);
        return false;
    }
    db::TransactionGuard transaction(lease.connection(), &db_error);
    if (!transaction.active()) {
        setDatabaseError(error, "开启新增交易事务", db_error);
        return false;
    }
    db::GroupDao groups(lease.connection());
    std::optional<db::GroupRecord> group;
    std::optional<db::MembershipRecord> actor;
    if (!lockActiveGroupAndMember(groups, group_id, user_id, &group, &actor, error,
                                  &db_error)) {
        return false;
    }
    db::TransactionDao transactions(lease.connection());
    if (normalized_input.client_token.has_value()) {
        std::optional<db::TransactionRecord> existing;
        if (!transactions.findByCreatorToken(user_id, *normalized_input.client_token, &existing,
                                             &db_error)) {
            setDatabaseError(error, "查询幂等交易", db_error);
            return false;
        }
        if (existing.has_value()) {
            if (existing->group_id != group_id) {
                setError(error, 409, 1018, "client_token 已在其他组使用");
                return false;
            }
            if (!loadExistingDetail(transactions, *existing, detail, error, &db_error)) {
                return false;
            }
            if (!transaction.commit(&db_error)) {
                setDatabaseError(error, "提交幂等交易回查", db_error);
                return false;
            }
            return true;
        }
    }
    db::CategoryDao categories(lease.connection());
    if (!validateLedgerRelations(groups, categories, group_id, normalized_input, error,
                                 &db_error)) {
        return false;
    }

    db::TransactionRecord created = makeTransaction(user_id, group_id, normalized_input);
    bool duplicate = false;
    if (!transactions.insert(created, &db_error, &duplicate)) {
        if (duplicate && normalized_input.client_token.has_value()) {
            transaction.rollback();
            std::optional<db::TransactionRecord> existing;
            if (!transactions.findByCreatorToken(user_id, *normalized_input.client_token,
                                                 &existing,
                                                 &db_error)) {
                setDatabaseError(error, "回查并发幂等交易", db_error);
                return false;
            }
            if (!existing.has_value()) {
                setDatabaseError(error, "回查并发幂等交易", "唯一键冲突后找不到首笔交易");
                return false;
            }
            if (existing->group_id != group_id) {
                setError(error, 409, 1018, "client_token 已在其他组使用");
                return false;
            }
            return loadExistingDetail(transactions, *existing, detail, error, &db_error);
        }
        setDatabaseError(error, "新增交易", db_error);
        return false;
    }
    if (!insertShares(transactions, created.id, normalized_input, shares, error, &db_error)) {
        return false;
    }
    if (!loadDetail(transactions, group_id, created.id, detail, error, &db_error)) {
        return false;
    }
    if (!transaction.commit(&db_error)) {
        setDatabaseError(error, "提交新增交易事务", db_error);
        return false;
    }
    return true;
}

bool LedgerService::listTransactions(std::uint64_t user_id, std::uint64_t group_id,
                                     const TransactionFilterInput& filter,
                                     TransactionPage* page, ServiceError* error) {
    if (page == nullptr) {
        setError(error, 500, 2001, "服务器内部错误");
        return false;
    }
    if (filter.limit == 0 || filter.limit > 100) {
        setError(error, 400, 1001, "limit 必须在 1-100 之间");
        return false;
    }
    db::TransactionFilter dao_filter;
    dao_filter.cursor = filter.cursor;
    dao_filter.category_id = filter.category_id;
    dao_filter.payer_id = filter.payer_id;
    dao_filter.limit = filter.limit + 1;
    if (filter.from.has_value()) {
        std::string detail;
        std::string normalized;
        if (!parseUtcBoundary(*filter.from, false, &normalized, &detail)) {
            setError(error, 400, 1001, "from 参数错误：" + detail);
            return false;
        }
        dao_filter.from = normalized;
    }
    if (filter.to.has_value()) {
        std::string detail;
        std::string normalized;
        if (!parseUtcBoundary(*filter.to, true, &normalized, &detail)) {
            setError(error, 400, 1001, "to 参数错误：" + detail);
            return false;
        }
        dao_filter.to = normalized;
    }
    if (dao_filter.from.has_value() && dao_filter.to.has_value() &&
        *dao_filter.from > *dao_filter.to) {
        setError(error, 400, 1001, "from 不能晚于 to");
        return false;
    }

    std::string db_error;
    auto lease = pool_.acquire(-1, &db_error);
    if (!lease) {
        setDatabaseError(error, "获取数据库连接", db_error);
        return false;
    }
    db::GroupDao groups(lease.connection());
    std::optional<db::GroupRecord> group;
    std::optional<db::MembershipRecord> member;
    if (!findActiveGroupAndMember(groups, group_id, user_id, &group, &member, error,
                                  &db_error)) {
        return false;
    }
    db::TransactionDao transactions(lease.connection());
    std::vector<db::TransactionRecord> rows;
    if (!transactions.list(group_id, dao_filter, &rows, &db_error)) {
        setDatabaseError(error, "查询交易列表", db_error);
        return false;
    }
    page->next_cursor.reset();
    if (rows.size() > filter.limit) {
        rows.resize(static_cast<std::size_t>(filter.limit));
        if (!rows.empty()) {
            page->next_cursor = rows.back().id;
        }
    }
    page->items = std::move(rows);
    return true;
}

bool LedgerService::getTransaction(std::uint64_t user_id, std::uint64_t group_id,
                                   std::uint64_t transaction_id, db::TransactionDetail* detail,
                                   ServiceError* error) {
    if (detail == nullptr) {
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
    std::optional<db::GroupRecord> group;
    std::optional<db::MembershipRecord> member;
    if (!findActiveGroupAndMember(groups, group_id, user_id, &group, &member, error,
                                  &db_error)) {
        return false;
    }
    db::TransactionDao transactions(lease.connection());
    return loadDetail(transactions, group_id, transaction_id, detail, error, &db_error);
}

bool LedgerService::updateTransaction(std::uint64_t user_id, std::uint64_t group_id,
                                      std::uint64_t transaction_id,
                                      const TransactionInput& input,
                                      db::TransactionDetail* detail, ServiceError* error) {
    if (detail == nullptr) {
        setError(error, 500, 2001, "服务器内部错误");
        return false;
    }
    std::vector<std::int64_t> shares;
    if (!buildShares(input, &shares, error)) {
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
        setDatabaseError(error, "开启修改交易事务", db_error);
        return false;
    }
    db::GroupDao groups(lease.connection());
    std::optional<db::GroupRecord> group;
    std::optional<db::MembershipRecord> actor;
    if (!lockActiveGroupAndMember(groups, group_id, user_id, &group, &actor, error,
                                  &db_error)) {
        return false;
    }
    db::TransactionDao transactions(lease.connection());
    std::optional<db::TransactionRecord> header;
    if (!transactions.findHeaderById(group_id, transaction_id, true, &header, &db_error)) {
        setDatabaseError(error, "锁定交易", db_error);
        return false;
    }
    if (!header.has_value()) {
        setError(error, 404, 1005, "交易不存在");
        return false;
    }
    if (header->created_by != user_id && actor->role != 0) {
        setError(error, 403, 1004, "仅创建者或组长可以修改交易");
        return false;
    }
    db::CategoryDao categories(lease.connection());
    if (!validateLedgerRelations(groups, categories, group_id, input, error, &db_error)) {
        return false;
    }
    header->payer_id = input.payer_id;
    header->type = input.type;
    header->amount_cents = input.amount_cents;
    header->category_id = input.category_id;
    header->note = input.note;
    header->split_type = input.split_type;
    if (!transactions.updateHeader(*header, &db_error)) {
        setDatabaseError(error, "更新交易", db_error);
        return false;
    }
    if (!transactions.deleteParticipants(transaction_id, &db_error)) {
        setDatabaseError(error, "清理旧分摊", db_error);
        return false;
    }
    if (!insertShares(transactions, transaction_id, input, shares, error, &db_error)) {
        return false;
    }
    if (!loadDetail(transactions, group_id, transaction_id, detail, error, &db_error)) {
        return false;
    }
    if (!transaction.commit(&db_error)) {
        setDatabaseError(error, "提交修改交易事务", db_error);
        return false;
    }
    return true;
}

bool LedgerService::deleteTransaction(std::uint64_t user_id, std::uint64_t group_id,
                                      std::uint64_t transaction_id, ServiceError* error) {
    std::string db_error;
    auto lease = pool_.acquire(-1, &db_error);
    if (!lease) {
        setDatabaseError(error, "获取数据库连接", db_error);
        return false;
    }
    db::TransactionGuard transaction(lease.connection(), &db_error);
    if (!transaction.active()) {
        setDatabaseError(error, "开启删除交易事务", db_error);
        return false;
    }
    db::GroupDao groups(lease.connection());
    std::optional<db::GroupRecord> group;
    std::optional<db::MembershipRecord> actor;
    if (!lockActiveGroupAndMember(groups, group_id, user_id, &group, &actor, error,
                                  &db_error)) {
        return false;
    }
    db::TransactionDao transactions(lease.connection());
    std::optional<db::TransactionRecord> header;
    if (!transactions.findHeaderById(group_id, transaction_id, true, &header, &db_error)) {
        setDatabaseError(error, "锁定交易", db_error);
        return false;
    }
    if (!header.has_value()) {
        setError(error, 404, 1005, "交易不存在");
        return false;
    }
    if (header->created_by != user_id && actor->role != 0) {
        setError(error, 403, 1004, "仅创建者或组长可以删除交易");
        return false;
    }
    if (!transactions.softDelete(transaction_id, &db_error)) {
        setDatabaseError(error, "软删除交易", db_error);
        return false;
    }
    if (!transaction.commit(&db_error)) {
        setDatabaseError(error, "提交删除交易事务", db_error);
        return false;
    }
    return true;
}

bool LedgerService::listCategories(std::uint64_t user_id,
                                   std::optional<std::uint64_t> group_id,
                                   std::vector<db::CategoryRecord>* categories,
                                   ServiceError* error) {
    if (categories == nullptr) {
        setError(error, 500, 2001, "服务器内部错误");
        return false;
    }
    std::string db_error;
    auto lease = pool_.acquire(-1, &db_error);
    if (!lease) {
        setDatabaseError(error, "获取数据库连接", db_error);
        return false;
    }
    if (group_id.has_value()) {
        db::GroupDao groups(lease.connection());
        std::optional<db::GroupRecord> group;
        std::optional<db::MembershipRecord> member;
        if (!findActiveGroupAndMember(groups, *group_id, user_id, &group, &member, error,
                                      &db_error)) {
            return false;
        }
    }
    db::CategoryDao dao(lease.connection());
    if (!dao.listVisible(group_id, categories, &db_error)) {
        setDatabaseError(error, "查询分类列表", db_error);
        return false;
    }
    return true;
}

bool LedgerService::createCategory(std::uint64_t user_id, std::uint64_t group_id,
                                   const std::string& name, const std::string& icon, int sort,
                                   db::CategoryRecord* category, ServiceError* error) {
    if (category == nullptr) {
        setError(error, 500, 2001, "服务器内部错误");
        return false;
    }
    const std::string normalized_name = util::trim(name);
    const std::string normalized_icon = util::trim(icon);
    if (normalized_name.empty() || !validCategoryText(normalized_name, 50)) {
        setError(error, 400, 1001, "分类名须为 1-50 个字符且不能包含控制字符");
        return false;
    }
    if (!validCategoryText(normalized_icon, 50)) {
        setError(error, 400, 1001, "图标标识不能超过 50 个字符且不能包含控制字符");
        return false;
    }
    if (sort < -100000 || sort > 100000) {
        setError(error, 400, 1001, "sort 超出范围");
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
        setDatabaseError(error, "开启新增分类事务", db_error);
        return false;
    }
    db::GroupDao groups(lease.connection());
    std::optional<db::GroupRecord> group;
    std::optional<db::MembershipRecord> member;
    if (!lockActiveGroupAndMember(groups, group_id, user_id, &group, &member, error,
                                  &db_error)) {
        return false;
    }
    db::CategoryDao dao(lease.connection());
    db::CategoryRecord created;
    created.group_id = group_id;
    created.name = normalized_name;
    created.icon = normalized_icon;
    created.sort = sort;
    bool duplicate = false;
    if (!dao.insert(created, &db_error, &duplicate)) {
        if (duplicate) {
            setError(error, 409, 1016, "分类名已存在");
        } else {
            setDatabaseError(error, "新增分类", db_error);
        }
        return false;
    }
    if (!transaction.commit(&db_error)) {
        setDatabaseError(error, "提交新增分类事务", db_error);
        return false;
    }
    *category = std::move(created);
    return true;
}

bool LedgerService::deleteCategory(std::uint64_t user_id, std::uint64_t group_id,
                                   std::uint64_t category_id, ServiceError* error) {
    std::string db_error;
    auto lease = pool_.acquire(-1, &db_error);
    if (!lease) {
        setDatabaseError(error, "获取数据库连接", db_error);
        return false;
    }
    db::TransactionGuard transaction(lease.connection(), &db_error);
    if (!transaction.active()) {
        setDatabaseError(error, "开启删除分类事务", db_error);
        return false;
    }
    db::GroupDao groups(lease.connection());
    std::optional<db::GroupRecord> group;
    std::optional<db::MembershipRecord> member;
    if (!lockActiveGroupAndMember(groups, group_id, user_id, &group, &member, error,
                                  &db_error)) {
        return false;
    }
    db::CategoryDao categories(lease.connection());
    std::optional<db::CategoryRecord> category;
    if (!categories.findById(category_id, &category, &db_error)) {
        setDatabaseError(error, "读取待删除分类", db_error);
        return false;
    }
    if (!category.has_value()) {
        setError(error, 404, 1005, "分类不存在");
        return false;
    }
    if (category->group_id == 0) {
        setError(error, 403, 1004, "系统预置分类不可删除");
        return false;
    }
    if (category->group_id != group_id) {
        setError(error, 404, 1005, "分类不属于该组");
        return false;
    }
    std::uint64_t count = 0;
    if (!categories.countTransactions(group_id, category_id, &count, &db_error)) {
        setDatabaseError(error, "统计分类下交易", db_error);
        return false;
    }
    if (count != 0) {
        setError(error, 409, 1014, "分类下仍有交易，无法删除");
        return false;
    }
    if (!categories.remove(category_id, group_id, &db_error)) {
        setDatabaseError(error, "删除分类", db_error);
        return false;
    }
    if (!transaction.commit(&db_error)) {
        setDatabaseError(error, "提交删除分类事务", db_error);
        return false;
    }
    return true;
}

}  // namespace service
}  // namespace cofinance
