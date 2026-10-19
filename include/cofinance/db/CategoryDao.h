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

struct CategoryRecord {
    std::uint64_t id = 0;
    std::uint64_t group_id = 0;
    std::string name;
    std::string icon;
    int sort = 0;
};

class CategoryDao : private NonCopyable {
public:
    explicit CategoryDao(MySqlConnection& connection);

    bool findById(std::uint64_t id, std::optional<CategoryRecord>* category, std::string* err);
    bool listVisible(std::optional<std::uint64_t> group_id,
                     std::vector<CategoryRecord>* categories, std::string* err);
    bool insert(CategoryRecord& category, std::string* err, bool* duplicate = nullptr);
    bool remove(std::uint64_t id, std::uint64_t group_id, std::string* err);
    bool countTransactions(std::uint64_t group_id, std::uint64_t category_id,
                           std::uint64_t* count, std::string* err);

private:
    MySqlConnection& connection_;
};

}  // namespace db
}  // namespace cofinance
