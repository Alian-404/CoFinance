#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "cofinance/db/GroupDao.h"
#include "cofinance/service/UserService.h"
#include "cofinance/util/Common.h"

namespace cofinance {
namespace db {
class ConnectionPool;
}
namespace service {

class GroupService : private NonCopyable {
public:
    using RealtimeMemberRevoke = std::function<void(std::uint64_t, std::uint64_t)>;
    using RealtimeGroupRevoke = std::function<void(std::uint64_t)>;

    explicit GroupService(db::ConnectionPool& pool);

    void setRealtimeRevokeCallbacks(RealtimeMemberRevoke member_revoke,
                                    RealtimeGroupRevoke group_revoke);

    bool createGroup(std::uint64_t user_id, const std::string& name, db::GroupRecord* group,
                     ServiceError* error);
    bool listGroups(std::uint64_t user_id, std::vector<db::GroupSummaryRecord>* groups,
                    ServiceError* error);
    bool getGroup(std::uint64_t user_id, std::uint64_t group_id, db::GroupRecord* group,
                  int* role, std::uint64_t* member_count, ServiceError* error);
    bool renameGroup(std::uint64_t user_id, std::uint64_t group_id, const std::string& name,
                     db::GroupRecord* group, ServiceError* error);
    bool dissolveGroup(std::uint64_t user_id, std::uint64_t group_id, bool force,
                       ServiceError* error);
    bool dissolveGroup(std::uint64_t user_id, std::uint64_t group_id, ServiceError* error) {
        return dissolveGroup(user_id, group_id, false, error);
    }

    bool listMembers(std::uint64_t user_id, std::uint64_t group_id,
                     std::vector<db::MembershipRecord>* members, ServiceError* error);
    bool inviteMember(std::uint64_t user_id, std::uint64_t group_id,
                      const std::string& username, db::InviteRecord* invite,
                      ServiceError* error);
    bool removeMember(std::uint64_t user_id, std::uint64_t group_id,
                      std::uint64_t target_user_id, ServiceError* error);
    bool leaveGroup(std::uint64_t user_id, std::uint64_t group_id, ServiceError* error);

    bool listInvites(std::uint64_t user_id, std::vector<db::InviteRecord>* invites,
                     ServiceError* error);
    bool acceptInvite(std::uint64_t user_id, std::uint64_t invite_id, std::uint64_t* group_id,
                      ServiceError* error);
    bool rejectInvite(std::uint64_t user_id, std::uint64_t invite_id, ServiceError* error);

    bool transferOwner(std::uint64_t user_id, std::uint64_t group_id,
                       std::uint64_t target_user_id, ServiceError* error);

private:
    void notifyMemberRevoked(std::uint64_t group_id, std::uint64_t user_id) noexcept;
    void notifyGroupRevoked(std::uint64_t group_id) noexcept;

    db::ConnectionPool& pool_;
    RealtimeMemberRevoke realtime_member_revoke_;
    RealtimeGroupRevoke realtime_group_revoke_;
};

}  // namespace service
}  // namespace cofinance
