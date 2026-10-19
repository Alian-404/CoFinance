#include "cofinance/service/GroupService.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cofinance/db/ConnectionPool.h"
#include "cofinance/db/TransactionGuard.h"
#include "cofinance/db/UserDao.h"
#include "cofinance/util/Logger.h"

namespace cofinance {
namespace service {
namespace {

constexpr std::uint64_t kMaxGroupMembers = 20;

void setError(ServiceError* error, int status, int code, std::string message) {
    if (error != nullptr) {
        error->http_status = status;
        error->code = code;
        error->message = std::move(message);
    }
}

void setDatabaseError(ServiceError* error, const char* operation, const std::string& detail) {
    LOG_ERROR("%s 失败：%s", operation, detail.c_str());
    setError(error, 500, 2001, "服务器内部错误");
}

bool hasControlCharacters(const std::string& text) {
    for (char c : text) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 0x20 || uc == 0x7f) {
            return true;
        }
    }
    return false;
}

bool normalizeGroupName(const std::string& raw, std::string* normalized) {
    *normalized = util::trim(raw);
    bool valid_utf8 = false;
    const std::size_t length = util::utf8Length(*normalized, &valid_utf8);
    return valid_utf8 && length >= 1 && length <= 50 && !hasControlCharacters(*normalized);
}

bool lockActiveGroup(db::GroupDao& groups, std::uint64_t group_id,
                     std::optional<db::GroupRecord>* group, ServiceError* error,
                     std::string* db_error) {
    if (!groups.lockGroup(group_id, group, db_error)) {
        setDatabaseError(error, "锁定财务组", *db_error);
        return false;
    }
    if (!group->has_value() || group->value().status != 0) {
        setError(error, 404, 1005, "组不存在或已解散");
        return false;
    }
    return true;
}

bool requireJoinedMember(db::GroupDao& groups, std::uint64_t group_id, std::uint64_t user_id,
                         std::optional<db::MembershipRecord>* member, ServiceError* error,
                         std::string* db_error) {
    if (!groups.lockMembership(group_id, user_id, member, db_error)) {
        setDatabaseError(error, "锁定组成员", *db_error);
        return false;
    }
    if (!member->has_value() || member->value().status != 1) {
        setError(error, 403, 1004, "无权访问该组");
        return false;
    }
    return true;
}

bool requireOwner(db::GroupDao& groups, std::uint64_t group_id, std::uint64_t user_id,
                  std::optional<db::MembershipRecord>* member, ServiceError* error,
                  std::string* db_error) {
    if (!requireJoinedMember(groups, group_id, user_id, member, error, db_error)) {
        return false;
    }
    if (member->value().role != 0) {
        setError(error, 403, 1004, "仅组长可以执行该操作");
        return false;
    }
    return true;
}

}  // namespace

GroupService::GroupService(db::ConnectionPool& pool) : pool_(pool) {}

bool GroupService::createGroup(std::uint64_t user_id, const std::string& name,
                               db::GroupRecord* group, ServiceError* error) {
    if (group == nullptr) {
        setError(error, 500, 2001, "服务器内部错误");
        return false;
    }
    std::string normalized;
    if (!normalizeGroupName(name, &normalized)) {
        setError(error, 400, 1001, "组名须为 1-50 个字符且不能包含控制字符");
        return false;
    }

    std::string db_error;
    auto lease = pool_.acquire(-1, &db_error);
    if (!lease) {
        setDatabaseError(error, "获取数据库连接", db_error);
        return false;
    }
    db::TransactionGuard transaction(lease.connection(), &db_error);
    if (!transaction.active()) {
        setDatabaseError(error, "开启创建组事务", db_error);
        return false;
    }
    db::GroupDao groups(lease.connection());
    db::GroupRecord created;
    created.name = normalized;
    created.owner_id = user_id;
    if (!groups.insertGroup(created, &db_error)) {
        setDatabaseError(error, "创建财务组", db_error);
        return false;
    }
    std::optional<db::MembershipRecord> owner_member;
    if (!groups.insertMembership(created.id, user_id, 0, 1, 0, true, &owner_member,
                                 &db_error)) {
        setDatabaseError(error, "创建组长成员关系", db_error);
        return false;
    }
    std::optional<db::GroupRecord> stored;
    if (!groups.findGroup(created.id, &stored, &db_error)) {
        setDatabaseError(error, "读取新建组", db_error);
        return false;
    }
    if (!stored.has_value()) {
        setDatabaseError(error, "读取新建组", "组插入后无法读回");
        return false;
    }
    if (!transaction.commit(&db_error)) {
        setDatabaseError(error, "提交创建组事务", db_error);
        return false;
    }
    *group = std::move(*stored);
    return true;
}

bool GroupService::listGroups(std::uint64_t user_id,
                              std::vector<db::GroupSummaryRecord>* groups,
                              ServiceError* error) {
    if (groups == nullptr) {
        setError(error, 500, 2001, "服务器内部错误");
        return false;
    }
    std::string db_error;
    auto lease = pool_.acquire(-1, &db_error);
    if (!lease) {
        setDatabaseError(error, "获取数据库连接", db_error);
        return false;
    }
    db::GroupDao dao(lease.connection());
    if (!dao.listGroupsForUser(user_id, groups, &db_error)) {
        setDatabaseError(error, "查询我的组", db_error);
        return false;
    }
    return true;
}

bool GroupService::getGroup(std::uint64_t user_id, std::uint64_t group_id,
                            db::GroupRecord* group, int* role, std::uint64_t* member_count,
                            ServiceError* error) {
    if (group == nullptr || role == nullptr || member_count == nullptr) {
        setError(error, 500, 2001, "服务器内部错误");
        return false;
    }
    std::string db_error;
    auto lease = pool_.acquire(-1, &db_error);
    if (!lease) {
        setDatabaseError(error, "获取数据库连接", db_error);
        return false;
    }
    db::GroupDao groups(lease.connection());
    std::optional<db::GroupRecord> found;
    if (!groups.findGroup(group_id, &found, &db_error)) {
        setDatabaseError(error, "读取组详情", db_error);
        return false;
    }
    if (!found.has_value() || found->status != 0) {
        setError(error, 404, 1005, "组不存在或已解散");
        return false;
    }
    std::optional<db::MembershipRecord> member;
    if (!groups.findMembership(group_id, user_id, &member, &db_error)) {
        setDatabaseError(error, "读取组成员身份", db_error);
        return false;
    }
    if (!member.has_value() || member->status != 1) {
        setError(error, 403, 1004, "无权访问该组");
        return false;
    }
    std::uint64_t count = 0;
    if (!groups.countJoinedMembers(group_id, &count, &db_error)) {
        setDatabaseError(error, "统计组正式成员", db_error);
        return false;
    }
    *group = std::move(*found);
    *role = member->role;
    *member_count = count;
    return true;
}

bool GroupService::renameGroup(std::uint64_t user_id, std::uint64_t group_id,
                               const std::string& name, db::GroupRecord* group,
                               ServiceError* error) {
    if (group == nullptr) {
        setError(error, 500, 2001, "服务器内部错误");
        return false;
    }
    std::string normalized;
    if (!normalizeGroupName(name, &normalized)) {
        setError(error, 400, 1001, "组名须为 1-50 个字符且不能包含控制字符");
        return false;
    }
    std::string db_error;
    auto lease = pool_.acquire(-1, &db_error);
    if (!lease) {
        setDatabaseError(error, "获取数据库连接", db_error);
        return false;
    }
    db::TransactionGuard transaction(lease.connection(), &db_error);
    if (!transaction.active()) {
        setDatabaseError(error, "开启改组名事务", db_error);
        return false;
    }
    db::GroupDao groups(lease.connection());
    std::optional<db::GroupRecord> found;
    if (!lockActiveGroup(groups, group_id, &found, error, &db_error)) {
        return false;
    }
    std::optional<db::MembershipRecord> member;
    if (!requireOwner(groups, group_id, user_id, &member, error, &db_error)) {
        return false;
    }
    if (!groups.updateGroupName(group_id, normalized, &db_error)) {
        setDatabaseError(error, "修改组名", db_error);
        return false;
    }
    if (!groups.findGroup(group_id, &found, &db_error)) {
        setDatabaseError(error, "读取修改后的组", db_error);
        return false;
    }
    if (!transaction.commit(&db_error)) {
        setDatabaseError(error, "提交改组名事务", db_error);
        return false;
    }
    *group = std::move(*found);
    return true;
}

bool GroupService::dissolveGroup(std::uint64_t user_id, std::uint64_t group_id,
                                 ServiceError* error) {
    std::string db_error;
    auto lease = pool_.acquire(-1, &db_error);
    if (!lease) {
        setDatabaseError(error, "获取数据库连接", db_error);
        return false;
    }
    db::TransactionGuard transaction(lease.connection(), &db_error);
    if (!transaction.active()) {
        setDatabaseError(error, "开启解散组事务", db_error);
        return false;
    }
    db::GroupDao groups(lease.connection());
    std::optional<db::GroupRecord> found;
    if (!lockActiveGroup(groups, group_id, &found, error, &db_error)) {
        return false;
    }
    std::optional<db::MembershipRecord> member;
    if (!requireOwner(groups, group_id, user_id, &member, error, &db_error)) {
        return false;
    }
    if (!groups.dissolveGroup(group_id, &db_error)) {
        setDatabaseError(error, "解散财务组", db_error);
        return false;
    }
    if (!transaction.commit(&db_error)) {
        setDatabaseError(error, "提交解散组事务", db_error);
        return false;
    }
    return true;
}

bool GroupService::listMembers(std::uint64_t user_id, std::uint64_t group_id,
                               std::vector<db::MembershipRecord>* members,
                               ServiceError* error) {
    if (members == nullptr) {
        setError(error, 500, 2001, "服务器内部错误");
        return false;
    }
    std::string db_error;
    auto lease = pool_.acquire(-1, &db_error);
    if (!lease) {
        setDatabaseError(error, "获取数据库连接", db_error);
        return false;
    }
    db::GroupDao groups(lease.connection());
    std::optional<db::GroupRecord> group;
    if (!groups.findGroup(group_id, &group, &db_error)) {
        setDatabaseError(error, "读取组信息", db_error);
        return false;
    }
    if (!group.has_value() || group->status != 0) {
        setError(error, 404, 1005, "组不存在或已解散");
        return false;
    }
    std::optional<db::MembershipRecord> member;
    if (!groups.findMembership(group_id, user_id, &member, &db_error)) {
        setDatabaseError(error, "读取组成员身份", db_error);
        return false;
    }
    if (!member.has_value() || member->status != 1) {
        setError(error, 403, 1004, "无权访问该组");
        return false;
    }
    if (!groups.listJoinedMembers(group_id, members, &db_error)) {
        setDatabaseError(error, "查询组成员列表", db_error);
        return false;
    }
    return true;
}

bool GroupService::inviteMember(std::uint64_t user_id, std::uint64_t group_id,
                                const std::string& username, db::InviteRecord* invite,
                                ServiceError* error) {
    if (invite == nullptr) {
        setError(error, 500, 2001, "服务器内部错误");
        return false;
    }
    const std::string target_name = util::trim(username);
    if (target_name.empty()) {
        setError(error, 400, 1001, "用户名不能为空");
        return false;
    }
    std::string db_error;
    auto lease = pool_.acquire(-1, &db_error);
    if (!lease) {
        setDatabaseError(error, "获取数据库连接", db_error);
        return false;
    }
    db::TransactionGuard transaction(lease.connection(), &db_error);
    if (!transaction.active()) {
        setDatabaseError(error, "开启邀请事务", db_error);
        return false;
    }
    db::GroupDao groups(lease.connection());
    std::optional<db::GroupRecord> group;
    if (!lockActiveGroup(groups, group_id, &group, error, &db_error)) {
        return false;
    }
    std::optional<db::MembershipRecord> inviter;
    if (!requireJoinedMember(groups, group_id, user_id, &inviter, error, &db_error)) {
        return false;
    }
    db::UserDao users(lease.connection());
    std::optional<db::User> target;
    if (!users.lockActiveByUsername(target_name, &target, &db_error)) {
        setDatabaseError(error, "锁定被邀请用户", db_error);
        return false;
    }
    if (!target.has_value()) {
        setError(error, 404, 1005, "用户不存在");
        return false;
    }
    if (target->id == user_id) {
        setError(error, 400, 1011, "不能邀请自己");
        return false;
    }

    std::optional<db::MembershipRecord> existing;
    if (!groups.lockMembership(group_id, target->id, &existing, &db_error)) {
        setDatabaseError(error, "查询目标成员状态", db_error);
        return false;
    }
    if (existing.has_value() && (existing->status == 0 || existing->status == 1)) {
        setError(error, 409, 1008, "该用户已在组内或已有待处理邀请");
        return false;
    }
    std::uint64_t occupied = 0;
    if (!groups.countOccupiedSlots(group_id, &occupied, &db_error)) {
        setDatabaseError(error, "统计组容量", db_error);
        return false;
    }
    if (occupied >= kMaxGroupMembers) {
        setError(error, 409, 1012, "组成员数量已达上限");
        return false;
    }

    std::uint64_t member_id = 0;
    if (existing.has_value()) {
        member_id = existing->id;
        if (!groups.updateInvitation(member_id, user_id, &db_error)) {
            setDatabaseError(error, "重新发送邀请", db_error);
            return false;
        }
    } else {
        std::optional<db::MembershipRecord> inserted;
        if (!groups.insertMembership(group_id, target->id, 1, 0, user_id, false, &inserted,
                                     &db_error)) {
            setDatabaseError(error, "创建成员邀请", db_error);
            return false;
        }
        if (!inserted.has_value()) {
            setDatabaseError(error, "创建成员邀请", "邀请插入后无法读回");
            return false;
        }
        member_id = inserted->id;
    }

    std::optional<db::InviteRecord> stored;
    if (!groups.findInvite(member_id, &stored, &db_error)) {
        setDatabaseError(error, "读取新建邀请", db_error);
        return false;
    }
    if (!stored.has_value()) {
        setDatabaseError(error, "读取新建邀请", "邀请插入后无法读回");
        return false;
    }
    if (!transaction.commit(&db_error)) {
        setDatabaseError(error, "提交邀请事务", db_error);
        return false;
    }
    *invite = std::move(*stored);
    return true;
}

bool GroupService::removeMember(std::uint64_t user_id, std::uint64_t group_id,
                                std::uint64_t target_user_id, ServiceError* error) {
    std::string db_error;
    auto lease = pool_.acquire(-1, &db_error);
    if (!lease) {
        setDatabaseError(error, "获取数据库连接", db_error);
        return false;
    }
    db::TransactionGuard transaction(lease.connection(), &db_error);
    if (!transaction.active()) {
        setDatabaseError(error, "开启移除成员事务", db_error);
        return false;
    }
    db::GroupDao groups(lease.connection());
    std::optional<db::GroupRecord> group;
    if (!lockActiveGroup(groups, group_id, &group, error, &db_error)) {
        return false;
    }
    std::optional<db::MembershipRecord> owner;
    if (!requireOwner(groups, group_id, user_id, &owner, error, &db_error)) {
        return false;
    }
    std::optional<db::MembershipRecord> target;
    if (!groups.lockMembership(group_id, target_user_id, &target, &db_error)) {
        setDatabaseError(error, "锁定待移除成员", db_error);
        return false;
    }
    if (!target.has_value() || target->status != 1 || target->role == 0 ||
        target_user_id == user_id) {
        setError(error, 400, 1017, "目标用户不是可移除的组员");
        return false;
    }
    if (!groups.setMembershipStatus(target->id, 4, &db_error)) {
        setDatabaseError(error, "移除组成员", db_error);
        return false;
    }
    if (!transaction.commit(&db_error)) {
        setDatabaseError(error, "提交移除成员事务", db_error);
        return false;
    }
    return true;
}

bool GroupService::leaveGroup(std::uint64_t user_id, std::uint64_t group_id,
                              ServiceError* error) {
    std::string db_error;
    auto lease = pool_.acquire(-1, &db_error);
    if (!lease) {
        setDatabaseError(error, "获取数据库连接", db_error);
        return false;
    }
    db::TransactionGuard transaction(lease.connection(), &db_error);
    if (!transaction.active()) {
        setDatabaseError(error, "开启退出组事务", db_error);
        return false;
    }
    db::GroupDao groups(lease.connection());
    std::optional<db::GroupRecord> group;
    if (!lockActiveGroup(groups, group_id, &group, error, &db_error)) {
        return false;
    }
    std::optional<db::MembershipRecord> member;
    if (!requireJoinedMember(groups, group_id, user_id, &member, error, &db_error)) {
        return false;
    }
    if (member->role == 0) {
        setError(error, 400, 1017, "组长必须先转让或解散组");
        return false;
    }
    if (!groups.setMembershipStatus(member->id, 3, &db_error)) {
        setDatabaseError(error, "退出财务组", db_error);
        return false;
    }
    if (!transaction.commit(&db_error)) {
        setDatabaseError(error, "提交退出组事务", db_error);
        return false;
    }
    return true;
}

bool GroupService::listInvites(std::uint64_t user_id, std::vector<db::InviteRecord>* invites,
                               ServiceError* error) {
    if (invites == nullptr) {
        setError(error, 500, 2001, "服务器内部错误");
        return false;
    }
    std::string db_error;
    auto lease = pool_.acquire(-1, &db_error);
    if (!lease) {
        setDatabaseError(error, "获取数据库连接", db_error);
        return false;
    }
    db::TransactionGuard transaction(lease.connection(), &db_error);
    if (!transaction.active()) {
        setDatabaseError(error, "开启查询邀请事务", db_error);
        return false;
    }
    db::GroupDao groups(lease.connection());
    std::uint64_t expired = 0;
    if (!groups.expirePendingInvites(user_id, &expired, &db_error)) {
        setDatabaseError(error, "标记过期邀请", db_error);
        return false;
    }
    if (!groups.listPendingInvites(user_id, invites, &db_error)) {
        setDatabaseError(error, "查询待处理邀请", db_error);
        return false;
    }
    if (!transaction.commit(&db_error)) {
        setDatabaseError(error, "提交查询邀请事务", db_error);
        return false;
    }
    return true;
}

bool GroupService::acceptInvite(std::uint64_t user_id, std::uint64_t invite_id,
                                std::uint64_t* group_id, ServiceError* error) {
    if (group_id == nullptr) {
        setError(error, 500, 2001, "服务器内部错误");
        return false;
    }
    std::string db_error;
    auto lease = pool_.acquire(-1, &db_error);
    if (!lease) {
        setDatabaseError(error, "获取数据库连接", db_error);
        return false;
    }
    db::TransactionGuard transaction(lease.connection(), &db_error);
    if (!transaction.active()) {
        setDatabaseError(error, "开启接受邀请事务", db_error);
        return false;
    }
    db::GroupDao groups(lease.connection());
    std::optional<db::InviteRecord> hint;
    if (!groups.findInvite(invite_id, &hint, &db_error)) {
        setDatabaseError(error, "读取邀请", db_error);
        return false;
    }
    if (!hint.has_value() || hint->user_id != user_id) {
        setError(error, 409, 1009, "邀请不存在或已处理");
        return false;
    }
    std::optional<db::GroupRecord> group;
    if (!lockActiveGroup(groups, hint->group_id, &group, error, &db_error)) {
        setError(error, 409, 1009, "邀请所属组不存在或已解散");
        return false;
    }
    std::optional<db::InviteRecord> locked;
    if (!groups.lockInvite(invite_id, &locked, &db_error)) {
        setDatabaseError(error, "锁定邀请", db_error);
        return false;
    }
    if (!locked.has_value() || locked->user_id != user_id || locked->status != 0) {
        setError(error, 409, 1009, "邀请不存在或已处理");
        return false;
    }
    if (locked->expired) {
        if (!groups.setMembershipStatus(locked->id, 5, &db_error)) {
            setDatabaseError(error, "标记过期邀请", db_error);
            return false;
        }
        if (!transaction.commit(&db_error)) {
            setDatabaseError(error, "提交过期邀请事务", db_error);
            return false;
        }
        setError(error, 409, 1009, "邀请已过期");
        return false;
    }
    std::uint64_t occupied = 0;
    if (!groups.countOccupiedSlots(locked->group_id, &occupied, &db_error)) {
        setDatabaseError(error, "统计组容量", db_error);
        return false;
    }
    if (occupied > kMaxGroupMembers) {
        setError(error, 409, 1012, "组成员数量已达上限");
        return false;
    }
    if (!groups.acceptMembership(locked->id, &db_error)) {
        setDatabaseError(error, "接受邀请", db_error);
        return false;
    }
    if (!transaction.commit(&db_error)) {
        setDatabaseError(error, "提交接受邀请事务", db_error);
        return false;
    }
    *group_id = locked->group_id;
    return true;
}

bool GroupService::rejectInvite(std::uint64_t user_id, std::uint64_t invite_id,
                                ServiceError* error) {
    std::string db_error;
    auto lease = pool_.acquire(-1, &db_error);
    if (!lease) {
        setDatabaseError(error, "获取数据库连接", db_error);
        return false;
    }
    db::TransactionGuard transaction(lease.connection(), &db_error);
    if (!transaction.active()) {
        setDatabaseError(error, "开启拒绝邀请事务", db_error);
        return false;
    }
    db::GroupDao groups(lease.connection());
    std::optional<db::InviteRecord> hint;
    if (!groups.findInvite(invite_id, &hint, &db_error)) {
        setDatabaseError(error, "读取邀请", db_error);
        return false;
    }
    if (!hint.has_value() || hint->user_id != user_id) {
        setError(error, 409, 1009, "邀请不存在或已处理");
        return false;
    }
    std::optional<db::GroupRecord> group;
    if (!lockActiveGroup(groups, hint->group_id, &group, error, &db_error)) {
        setError(error, 409, 1009, "邀请所属组不存在或已解散");
        return false;
    }
    std::optional<db::InviteRecord> locked;
    if (!groups.lockInvite(invite_id, &locked, &db_error)) {
        setDatabaseError(error, "锁定邀请", db_error);
        return false;
    }
    if (!locked.has_value() || locked->user_id != user_id || locked->status != 0) {
        setError(error, 409, 1009, "邀请不存在或已处理");
        return false;
    }
    if (locked->expired) {
        if (!groups.setMembershipStatus(locked->id, 5, &db_error)) {
            setDatabaseError(error, "标记过期邀请", db_error);
            return false;
        }
        if (!transaction.commit(&db_error)) {
            setDatabaseError(error, "提交过期邀请事务", db_error);
            return false;
        }
        setError(error, 409, 1009, "邀请已过期");
        return false;
    }
    if (!groups.setMembershipStatus(locked->id, 2, &db_error)) {
        setDatabaseError(error, "拒绝邀请", db_error);
        return false;
    }
    if (!transaction.commit(&db_error)) {
        setDatabaseError(error, "提交拒绝邀请事务", db_error);
        return false;
    }
    return true;
}

bool GroupService::transferOwner(std::uint64_t user_id, std::uint64_t group_id,
                                 std::uint64_t target_user_id, ServiceError* error) {
    std::string db_error;
    auto lease = pool_.acquire(-1, &db_error);
    if (!lease) {
        setDatabaseError(error, "获取数据库连接", db_error);
        return false;
    }
    db::TransactionGuard transaction(lease.connection(), &db_error);
    if (!transaction.active()) {
        setDatabaseError(error, "开启转让事务", db_error);
        return false;
    }
    db::GroupDao groups(lease.connection());
    std::optional<db::GroupRecord> group;
    if (!lockActiveGroup(groups, group_id, &group, error, &db_error)) {
        return false;
    }
    std::optional<db::MembershipRecord> owner;
    if (!requireOwner(groups, group_id, user_id, &owner, error, &db_error)) {
        return false;
    }
    if (target_user_id == user_id) {
        setError(error, 400, 1017, "目标用户已经是组长");
        return false;
    }
    db::UserDao users(lease.connection());
    std::optional<db::User> target_user;
    if (!users.lockActiveById(target_user_id, &target_user, &db_error)) {
        setDatabaseError(error, "锁定新组长", db_error);
        return false;
    }
    std::optional<db::MembershipRecord> target;
    if (!groups.lockMembership(group_id, target_user_id, &target, &db_error)) {
        setDatabaseError(error, "锁定新组长成员关系", db_error);
        return false;
    }
    if (!target_user.has_value() || !target.has_value() || target->status != 1) {
        setError(error, 400, 1017, "目标用户不是组内正式成员");
        return false;
    }
    if (!groups.setGroupOwner(group_id, target_user_id, &db_error)) {
        setDatabaseError(error, "更新组长 ID", db_error);
        return false;
    }
    if (!groups.setMembershipRole(owner->id, 1, &db_error) ||
        !groups.setMembershipRole(target->id, 0, &db_error)) {
        setDatabaseError(error, "更新组长与成员角色", db_error);
        return false;
    }
    if (!transaction.commit(&db_error)) {
        setDatabaseError(error, "提交转让事务", db_error);
        return false;
    }
    return true;
}

}  // namespace service
}  // namespace cofinance
