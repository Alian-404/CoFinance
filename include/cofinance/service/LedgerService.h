#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "cofinance/db/CategoryDao.h"
#include "cofinance/db/TransactionDao.h"
#include "cofinance/service/UserService.h"
#include "cofinance/util/Common.h"

namespace cofinance {
namespace db {
class ConnectionPool;
}
namespace service {

struct ParticipantInput {
    std::uint64_t user_id = 0;
    std::optional<std::int64_t> share_cents;
};

struct TransactionInput {
    int type = 0;
    std::int64_t amount_cents = 0;
    std::uint64_t category_id = 0;
    std::string note;
    std::uint64_t payer_id = 0;
    std::vector<ParticipantInput> participants;
    int split_type = 0;
    std::optional<std::string> client_token;
};

struct TransactionFilterInput {
    std::optional<std::uint64_t> cursor;
    std::optional<std::uint64_t> category_id;
    std::optional<std::uint64_t> payer_id;
    std::optional<std::string> from;
    std::optional<std::string> to;
    std::uint64_t limit = 20;
};

struct TransactionPage {
    std::vector<db::TransactionRecord> items;
    std::optional<std::uint64_t> next_cursor;
};

class LedgerService : private NonCopyable {
public:
    explicit LedgerService(db::ConnectionPool& pool);

    bool createTransaction(std::uint64_t user_id, std::uint64_t group_id,
                           const TransactionInput& input, db::TransactionDetail* detail,
                           ServiceError* error);
    bool listTransactions(std::uint64_t user_id, std::uint64_t group_id,
                          const TransactionFilterInput& filter, TransactionPage* page,
                          ServiceError* error);
    bool getTransaction(std::uint64_t user_id, std::uint64_t group_id,
                        std::uint64_t transaction_id, db::TransactionDetail* detail,
                        ServiceError* error);
    bool updateTransaction(std::uint64_t user_id, std::uint64_t group_id,
                           std::uint64_t transaction_id, const TransactionInput& input,
                           db::TransactionDetail* detail, ServiceError* error);
    bool deleteTransaction(std::uint64_t user_id, std::uint64_t group_id,
                           std::uint64_t transaction_id, ServiceError* error);

    bool listCategories(std::uint64_t user_id, std::optional<std::uint64_t> group_id,
                        std::vector<db::CategoryRecord>* categories, ServiceError* error);
    bool createCategory(std::uint64_t user_id, std::uint64_t group_id, const std::string& name,
                        const std::string& icon, int sort, db::CategoryRecord* category,
                        ServiceError* error);
    bool deleteCategory(std::uint64_t user_id, std::uint64_t group_id,
                        std::uint64_t category_id, ServiceError* error);

private:
    db::ConnectionPool& pool_;
};

}  // namespace service
}  // namespace cofinance
