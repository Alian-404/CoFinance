#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "cofinance/db/PreparedStatement.h"
#include "cofinance/util/Common.h"

namespace cofinance {
namespace db {

class MySqlConnection;

struct UserDelta {
    std::uint64_t user_id = 0;
    std::int64_t delta_cents = 0;
};

struct BalanceRecord {
    std::uint64_t user_id = 0;
    std::string username;
    std::string nickname;
    std::string avatar_url;
    std::int64_t transaction_balance_cents = 0;
    std::int64_t settlement_balance_cents = 0;
    std::int64_t balance_cents = 0;
};

struct SettlementSummaryRecord {
    std::int64_t total_expense_cents = 0;
    std::int64_t total_income_cents = 0;
    std::uint64_t transaction_count = 0;
};

struct CategoryStatRecord {
    std::uint64_t category_id = 0;
    int type = 0;
    std::string category_name;
    std::int64_t amount_cents = 0;
};

struct MemberStatRecord {
    std::uint64_t user_id = 0;
    std::string username;
    std::string nickname;
    std::string avatar_url;
    std::int64_t paid_expense_cents = 0;
    std::int64_t paid_income_cents = 0;
    std::int64_t share_expense_cents = 0;
    std::int64_t share_income_cents = 0;
};

struct SettlementRecord {
    std::uint64_t id = 0;
    std::uint64_t group_id = 0;
    std::uint64_t from_user_id = 0;
    std::uint64_t to_user_id = 0;
    std::int64_t amount_cents = 0;
    int status = 0;
    std::uint64_t created_by = 0;
    std::string created_at;
    std::optional<std::string> confirmed_at;
    std::string from_username;
    std::string from_nickname;
    std::string to_username;
    std::string to_nickname;
};

// 结算与统计的数据访问。权限、净额合并、贪心建议和事务边界在 SettlementService。
class SettlementDao : private NonCopyable {
public:
    explicit SettlementDao(MySqlConnection& connection);

    bool loadTransactionDeltas(std::uint64_t group_id, std::vector<UserDelta>* deltas,
                               std::string* err);
    bool loadSettlementDeltas(std::uint64_t group_id, std::vector<UserDelta>* deltas,
                              std::string* err);

    bool summarize(std::uint64_t group_id, const std::optional<std::string>& from,
                   const std::optional<std::string>& to, SettlementSummaryRecord* summary,
                   std::string* err);
    bool listCategoryStats(std::uint64_t group_id, const std::optional<std::string>& from,
                           const std::optional<std::string>& to,
                           std::vector<CategoryStatRecord>* categories, std::string* err);
    bool listMemberStats(std::uint64_t group_id, const std::optional<std::string>& from,
                         const std::optional<std::string>& to,
                         std::vector<MemberStatRecord>* members, std::string* err);

    bool insertSettlement(SettlementRecord& settlement, std::string* err);
    bool findSettlement(std::uint64_t settlement_id, SettlementRecord* settlement,
                        std::string* err);
    bool listSettlements(std::uint64_t group_id, std::vector<SettlementRecord>* settlements,
                         std::string* err);
    bool insertSystemMessage(std::uint64_t group_id, const std::string& content,
                             std::string* err);

private:
    MySqlConnection& connection_;
};

}  // namespace db
}  // namespace cofinance
