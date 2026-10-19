#include "cofinance/db/TransactionGuard.h"

#include <string>

#include "cofinance/db/MySqlConnection.h"
#include "cofinance/util/Logger.h"

namespace cofinance {
namespace db {

TransactionGuard::TransactionGuard(MySqlConnection& connection, std::string* err)
    : connection_(&connection) {
    std::string begin_error;
    if (!connection_->execute("START TRANSACTION", &begin_error)) {
        if (err != nullptr) {
            *err = begin_error;
        }
        return;
    }
    active_ = true;
    if (err != nullptr) {
        err->clear();
    }
}

TransactionGuard::~TransactionGuard() {
    if (active_) {
        rollback();
    }
}

bool TransactionGuard::active() const {
    return active_;
}

bool TransactionGuard::commit(std::string* err) {
    if (!active_ || connection_ == nullptr) {
        if (err != nullptr) {
            *err = "事务未处于活动状态";
        }
        return false;
    }
    std::string commit_error;
    if (!connection_->execute("COMMIT", &commit_error)) {
        if (err != nullptr) {
            *err = commit_error;
        }
        return false;
    }
    active_ = false;
    if (err != nullptr) {
        err->clear();
    }
    return true;
}

void TransactionGuard::rollback() {
    if (!active_ || connection_ == nullptr) {
        return;
    }
    std::string rollback_error;
    if (!connection_->execute("ROLLBACK", &rollback_error)) {
        LOG_WARN("事务回滚失败：%s", rollback_error.c_str());
    }
    active_ = false;
}

}  // namespace db
}  // namespace cofinance
