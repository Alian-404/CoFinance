#include "cofinance/db/PreparedStatement.h"

#include <mysql/mysql.h>

#include <memory>
#include <utility>

#include "cofinance/db/MySqlConnection.h"

namespace cofinance {
namespace db {

struct PreparedStatement::Impl {
    MYSQL_STMT* statement = nullptr;
    MYSQL_RES* metadata = nullptr;

    std::vector<MYSQL_BIND> param_binds;
    std::vector<MYSQL_BIND> result_binds;
    std::vector<SqlParam> param_values;
    std::vector<unsigned long> param_lengths;
    std::unique_ptr<bool[]> param_is_null;

    std::vector<std::vector<char>> result_buffers;
    std::vector<unsigned long> result_lengths;
    std::unique_ptr<bool[]> result_is_null;

    std::string last_error;
    bool executed = false;

    void setStatementError(const char* prefix) {
        last_error = std::string(prefix) + "：" +
                     (statement == nullptr ? "预处理语句未初始化" : mysql_stmt_error(statement));
    }

    void releaseResult() {
        if (statement != nullptr && executed) {
            mysql_stmt_free_result(statement);
            executed = false;
        }
        if (metadata != nullptr) {
            mysql_free_result(metadata);
            metadata = nullptr;
        }
        result_binds.clear();
        result_buffers.clear();
        result_lengths.clear();
        result_is_null.reset();
    }
};

PreparedStatement::PreparedStatement(MySqlConnection& connection, const std::string& sql,
                                     std::string* err)
    : impl_(std::make_unique<Impl>()) {
    if (connection.nativeHandle() == nullptr) {
        impl_->last_error = "连接未建立";
    } else {
        impl_->statement = mysql_stmt_init(connection.nativeHandle());
        if (impl_->statement == nullptr) {
            impl_->last_error = "mysql_stmt_init 失败（内存不足）";
        } else if (mysql_stmt_prepare(impl_->statement, sql.data(),
                                      static_cast<unsigned long>(sql.size())) != 0) {
            impl_->setStatementError("预处理 SQL 失败");
        }
    }
    if (!valid() && err != nullptr) {
        *err = impl_->last_error;
    }
}

PreparedStatement::~PreparedStatement() {
    if (impl_ == nullptr) {
        return;
    }
    impl_->releaseResult();
    if (impl_->statement != nullptr) {
        mysql_stmt_close(impl_->statement);
        impl_->statement = nullptr;
    }
}

bool PreparedStatement::valid() const {
    return impl_ != nullptr && impl_->statement != nullptr && impl_->last_error.empty();
}

bool PreparedStatement::bind(const std::vector<SqlParam>& params, std::string* err) {
    if (!valid()) {
        if (err != nullptr) {
            *err = impl_->last_error;
        }
        return false;
    }

    const unsigned long expected = mysql_stmt_param_count(impl_->statement);
    if (expected != params.size()) {
        impl_->last_error = "参数个数不匹配：SQL 需要 " + std::to_string(expected) + " 个，实际 " +
                            std::to_string(params.size());
        if (err != nullptr) {
            *err = impl_->last_error;
        }
        return false;
    }

    impl_->param_values = params;
    impl_->param_binds.assign(params.size(), MYSQL_BIND{});
    impl_->param_lengths.assign(params.size(), 0);
    impl_->param_is_null = std::make_unique<bool[]>(params.size());

    for (std::size_t i = 0; i < params.size(); ++i) {
        MYSQL_BIND& bind = impl_->param_binds[i];
        impl_->param_is_null[i] = false;
        const SqlParam& value = impl_->param_values[i];

        if (std::holds_alternative<std::nullptr_t>(value)) {
            bind.buffer_type = MYSQL_TYPE_NULL;
            impl_->param_is_null[i] = true;
            bind.is_null = &impl_->param_is_null[i];
            continue;
        }
        if (const auto* text = std::get_if<std::string>(&value)) {
            impl_->param_lengths[i] = static_cast<unsigned long>(text->size());
            bind.buffer_type = MYSQL_TYPE_STRING;
            bind.buffer = const_cast<char*>(text->data());
            bind.buffer_length = impl_->param_lengths[i];
            bind.length = &impl_->param_lengths[i];
        } else if (const auto* signed_value = std::get_if<std::int64_t>(&value)) {
            bind.buffer_type = MYSQL_TYPE_LONGLONG;
            bind.buffer = const_cast<std::int64_t*>(signed_value);
            bind.is_unsigned = false;
        } else if (const auto* unsigned_value = std::get_if<std::uint64_t>(&value)) {
            bind.buffer_type = MYSQL_TYPE_LONGLONG;
            bind.buffer = const_cast<std::uint64_t*>(unsigned_value);
            bind.is_unsigned = true;
        } else if (const auto* double_value = std::get_if<double>(&value)) {
            bind.buffer_type = MYSQL_TYPE_DOUBLE;
            bind.buffer = const_cast<double*>(double_value);
        }
        bind.is_null = &impl_->param_is_null[i];
    }

    if (mysql_stmt_bind_param(impl_->statement, impl_->param_binds.data()) != 0) {
        impl_->setStatementError("绑定参数失败");
        if (err != nullptr) {
            *err = impl_->last_error;
        }
        return false;
    }
    impl_->last_error.clear();
    return true;
}

bool PreparedStatement::execute(std::string* err) {
    if (!valid()) {
        if (err != nullptr) {
            *err = impl_->last_error;
        }
        return false;
    }

    impl_->releaseResult();
    if (mysql_stmt_execute(impl_->statement) != 0) {
        impl_->setStatementError("执行预处理语句失败");
        if (err != nullptr) {
            *err = impl_->last_error;
        }
        return false;
    }
    impl_->executed = true;

    impl_->metadata = mysql_stmt_result_metadata(impl_->statement);
    if (impl_->metadata == nullptr) {
        if (mysql_stmt_field_count(impl_->statement) != 0) {
            impl_->setStatementError("获取结果集元数据失败");
            if (err != nullptr) {
                *err = impl_->last_error;
            }
            return false;
        }
        impl_->last_error.clear();
        return true;
    }

    const unsigned int columns = mysql_num_fields(impl_->metadata);
    impl_->result_binds.assign(columns, MYSQL_BIND{});
    impl_->result_buffers.assign(columns, std::vector<char>(256, '\0'));
    impl_->result_lengths.assign(columns, 0);
    impl_->result_is_null = std::make_unique<bool[]>(columns);

    for (unsigned int i = 0; i < columns; ++i) {
        MYSQL_BIND& bind = impl_->result_binds[i];
        impl_->result_is_null[i] = false;
        bind.buffer_type = MYSQL_TYPE_STRING;
        bind.buffer = impl_->result_buffers[i].data();
        bind.buffer_length = static_cast<unsigned long>(impl_->result_buffers[i].size());
        bind.length = &impl_->result_lengths[i];
        bind.is_null = &impl_->result_is_null[i];
    }

    if (mysql_stmt_bind_result(impl_->statement, impl_->result_binds.data()) != 0) {
        impl_->setStatementError("绑定结果集失败");
        if (err != nullptr) {
            *err = impl_->last_error;
        }
        return false;
    }
    impl_->last_error.clear();
    return true;
}

bool PreparedStatement::fetch(SqlRow* row, bool* has_row, std::string* err) {
    if (!valid() || !impl_->executed || impl_->metadata == nullptr) {
        impl_->last_error = "当前语句没有可读取的结果集";
        if (err != nullptr) {
            *err = impl_->last_error;
        }
        return false;
    }
    if (row == nullptr || has_row == nullptr) {
        impl_->last_error = "fetch 输出参数为空";
        if (err != nullptr) {
            *err = impl_->last_error;
        }
        return false;
    }

    const int result = mysql_stmt_fetch(impl_->statement);
    if (result == MYSQL_NO_DATA) {
        *has_row = false;
        row->clear();
        impl_->last_error.clear();
        return true;
    }
    if (result == 1) {
        impl_->setStatementError("读取结果行失败");
        if (err != nullptr) {
            *err = impl_->last_error;
        }
        return false;
    }

    *has_row = true;
    row->clear();
    row->reserve(impl_->result_binds.size());
    for (std::size_t i = 0; i < impl_->result_binds.size(); ++i) {
        if (impl_->result_is_null[i]) {
            row->emplace_back(std::nullopt);
            continue;
        }

        const unsigned long length = impl_->result_lengths[i];
        if (length >= impl_->result_buffers[i].size() || result == MYSQL_DATA_TRUNCATED) {
            std::vector<char> expanded(length + 1, '\0');
            MYSQL_BIND column_bind{};
            column_bind.buffer_type = MYSQL_TYPE_STRING;
            column_bind.buffer = expanded.data();
            column_bind.buffer_length = length;
            column_bind.length = &impl_->result_lengths[i];
            column_bind.is_null = &impl_->result_is_null[i];
            if (mysql_stmt_fetch_column(impl_->statement, &column_bind,
                                        static_cast<unsigned int>(i), 0) != 0) {
                impl_->setStatementError("读取超长结果列失败");
                if (err != nullptr) {
                    *err = impl_->last_error;
                }
                return false;
            }
            row->emplace_back(std::string(expanded.data(), impl_->result_lengths[i]));
        } else {
            row->emplace_back(std::string(impl_->result_buffers[i].data(), length));
        }
    }
    impl_->last_error.clear();
    return true;
}

std::uint64_t PreparedStatement::affectedRows() const {
    return impl_ == nullptr || impl_->statement == nullptr
               ? 0
               : static_cast<std::uint64_t>(mysql_stmt_affected_rows(impl_->statement));
}

std::uint64_t PreparedStatement::lastInsertId() const {
    return impl_ == nullptr || impl_->statement == nullptr
               ? 0
               : static_cast<std::uint64_t>(mysql_stmt_insert_id(impl_->statement));
}

unsigned int PreparedStatement::columnCount() const {
    return impl_ == nullptr || impl_->metadata == nullptr
               ? 0
               : mysql_num_fields(impl_->metadata);
}

unsigned int PreparedStatement::lastErrorCode() const {
    return impl_ == nullptr || impl_->statement == nullptr ? 0 : mysql_stmt_errno(impl_->statement);
}

const std::string& PreparedStatement::lastError() const {
    static const std::string kEmpty;
    return impl_ == nullptr ? kEmpty : impl_->last_error;
}

}  // namespace db
}  // namespace cofinance
