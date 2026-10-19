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

struct TransactionRecord {
    std::uint64_t id = 0;
    std::uint64_t group_id = 0;
    std::uint64_t payer_id = 0;
    int type = 0;
    std::int64_t amount_cents = 0;
    std::string currency = "CNY";
    std::uint64_t category_id = 0;
    std::string note;
    int split_type = 0;
    std::optional<std::string> client_token;
    std::uint64_t created_by = 0;
    std::string created_at;
    std::string updated_at;
    std::string category_name;
    std::string payer_nickname;
    bool deleted = false;
};

struct ParticipantRecord {
    std::uint64_t id = 0;
    std::uint64_t transaction_id = 0;
    std::uint64_t user_id = 0;
    std::int64_t share_cents = 0;
    std::string username;
    std::string nickname;
    std::string avatar_url;
};

struct TransactionDetail {
    TransactionRecord transaction;
    std::vector<ParticipantRecord> participants;
};

struct TransactionFilter {
    std::optional<std::uint64_t> cursor;
    std::optional<std::uint64_t> category_id;
    std::optional<std::uint64_t> payer_id;
    std::optional<std::string> from;
    std::optional<std::string> to;
    std::uint64_t limit = 20;
};

class TransactionDao : private NonCopyable {
public:
    explicit TransactionDao(MySqlConnection& connection);

    bool insert(TransactionRecord& transaction, std::string* err, bool* duplicate = nullptr);
    bool updateHeader(const TransactionRecord& transaction, std::string* err);
    bool softDelete(std::uint64_t id, std::string* err);
    bool findHeaderById(std::uint64_t group_id, std::uint64_t transaction_id, bool for_update,
                        std::optional<TransactionRecord>* transaction, std::string* err);
    bool findByCreatorToken(std::uint64_t created_by, const std::string& client_token,
                            std::optional<TransactionRecord>* transaction, std::string* err);
    bool list(std::uint64_t group_id, const TransactionFilter& filter,
              std::vector<TransactionRecord>* transactions, std::string* err);

    bool listParticipants(std::uint64_t transaction_id,
                          std::vector<ParticipantRecord>* participants, std::string* err);
    bool insertParticipant(std::uint64_t transaction_id, std::uint64_t user_id,
                           std::int64_t share_cents, std::string* err);
    bool deleteParticipants(std::uint64_t transaction_id, std::string* err);

private:
    MySqlConnection& connection_;
};

}  // namespace db
}  // namespace cofinance
