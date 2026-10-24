#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "cofinance/db/SettlementDao.h"
#include "cofinance/service/UserService.h"
#include "cofinance/util/Common.h"

namespace cofinance {
namespace db {
class ConnectionPool;
class MySqlConnection;
}
namespace service {

struct BalanceSummary {
    std::vector<db::BalanceRecord> items;
    std::int64_t total_balance_cents = 0;
};

struct StatsInput {
    std::optional<std::string> from;
    std::optional<std::string> to;
};

struct GroupStats {
    std::optional<std::string> from;
    std::optional<std::string> to;
    db::SettlementSummaryRecord summary;
    std::vector<db::CategoryStatRecord> categories;
    std::vector<db::MemberStatRecord> members;
};

struct SettlementSuggestion {
    std::uint64_t from_user_id = 0;
    std::string from_username;
    std::string from_nickname;
    std::uint64_t to_user_id = 0;
    std::string to_username;
    std::string to_nickname;
    std::int64_t amount_cents = 0;
};

class SettlementService : private NonCopyable {
public:
    explicit SettlementService(db::ConnectionPool& pool);

    bool getStats(std::uint64_t user_id, std::uint64_t group_id, const StatsInput& input,
                  GroupStats* stats, ServiceError* error);
    bool getBalances(std::uint64_t user_id, std::uint64_t group_id, BalanceSummary* balances,
                     ServiceError* error);
    bool suggestSettlements(std::uint64_t user_id, std::uint64_t group_id,
                            std::vector<SettlementSuggestion>* suggestions, ServiceError* error);
    bool confirmSettlement(std::uint64_t user_id, std::uint64_t group_id,
                           std::uint64_t from_user_id, std::uint64_t to_user_id,
                           std::int64_t amount_cents, db::SettlementRecord* settlement,
                           ServiceError* error);
    bool listSettlements(std::uint64_t user_id, std::uint64_t group_id,
                         std::vector<db::SettlementRecord>* settlements, ServiceError* error);

    // GroupService 在退组、移除和解散时复用同一套余额口径；调用方必须已持有组行锁。
    static bool calculateBalances(db::MySqlConnection& connection, std::uint64_t group_id,
                                  std::vector<db::BalanceRecord>* balances, std::string* db_error);

private:
    db::ConnectionPool& pool_;
};

}  // namespace service
}  // namespace cofinance
