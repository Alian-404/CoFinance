#pragma once

#include <string>

#include "cofinance/util/Common.h"

namespace cofinance {
namespace db {

class MySqlConnection;

// 事务作用域守卫：构造时 START TRANSACTION，正常路径显式 commit()，
// 任何提前 return / 异常导致析构时仍在活动状态，就自动 ROLLBACK。
class TransactionGuard : private NonCopyable {
public:
    explicit TransactionGuard(MySqlConnection& connection, std::string* err = nullptr);
    ~TransactionGuard();

    bool active() const;
    bool commit(std::string* err);
    void rollback();

private:
    MySqlConnection* connection_ = nullptr;
    bool active_ = false;
};

}  // namespace db
}  // namespace cofinance
