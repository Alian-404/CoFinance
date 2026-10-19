#include "cofinance/db/CategoryDao.h"

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "cofinance/db/MySqlConnection.h"

namespace cofinance {
namespace db {
namespace {

const char* const kCategoryColumns = "id, group_id, name, icon, sort ";

std::uint64_t parseUnsigned(const std::optional<std::string>& value, const char* field) {
    if (!value.has_value()) {
        throw std::runtime_error(std::string(field) + " 为 NULL");
    }
    std::size_t consumed = 0;
    const unsigned long long parsed = std::stoull(*value, &consumed);
    if (consumed != value->size()) {
        throw std::runtime_error(std::string(field) + " 不是合法整数");
    }
    return static_cast<std::uint64_t>(parsed);
}

int parseInt(const std::optional<std::string>& value, const char* field) {
    if (!value.has_value()) {
        throw std::runtime_error(std::string(field) + " 为 NULL");
    }
    std::size_t consumed = 0;
    const int parsed = std::stoi(*value, &consumed);
    if (consumed != value->size()) {
        throw std::runtime_error(std::string(field) + " 不是合法整数");
    }
    return parsed;
}

std::string requiredText(const std::optional<std::string>& value, const char* field) {
    if (!value.has_value()) {
        throw std::runtime_error(std::string(field) + " 为 NULL");
    }
    return *value;
}

CategoryRecord rowToCategory(const SqlRow& row) {
    if (row.size() != 5) {
        throw std::runtime_error("分类查询结果列数不正确");
    }
    CategoryRecord category;
    category.id = parseUnsigned(row[0], "categories.id");
    category.group_id = parseUnsigned(row[1], "categories.group_id");
    category.name = requiredText(row[2], "categories.name");
    category.icon = requiredText(row[3], "categories.icon");
    category.sort = parseInt(row[4], "categories.sort");
    return category;
}

}  // namespace

CategoryDao::CategoryDao(MySqlConnection& connection) : connection_(connection) {}

bool CategoryDao::findById(std::uint64_t id, std::optional<CategoryRecord>* category,
                           std::string* err) {
    if (category == nullptr) {
        if (err != nullptr) {
            *err = "CategoryDao 输出参数为空";
        }
        return false;
    }
    category->reset();
    const std::string sql = std::string("SELECT ") + kCategoryColumns +
                            "FROM categories WHERE id = ?";
    PreparedStatement statement(connection_, sql, err);
    if (!statement.valid() || !statement.bind({SqlParam{id}}, err) ||
        !statement.execute(err)) {
        return false;
    }
    SqlRow row;
    bool has_row = false;
    if (!statement.fetch(&row, &has_row, err)) {
        return false;
    }
    if (!has_row) {
        return true;
    }
    try {
        *category = rowToCategory(row);
    } catch (const std::exception& e) {
        if (err != nullptr) {
            *err = std::string("解析分类行失败：") + e.what();
        }
        return false;
    }
    return true;
}

bool CategoryDao::listVisible(std::optional<std::uint64_t> group_id,
                              std::vector<CategoryRecord>* categories, std::string* err) {
    if (categories == nullptr) {
        if (err != nullptr) {
            *err = "CategoryDao 输出参数为空";
        }
        return false;
    }
    categories->clear();
    const std::string sql = std::string("SELECT ") + kCategoryColumns +
                            "FROM categories WHERE group_id = 0 OR group_id = ? "
                            "ORDER BY group_id ASC, sort ASC, id ASC";
    const std::uint64_t requested_group = group_id.value_or(0);
    PreparedStatement statement(connection_, sql, err);
    if (!statement.valid() || !statement.bind({SqlParam{requested_group}}, err) ||
        !statement.execute(err)) {
        return false;
    }
    for (;;) {
        SqlRow row;
        bool has_row = false;
        if (!statement.fetch(&row, &has_row, err)) {
            return false;
        }
        if (!has_row) {
            return true;
        }
        try {
            categories->push_back(rowToCategory(row));
        } catch (const std::exception& e) {
            if (err != nullptr) {
                *err = std::string("解析分类列表失败：") + e.what();
            }
            return false;
        }
    }
}

bool CategoryDao::insert(CategoryRecord& category, std::string* err, bool* duplicate) {
    if (duplicate != nullptr) {
        *duplicate = false;
    }
    PreparedStatement statement(connection_,
                                "INSERT INTO categories (group_id, name, icon, sort) "
                                "VALUES (?, ?, ?, ?)",
                                err);
    if (!statement.valid() ||
        !statement.bind({SqlParam{category.group_id}, SqlParam{category.name},
                         SqlParam{category.icon},
                         SqlParam{static_cast<std::int64_t>(category.sort)}}, err) ||
        !statement.execute(err)) {
        if (duplicate != nullptr) {
            *duplicate = statement.lastErrorCode() == 1062;
        }
        return false;
    }
    category.id = statement.lastInsertId();
    return true;
}

bool CategoryDao::remove(std::uint64_t id, std::uint64_t group_id, std::string* err) {
    PreparedStatement statement(connection_,
                                "DELETE FROM categories WHERE id = ? AND group_id = ?", err);
    return statement.valid() &&
           statement.bind({SqlParam{id}, SqlParam{group_id}}, err) &&
           statement.execute(err) && statement.affectedRows() == 1;
}

bool CategoryDao::countTransactions(std::uint64_t group_id, std::uint64_t category_id,
                                    std::uint64_t* count, std::string* err) {
    PreparedStatement statement(connection_,
                                "SELECT COUNT(*) FROM transactions "
                                "WHERE group_id = ? AND category_id = ? AND deleted_at IS NULL",
                                err);
    if (!statement.valid() ||
        !statement.bind({SqlParam{group_id}, SqlParam{category_id}}, err) ||
        !statement.execute(err)) {
        return false;
    }
    SqlRow row;
    bool has_row = false;
    if (!statement.fetch(&row, &has_row, err)) {
        return false;
    }
    if (!has_row || row.size() != 1) {
        if (err != nullptr) {
            *err = "分类交易计数没有返回一行一列";
        }
        return false;
    }
    try {
        *count = parseUnsigned(row[0], "COUNT(*)");
    } catch (const std::exception& e) {
        if (err != nullptr) {
            *err = std::string("解析分类交易计数失败：") + e.what();
        }
        return false;
    }
    return true;
}

}  // namespace db
}  // namespace cofinance
