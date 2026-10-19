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

struct GroupRecord {
    std::uint64_t id = 0;
    std::string name;
    std::uint64_t owner_id = 0;
    int status = 0;
    std::string created_at;
    std::string updated_at;
};

struct MembershipRecord {
    std::uint64_t id = 0;
    std::uint64_t group_id = 0;
    std::uint64_t user_id = 0;
    int role = 1;
    int status = 0;
    std::uint64_t invited_by = 0;
    std::string created_at;
    std::optional<std::string> joined_at;
    std::string username;
    std::string nickname;
    std::string avatar_url;
    bool invite_expired = false;
};

struct GroupSummaryRecord {
    GroupRecord group;
    int role = 1;
    std::uint64_t member_count = 0;
};

struct InviteRecord {
    std::uint64_t id = 0;
    std::uint64_t group_id = 0;
    std::uint64_t user_id = 0;
    int status = 0;
    std::uint64_t invited_by = 0;
    std::string group_name;
    std::string inviter_username;
    std::string created_at;
    std::string expires_at;
    bool expired = false;
};

// 组与成员表的数据访问。权限、状态机和事务边界在 GroupService。
class GroupDao : private NonCopyable {
public:
    explicit GroupDao(MySqlConnection& connection);

    bool findGroup(std::uint64_t group_id, std::optional<GroupRecord>* group, std::string* err);
    bool lockGroup(std::uint64_t group_id, std::optional<GroupRecord>* group, std::string* err);
    bool insertGroup(GroupRecord& group, std::string* err);
    bool updateGroupName(std::uint64_t group_id, const std::string& name, std::string* err);
    bool setGroupOwner(std::uint64_t group_id, std::uint64_t owner_id, std::string* err);
    bool dissolveGroup(std::uint64_t group_id, std::string* err);
    bool listGroupsForUser(std::uint64_t user_id, std::vector<GroupSummaryRecord>* groups,
                           std::string* err);

    bool findMembership(std::uint64_t group_id, std::uint64_t user_id,
                        std::optional<MembershipRecord>* membership, std::string* err);
    bool lockMembership(std::uint64_t group_id, std::uint64_t user_id,
                        std::optional<MembershipRecord>* membership, std::string* err);
    bool insertMembership(std::uint64_t group_id, std::uint64_t user_id, int role, int status,
                          std::uint64_t invited_by, bool joined,
                          std::optional<MembershipRecord>* membership, std::string* err);
    bool updateInvitation(std::uint64_t member_id, std::uint64_t invited_by, std::string* err);
    bool acceptMembership(std::uint64_t member_id, std::string* err);
    bool setMembershipStatus(std::uint64_t member_id, int status, std::string* err);
    bool setMembershipRole(std::uint64_t member_id, int role, std::string* err);
    bool withdrawMemberships(std::uint64_t user_id, std::string* err);
    bool listMembershipGroupIds(std::uint64_t user_id, std::vector<std::uint64_t>* group_ids,
                                std::string* err);
    bool listJoinedMembers(std::uint64_t group_id, std::vector<MembershipRecord>* members,
                           std::string* err);

    bool findInvite(std::uint64_t invite_id, std::optional<InviteRecord>* invite,
                    std::string* err);
    bool lockInvite(std::uint64_t invite_id, std::optional<InviteRecord>* invite,
                    std::string* err);
    bool listPendingInvites(std::uint64_t user_id, std::vector<InviteRecord>* invites,
                            std::string* err);
    bool expirePendingInvites(std::uint64_t user_id, std::uint64_t* changed, std::string* err);

    bool countJoinedMembers(std::uint64_t group_id, std::uint64_t* count, std::string* err);
    bool countOccupiedSlots(std::uint64_t group_id, std::uint64_t* count, std::string* err);

private:
    bool readOneGroup(const std::string& sql, const std::vector<SqlParam>& params,
                      std::optional<GroupRecord>* group, std::string* err);
    bool readOneMembership(const std::string& sql, const std::vector<SqlParam>& params,
                           std::optional<MembershipRecord>* membership, std::string* err);

    MySqlConnection& connection_;
};

}  // namespace db
}  // namespace cofinance
