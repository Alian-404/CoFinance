#include "cofinance/db/GroupDao.h"

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "cofinance/db/MySqlConnection.h"

namespace cofinance {
namespace db {
namespace {

const char* const kGroupColumns =
    "id, name, owner_id, status, "
    "DATE_FORMAT(created_at, '%Y-%m-%dT%H:%i:%sZ'), "
    "DATE_FORMAT(updated_at, '%Y-%m-%dT%H:%i:%sZ') ";

const char* const kMembershipColumns =
    "gm.id, gm.group_id, gm.user_id, gm.role, gm.status, gm.invited_by, "
    "DATE_FORMAT(gm.created_at, '%Y-%m-%dT%H:%i:%sZ'), "
    "DATE_FORMAT(gm.joined_at, '%Y-%m-%dT%H:%i:%sZ'), "
    "u.username, u.nickname, u.avatar_url, "
    "(gm.status = 0 AND gm.created_at < DATE_SUB(UTC_TIMESTAMP(), INTERVAL 7 DAY)) ";

const char* const kInviteColumns =
    "gm.id, gm.group_id, gm.user_id, gm.status, gm.invited_by, "
    "DATE_FORMAT(gm.created_at, '%Y-%m-%dT%H:%i:%sZ'), "
    "DATE_FORMAT(DATE_ADD(gm.created_at, INTERVAL 7 DAY), '%Y-%m-%dT%H:%i:%sZ'), "
    "g.name, COALESCE(u.username, ''), "
    "(gm.status = 0 AND gm.created_at < DATE_SUB(UTC_TIMESTAMP(), INTERVAL 7 DAY)) ";

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

GroupRecord rowToGroup(const SqlRow& row) {
    if (row.size() != 6) {
        throw std::runtime_error("组查询结果列数不正确");
    }
    GroupRecord group;
    group.id = parseUnsigned(row[0], "finance_groups.id");
    group.name = requiredText(row[1], "finance_groups.name");
    group.owner_id = parseUnsigned(row[2], "finance_groups.owner_id");
    group.status = parseInt(row[3], "finance_groups.status");
    group.created_at = requiredText(row[4], "finance_groups.created_at");
    group.updated_at = requiredText(row[5], "finance_groups.updated_at");
    return group;
}

MembershipRecord rowToMembership(const SqlRow& row) {
    if (row.size() != 12) {
        throw std::runtime_error("成员查询结果列数不正确");
    }
    MembershipRecord member;
    member.id = parseUnsigned(row[0], "group_members.id");
    member.group_id = parseUnsigned(row[1], "group_members.group_id");
    member.user_id = parseUnsigned(row[2], "group_members.user_id");
    member.role = parseInt(row[3], "group_members.role");
    member.status = parseInt(row[4], "group_members.status");
    member.invited_by = parseUnsigned(row[5], "group_members.invited_by");
    member.created_at = requiredText(row[6], "group_members.created_at");
    if (row[7].has_value()) {
        member.joined_at = *row[7];
    }
    member.username = requiredText(row[8], "users.username");
    member.nickname = requiredText(row[9], "users.nickname");
    member.avatar_url = requiredText(row[10], "users.avatar_url");
    member.invite_expired = parseInt(row[11], "invite_expired") != 0;
    return member;
}

InviteRecord rowToInvite(const SqlRow& row) {
    if (row.size() != 10) {
        throw std::runtime_error("邀请查询结果列数不正确");
    }
    InviteRecord invite;
    invite.id = parseUnsigned(row[0], "group_members.id");
    invite.group_id = parseUnsigned(row[1], "group_members.group_id");
    invite.user_id = parseUnsigned(row[2], "group_members.user_id");
    invite.status = parseInt(row[3], "group_members.status");
    invite.invited_by = parseUnsigned(row[4], "group_members.invited_by");
    invite.created_at = requiredText(row[5], "group_members.created_at");
    invite.expires_at = requiredText(row[6], "invite.expires_at");
    invite.group_name = requiredText(row[7], "finance_groups.name");
    invite.inviter_username = requiredText(row[8], "users.username");
    invite.expired = parseInt(row[9], "invite.expired") != 0;
    return invite;
}

bool readScalarCount(PreparedStatement& statement, std::uint64_t* count, std::string* err) {
    SqlRow row;
    bool has_row = false;
    if (!statement.fetch(&row, &has_row, err)) {
        return false;
    }
    if (!has_row || row.size() != 1) {
        if (err != nullptr) {
            *err = "计数查询没有返回一行一列";
        }
        return false;
    }
    try {
        *count = parseUnsigned(row[0], "COUNT(*)");
    } catch (const std::exception& e) {
        if (err != nullptr) {
            *err = std::string("解析计数结果失败：") + e.what();
        }
        return false;
    }
    return true;
}

}  // namespace

GroupDao::GroupDao(MySqlConnection& connection) : connection_(connection) {}

bool GroupDao::findGroup(std::uint64_t group_id, std::optional<GroupRecord>* group,
                         std::string* err) {
    const std::string sql =
        std::string("SELECT ") + kGroupColumns + "FROM finance_groups WHERE id = ?";
    return readOneGroup(sql, {SqlParam{group_id}}, group, err);
}

bool GroupDao::lockGroup(std::uint64_t group_id, std::optional<GroupRecord>* group,
                         std::string* err) {
    const std::string sql = std::string("SELECT ") + kGroupColumns +
                            "FROM finance_groups WHERE id = ? FOR UPDATE";
    return readOneGroup(sql, {SqlParam{group_id}}, group, err);
}

bool GroupDao::insertGroup(GroupRecord& group, std::string* err) {
    PreparedStatement statement(connection_,
                                "INSERT INTO finance_groups (name, owner_id, status) "
                                "VALUES (?, ?, 0)",
                                err);
    if (!statement.valid() ||
        !statement.bind({SqlParam{group.name}, SqlParam{group.owner_id}}, err) ||
        !statement.execute(err)) {
        return false;
    }
    group.id = statement.lastInsertId();
    group.status = 0;
    return true;
}

bool GroupDao::updateGroupName(std::uint64_t group_id, const std::string& name,
                               std::string* err) {
    PreparedStatement statement(connection_,
                                "UPDATE finance_groups SET name = ? WHERE id = ? AND status = 0",
                                err);
    return statement.valid() &&
           statement.bind({SqlParam{name}, SqlParam{group_id}}, err) &&
           statement.execute(err) && statement.affectedRows() == 1;
}

bool GroupDao::setGroupOwner(std::uint64_t group_id, std::uint64_t owner_id, std::string* err) {
    PreparedStatement statement(connection_,
                                "UPDATE finance_groups SET owner_id = ? "
                                "WHERE id = ? AND status = 0",
                                err);
    return statement.valid() &&
           statement.bind({SqlParam{owner_id}, SqlParam{group_id}}, err) &&
           statement.execute(err) && statement.affectedRows() == 1;
}

bool GroupDao::dissolveGroup(std::uint64_t group_id, std::string* err) {
    PreparedStatement statement(connection_,
                                "UPDATE finance_groups SET status = 1 "
                                "WHERE id = ? AND status = 0",
                                err);
    return statement.valid() && statement.bind({SqlParam{group_id}}, err) &&
           statement.execute(err) && statement.affectedRows() == 1;
}

bool GroupDao::listGroupsForUser(std::uint64_t user_id,
                                 std::vector<GroupSummaryRecord>* groups, std::string* err) {
    if (groups == nullptr) {
        if (err != nullptr) {
            *err = "GroupDao 输出参数为空";
        }
        return false;
    }
    groups->clear();
    const std::string sql =
        std::string("SELECT g.id, g.name, g.owner_id, g.status, ") +
        "DATE_FORMAT(g.created_at, '%Y-%m-%dT%H:%i:%sZ'), "
        "DATE_FORMAT(g.updated_at, '%Y-%m-%dT%H:%i:%sZ'), gm.role, "
        "(SELECT COUNT(*) FROM group_members c "
        " WHERE c.group_id = g.id AND c.status = 1) "
        "FROM finance_groups g JOIN group_members gm ON gm.group_id = g.id "
        "WHERE gm.user_id = ? AND gm.status = 1 AND g.status = 0 "
        "ORDER BY g.updated_at DESC, g.id DESC";
    PreparedStatement statement(connection_, sql, err);
    if (!statement.valid() || !statement.bind({SqlParam{user_id}}, err) ||
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
            if (row.size() != 8) {
                throw std::runtime_error("组列表查询结果列数不正确");
            }
            GroupSummaryRecord summary;
            summary.group.id = parseUnsigned(row[0], "finance_groups.id");
            summary.group.name = requiredText(row[1], "finance_groups.name");
            summary.group.owner_id = parseUnsigned(row[2], "finance_groups.owner_id");
            summary.group.status = parseInt(row[3], "finance_groups.status");
            summary.group.created_at = requiredText(row[4], "finance_groups.created_at");
            summary.group.updated_at = requiredText(row[5], "finance_groups.updated_at");
            summary.role = parseInt(row[6], "group_members.role");
            summary.member_count = parseUnsigned(row[7], "member_count");
            groups->push_back(std::move(summary));
        } catch (const std::exception& e) {
            if (err != nullptr) {
                *err = std::string("解析组列表失败：") + e.what();
            }
            return false;
        }
    }
}

bool GroupDao::findMembership(std::uint64_t group_id, std::uint64_t user_id,
                              std::optional<MembershipRecord>* membership, std::string* err) {
    const std::string sql = std::string("SELECT ") + kMembershipColumns +
                            "FROM group_members gm JOIN users u ON u.id = gm.user_id "
                            "WHERE gm.group_id = ? AND gm.user_id = ?";
    return readOneMembership(sql, {SqlParam{group_id}, SqlParam{user_id}}, membership, err);
}

bool GroupDao::lockMembership(std::uint64_t group_id, std::uint64_t user_id,
                              std::optional<MembershipRecord>* membership, std::string* err) {
    const std::string sql = std::string("SELECT ") + kMembershipColumns +
                            "FROM group_members gm JOIN users u ON u.id = gm.user_id "
                            "WHERE gm.group_id = ? AND gm.user_id = ? FOR UPDATE";
    return readOneMembership(sql, {SqlParam{group_id}, SqlParam{user_id}}, membership, err);
}

bool GroupDao::insertMembership(std::uint64_t group_id, std::uint64_t user_id, int role,
                                int status, std::uint64_t invited_by, bool joined,
                                std::optional<MembershipRecord>* membership, std::string* err) {
    const char* sql = joined
                          ? "INSERT INTO group_members "
                            "(group_id, user_id, role, status, invited_by, joined_at) "
                            "VALUES (?, ?, ?, ?, ?, UTC_TIMESTAMP())"
                          : "INSERT INTO group_members "
                            "(group_id, user_id, role, status, invited_by) "
                            "VALUES (?, ?, ?, ?, ?)";
    PreparedStatement statement(connection_, sql, err);
    if (!statement.valid() ||
        !statement.bind({SqlParam{group_id}, SqlParam{user_id},
                         SqlParam{static_cast<std::int64_t>(role)},
                         SqlParam{static_cast<std::int64_t>(status)}, SqlParam{invited_by}},
                        err) ||
        !statement.execute(err)) {
        return false;
    }
    return findMembership(group_id, user_id, membership, err);
}

bool GroupDao::updateInvitation(std::uint64_t member_id, std::uint64_t invited_by,
                                std::string* err) {
    PreparedStatement statement(
        connection_,
        "UPDATE group_members SET role = 1, status = 0, invited_by = ?, "
        "created_at = UTC_TIMESTAMP(), joined_at = NULL WHERE id = ?",
        err);
    return statement.valid() &&
           statement.bind({SqlParam{invited_by}, SqlParam{member_id}}, err) &&
           statement.execute(err) && statement.affectedRows() == 1;
}

bool GroupDao::acceptMembership(std::uint64_t member_id, std::string* err) {
    PreparedStatement statement(
        connection_,
        "UPDATE group_members SET status = 1, role = 1, joined_at = UTC_TIMESTAMP() "
        "WHERE id = ? AND status = 0",
        err);
    return statement.valid() && statement.bind({SqlParam{member_id}}, err) &&
           statement.execute(err) && statement.affectedRows() == 1;
}

bool GroupDao::setMembershipStatus(std::uint64_t member_id, int status, std::string* err) {
    PreparedStatement statement(connection_,
                                "UPDATE group_members SET status = ? WHERE id = ?", err);
    return statement.valid() &&
           statement.bind({SqlParam{static_cast<std::int64_t>(status)}, SqlParam{member_id}}, err) &&
           statement.execute(err) && statement.affectedRows() == 1;
}

bool GroupDao::setMembershipRole(std::uint64_t member_id, int role, std::string* err) {
    PreparedStatement statement(connection_, "UPDATE group_members SET role = ? WHERE id = ?",
                                err);
    return statement.valid() &&
           statement.bind({SqlParam{static_cast<std::int64_t>(role)}, SqlParam{member_id}}, err) &&
           statement.execute(err) && statement.affectedRows() == 1;
}

bool GroupDao::withdrawMemberships(std::uint64_t user_id, std::string* err) {
    PreparedStatement statement(connection_,
                                "UPDATE group_members SET status = 3, role = 1 "
                                "WHERE user_id = ? AND status IN (0, 1)",
                                err);
    return statement.valid() && statement.bind({SqlParam{user_id}}, err) &&
           statement.execute(err);
}

bool GroupDao::listMembershipGroupIds(std::uint64_t user_id,
                                      std::vector<std::uint64_t>* group_ids,
                                      std::string* err) {
    if (group_ids == nullptr) {
        if (err != nullptr) {
            *err = "GroupDao 输出参数为空";
        }
        return false;
    }
    group_ids->clear();
    PreparedStatement statement(connection_,
                                "SELECT group_id FROM group_members "
                                "WHERE user_id = ? AND status IN (0, 1) ORDER BY group_id",
                                err);
    if (!statement.valid() || !statement.bind({SqlParam{user_id}}, err) ||
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
            if (row.size() != 1) {
                throw std::runtime_error("成员组 ID 查询列数不正确");
            }
            group_ids->push_back(parseUnsigned(row[0], "group_members.group_id"));
        } catch (const std::exception& e) {
            if (err != nullptr) {
                *err = std::string("解析成员组 ID 失败：") + e.what();
            }
            return false;
        }
    }
}

bool GroupDao::listJoinedMembers(std::uint64_t group_id,
                                 std::vector<MembershipRecord>* members, std::string* err) {
    if (members == nullptr) {
        if (err != nullptr) {
            *err = "GroupDao 输出参数为空";
        }
        return false;
    }
    members->clear();
    const std::string sql = std::string("SELECT ") + kMembershipColumns +
                            "FROM group_members gm JOIN users u ON u.id = gm.user_id "
                            "WHERE gm.group_id = ? AND gm.status = 1 "
                            "ORDER BY gm.role ASC, gm.joined_at ASC, gm.id ASC";
    PreparedStatement statement(connection_, sql, err);
    if (!statement.valid() || !statement.bind({SqlParam{group_id}}, err) ||
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
            members->push_back(rowToMembership(row));
        } catch (const std::exception& e) {
            if (err != nullptr) {
                *err = std::string("解析成员行失败：") + e.what();
            }
            return false;
        }
    }
}

bool GroupDao::findInvite(std::uint64_t invite_id, std::optional<InviteRecord>* invite,
                          std::string* err) {
    if (invite == nullptr) {
        if (err != nullptr) {
            *err = "GroupDao 输出参数为空";
        }
        return false;
    }
    invite->reset();
    const std::string sql = std::string("SELECT ") + kInviteColumns +
                            "FROM group_members gm JOIN finance_groups g ON g.id = gm.group_id "
                            "LEFT JOIN users u ON u.id = gm.invited_by WHERE gm.id = ?";
    PreparedStatement statement(connection_, sql, err);
    if (!statement.valid() || !statement.bind({SqlParam{invite_id}}, err) ||
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
        *invite = rowToInvite(row);
    } catch (const std::exception& e) {
        if (err != nullptr) {
            *err = std::string("解析邀请行失败：") + e.what();
        }
        return false;
    }
    return true;
}

bool GroupDao::lockInvite(std::uint64_t invite_id, std::optional<InviteRecord>* invite,
                          std::string* err) {
    if (invite == nullptr) {
        if (err != nullptr) {
            *err = "GroupDao 输出参数为空";
        }
        return false;
    }
    invite->reset();
    const char* sql =
        "SELECT gm.id, gm.group_id, gm.user_id, gm.status, gm.invited_by, "
        "DATE_FORMAT(gm.created_at, '%Y-%m-%dT%H:%i:%sZ'), "
        "DATE_FORMAT(DATE_ADD(gm.created_at, INTERVAL 7 DAY), '%Y-%m-%dT%H:%i:%sZ'), "
        "(gm.status = 0 AND gm.created_at < DATE_SUB(UTC_TIMESTAMP(), INTERVAL 7 DAY)) "
        "FROM group_members gm WHERE gm.id = ? FOR UPDATE";
    PreparedStatement statement(connection_, sql, err);
    if (!statement.valid() || !statement.bind({SqlParam{invite_id}}, err) ||
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
        if (row.size() != 8) {
            throw std::runtime_error("邀请锁定查询结果列数不正确");
        }
        InviteRecord record;
        record.id = parseUnsigned(row[0], "group_members.id");
        record.group_id = parseUnsigned(row[1], "group_members.group_id");
        record.user_id = parseUnsigned(row[2], "group_members.user_id");
        record.status = parseInt(row[3], "group_members.status");
        record.invited_by = parseUnsigned(row[4], "group_members.invited_by");
        record.created_at = requiredText(row[5], "group_members.created_at");
        record.expires_at = requiredText(row[6], "invite.expires_at");
        record.expired = parseInt(row[7], "invite.expired") != 0;
        *invite = std::move(record);
    } catch (const std::exception& e) {
        if (err != nullptr) {
            *err = std::string("解析邀请行失败：") + e.what();
        }
        return false;
    }
    return true;
}

bool GroupDao::listPendingInvites(std::uint64_t user_id, std::vector<InviteRecord>* invites,
                                  std::string* err) {
    if (invites == nullptr) {
        if (err != nullptr) {
            *err = "GroupDao 输出参数为空";
        }
        return false;
    }
    invites->clear();
    const std::string sql = std::string("SELECT ") + kInviteColumns +
                            "FROM group_members gm JOIN finance_groups g ON g.id = gm.group_id "
                            "LEFT JOIN users u ON u.id = gm.invited_by "
                            "WHERE gm.user_id = ? AND gm.status = 0 AND g.status = 0 "
                            "AND gm.created_at >= DATE_SUB(UTC_TIMESTAMP(), INTERVAL 7 DAY) "
                            "ORDER BY gm.created_at DESC, gm.id DESC";
    PreparedStatement statement(connection_, sql, err);
    if (!statement.valid() || !statement.bind({SqlParam{user_id}}, err) ||
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
            invites->push_back(rowToInvite(row));
        } catch (const std::exception& e) {
            if (err != nullptr) {
                *err = std::string("解析邀请列表失败：") + e.what();
            }
            return false;
        }
    }
}

bool GroupDao::expirePendingInvites(std::uint64_t user_id, std::uint64_t* changed,
                                    std::string* err) {
    if (changed != nullptr) {
        *changed = 0;
    }
    PreparedStatement statement(
        connection_,
        "UPDATE group_members SET status = 5 WHERE user_id = ? AND status = 0 "
        "AND created_at < DATE_SUB(UTC_TIMESTAMP(), INTERVAL 7 DAY)",
        err);
    if (!statement.valid() || !statement.bind({SqlParam{user_id}}, err) ||
        !statement.execute(err)) {
        return false;
    }
    if (changed != nullptr) {
        *changed = statement.affectedRows();
    }
    return true;
}

bool GroupDao::countJoinedMembers(std::uint64_t group_id, std::uint64_t* count,
                                  std::string* err) {
    PreparedStatement statement(connection_,
                                "SELECT COUNT(*) FROM group_members "
                                "WHERE group_id = ? AND status = 1",
                                err);
    if (!statement.valid() || !statement.bind({SqlParam{group_id}}, err) ||
        !statement.execute(err)) {
        return false;
    }
    return readScalarCount(statement, count, err);
}

bool GroupDao::countOccupiedSlots(std::uint64_t group_id, std::uint64_t* count,
                                  std::string* err) {
    PreparedStatement statement(connection_,
                                "SELECT COUNT(*) FROM group_members "
                                "WHERE group_id = ? AND status IN (0, 1)",
                                err);
    if (!statement.valid() || !statement.bind({SqlParam{group_id}}, err) ||
        !statement.execute(err)) {
        return false;
    }
    return readScalarCount(statement, count, err);
}

bool GroupDao::readOneGroup(const std::string& sql, const std::vector<SqlParam>& params,
                            std::optional<GroupRecord>* group, std::string* err) {
    if (group == nullptr) {
        if (err != nullptr) {
            *err = "GroupDao 输出参数为空";
        }
        return false;
    }
    group->reset();
    PreparedStatement statement(connection_, sql, err);
    if (!statement.valid() || !statement.bind(params, err) || !statement.execute(err)) {
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
        *group = rowToGroup(row);
    } catch (const std::exception& e) {
        if (err != nullptr) {
            *err = std::string("解析组行失败：") + e.what();
        }
        return false;
    }
    return true;
}

bool GroupDao::readOneMembership(const std::string& sql, const std::vector<SqlParam>& params,
                                 std::optional<MembershipRecord>* membership,
                                 std::string* err) {
    if (membership == nullptr) {
        if (err != nullptr) {
            *err = "GroupDao 输出参数为空";
        }
        return false;
    }
    membership->reset();
    PreparedStatement statement(connection_, sql, err);
    if (!statement.valid() || !statement.bind(params, err) || !statement.execute(err)) {
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
        *membership = rowToMembership(row);
    } catch (const std::exception& e) {
        if (err != nullptr) {
            *err = std::string("解析成员行失败：") + e.what();
        }
        return false;
    }
    return true;
}

}  // namespace db
}  // namespace cofinance
