#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "cofinance/util/Common.h"

namespace cofinance {
namespace db {

class MySqlConnection;

struct SessionRecord {
    std::uint64_t user_id = 0;
    bool expired = false;
    bool needs_renewal = false;
};

class SessionDao : private NonCopyable {
public:
    explicit SessionDao(MySqlConnection& connection);

    bool create(std::uint64_t user_id, const std::string& token_hash, std::string* err);
    bool find(const std::string& token_hash, std::optional<SessionRecord>* session,
              std::string* err);
    bool touch(const std::string& token_hash, bool* touched, std::string* err);
    bool remove(const std::string& token_hash, bool* removed, std::string* err);
    bool removeAll(std::uint64_t user_id, std::string* err);
    bool removeOthers(std::uint64_t user_id, const std::string& keep_token_hash,
                      std::string* err);

private:
    MySqlConnection& connection_;
};

}  // namespace db
}  // namespace cofinance
