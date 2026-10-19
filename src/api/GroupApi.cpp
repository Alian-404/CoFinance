#include "cofinance/api/GroupApi.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "cofinance/api/ApiSupport.h"
#include "cofinance/http/HttpServer.h"
#include "cofinance/service/GroupService.h"
#include "cofinance/service/UserService.h"

namespace cofinance {
namespace api {
namespace {

using json = support::json;
using http::HttpRequest;
using http::HttpResponse;
using service::AuthContext;
using service::GroupService;
using service::ServiceError;
using service::UserService;

json roleName(int role) {
    return role == 0 ? "owner" : "member";
}

json groupJson(const db::GroupRecord& group) {
    return json{{"id", group.id},
                {"name", group.name},
                {"owner_id", group.owner_id},
                {"created_at", group.created_at},
                {"updated_at", group.updated_at}};
}

json groupJson(const db::GroupSummaryRecord& summary) {
    json value = groupJson(summary.group);
    value["role"] = roleName(summary.role);
    value["member_count"] = summary.member_count;
    return value;
}

json memberJson(const db::MembershipRecord& member) {
    json joined_at = nullptr;
    if (member.joined_at.has_value()) {
        joined_at = *member.joined_at;
    }
    return json{{"user_id", member.user_id},
                {"username", member.username},
                {"nickname", member.nickname},
                {"avatar_url", member.avatar_url},
                {"role", roleName(member.role)},
                {"joined_at", std::move(joined_at)}};
}

json inviteJson(const db::InviteRecord& invite) {
    return json{{"id", invite.id},
                {"group_id", invite.group_id},
                {"group_name", invite.group_name},
                {"invited_by", invite.invited_by},
                {"inviter_username", invite.inviter_username},
                {"created_at", invite.created_at},
                {"expires_at", invite.expires_at}};
}

bool pathId(const HttpRequest& request, const char* name, std::uint64_t* value,
            HttpResponse* error) {
    const auto found = request.path_params.find(name);
    if (found == request.path_params.end() || !support::parseUInt64(found->second, value)) {
        *error = support::badRequest(std::string("路径参数无效: ") + name);
        return false;
    }
    return true;
}

bool authenticate(const HttpRequest& request, UserService& users, AuthContext* context,
                  HttpResponse* error) {
    return support::authenticateRequest(request, users, context, error);
}

}  // namespace

void GroupApi::registerRoutes(http::HttpServer& server, UserService& users,
                              GroupService& groups) {
    server.addRoute("POST", "/api/groups", [&users, &groups](const HttpRequest& request) {
        HttpResponse error;
        AuthContext context;
        if (!authenticate(request, users, &context, &error)) {
            return error;
        }
        json body;
        if (!support::parseJsonObject(request, &body, &error)) {
            return error;
        }
        std::string name;
        if (!support::requireString(body, "name", &name, &error)) {
            return error;
        }
        ServiceError service_error;
        db::GroupRecord created;
        if (!groups.createGroup(context.user.id, name, &created, &service_error)) {
            return support::errorResult(service_error);
        }
        return support::jsonResult(200, 0, "ok", groupJson(created));
    });

    server.addRoute("GET", "/api/groups", [&users, &groups](const HttpRequest& request) {
        HttpResponse error;
        AuthContext context;
        if (!authenticate(request, users, &context, &error)) {
            return error;
        }
        ServiceError service_error;
        std::vector<db::GroupSummaryRecord> found;
        if (!groups.listGroups(context.user.id, &found, &service_error)) {
            return support::errorResult(service_error);
        }
        json items = json::array();
        for (const db::GroupSummaryRecord& group : found) {
            items.push_back(groupJson(group));
        }
        return support::jsonResult(200, 0, "ok", json{{"items", std::move(items)}});
    });

    server.addRoute("GET", "/api/groups/{gid}", [&users, &groups](const HttpRequest& request) {
        HttpResponse error;
        AuthContext context;
        if (!authenticate(request, users, &context, &error)) {
            return error;
        }
        std::uint64_t group_id = 0;
        if (!pathId(request, "gid", &group_id, &error)) {
            return error;
        }
        ServiceError service_error;
        db::GroupRecord group;
        int role = 1;
        std::uint64_t member_count = 0;
        if (!groups.getGroup(context.user.id, group_id, &group, &role, &member_count,
                             &service_error)) {
            return support::errorResult(service_error);
        }
        json value = groupJson(group);
        value["role"] = roleName(role);
        value["member_count"] = member_count;
        return support::jsonResult(200, 0, "ok", std::move(value));
    });

    server.addRoute("PUT", "/api/groups/{gid}", [&users, &groups](const HttpRequest& request) {
        HttpResponse error;
        AuthContext context;
        if (!authenticate(request, users, &context, &error)) {
            return error;
        }
        std::uint64_t group_id = 0;
        if (!pathId(request, "gid", &group_id, &error)) {
            return error;
        }
        json body;
        if (!support::parseJsonObject(request, &body, &error)) {
            return error;
        }
        std::string name;
        if (!support::requireString(body, "name", &name, &error)) {
            return error;
        }
        ServiceError service_error;
        db::GroupRecord updated;
        if (!groups.renameGroup(context.user.id, group_id, name, &updated, &service_error)) {
            return support::errorResult(service_error);
        }
        return support::jsonResult(200, 0, "ok", groupJson(updated));
    });

    server.addRoute("DELETE", "/api/groups/{gid}",
                    [&users, &groups](const HttpRequest& request) {
                        HttpResponse error;
                        AuthContext context;
                        if (!authenticate(request, users, &context, &error)) {
                            return error;
                        }
                        std::uint64_t group_id = 0;
                        if (!pathId(request, "gid", &group_id, &error)) {
                            return error;
                        }
                        ServiceError service_error;
                        if (!groups.dissolveGroup(context.user.id, group_id, &service_error)) {
                            return support::errorResult(service_error);
                        }
                        return support::jsonResult(200, 0, "ok");
                    });

    server.addRoute("GET", "/api/groups/{gid}/members",
                    [&users, &groups](const HttpRequest& request) {
                        HttpResponse error;
                        AuthContext context;
                        if (!authenticate(request, users, &context, &error)) {
                            return error;
                        }
                        std::uint64_t group_id = 0;
                        if (!pathId(request, "gid", &group_id, &error)) {
                            return error;
                        }
                        ServiceError service_error;
                        std::vector<db::MembershipRecord> members;
                        if (!groups.listMembers(context.user.id, group_id, &members,
                                                &service_error)) {
                            return support::errorResult(service_error);
                        }
                        json items = json::array();
                        for (const db::MembershipRecord& member : members) {
                            items.push_back(memberJson(member));
                        }
                        return support::jsonResult(200, 0, "ok",
                                                   json{{"items", std::move(items)},
                                                        {"count", members.size()}});
                    });

    server.addRoute("POST", "/api/groups/{gid}/members",
                    [&users, &groups](const HttpRequest& request) {
                        HttpResponse error;
                        AuthContext context;
                        if (!authenticate(request, users, &context, &error)) {
                            return error;
                        }
                        std::uint64_t group_id = 0;
                        if (!pathId(request, "gid", &group_id, &error)) {
                            return error;
                        }
                        json body;
                        if (!support::parseJsonObject(request, &body, &error)) {
                            return error;
                        }
                        std::string username;
                        if (!support::requireString(body, "username", &username, &error)) {
                            return error;
                        }
                        ServiceError service_error;
                        db::InviteRecord invite;
                        if (!groups.inviteMember(context.user.id, group_id, username, &invite,
                                                 &service_error)) {
                            return support::errorResult(service_error);
                        }
                        return support::jsonResult(200, 0, "ok", inviteJson(invite));
                    });

    server.addRoute("DELETE", "/api/groups/{gid}/members/{uid}",
                    [&users, &groups](const HttpRequest& request) {
                        HttpResponse error;
                        AuthContext context;
                        if (!authenticate(request, users, &context, &error)) {
                            return error;
                        }
                        std::uint64_t group_id = 0;
                        std::uint64_t target_id = 0;
                        if (!pathId(request, "gid", &group_id, &error) ||
                            !pathId(request, "uid", &target_id, &error)) {
                            return error;
                        }
                        ServiceError service_error;
                        if (!groups.removeMember(context.user.id, group_id, target_id,
                                                 &service_error)) {
                            return support::errorResult(service_error);
                        }
                        return support::jsonResult(200, 0, "ok");
                    });

    server.addRoute("POST", "/api/groups/{gid}/members/leave",
                    [&users, &groups](const HttpRequest& request) {
                        HttpResponse error;
                        AuthContext context;
                        if (!authenticate(request, users, &context, &error)) {
                            return error;
                        }
                        std::uint64_t group_id = 0;
                        if (!pathId(request, "gid", &group_id, &error)) {
                            return error;
                        }
                        ServiceError service_error;
                        if (!groups.leaveGroup(context.user.id, group_id, &service_error)) {
                            return support::errorResult(service_error);
                        }
                        return support::jsonResult(200, 0, "ok");
                    });

    server.addRoute("GET", "/api/invites", [&users, &groups](const HttpRequest& request) {
        HttpResponse error;
        AuthContext context;
        if (!authenticate(request, users, &context, &error)) {
            return error;
        }
        ServiceError service_error;
        std::vector<db::InviteRecord> invites;
        if (!groups.listInvites(context.user.id, &invites, &service_error)) {
            return support::errorResult(service_error);
        }
        json items = json::array();
        for (const db::InviteRecord& invite : invites) {
            items.push_back(inviteJson(invite));
        }
        return support::jsonResult(200, 0, "ok", json{{"items", std::move(items)}});
    });

    server.addRoute("POST", "/api/invites/{invite_id}/accept",
                    [&users, &groups](const HttpRequest& request) {
                        HttpResponse error;
                        AuthContext context;
                        if (!authenticate(request, users, &context, &error)) {
                            return error;
                        }
                        std::uint64_t invite_id = 0;
                        if (!pathId(request, "invite_id", &invite_id, &error)) {
                            return error;
                        }
                        ServiceError service_error;
                        std::uint64_t group_id = 0;
                        if (!groups.acceptInvite(context.user.id, invite_id, &group_id,
                                                 &service_error)) {
                            return support::errorResult(service_error);
                        }
                        return support::jsonResult(200, 0, "ok", json{{"group_id", group_id}});
                    });

    server.addRoute("POST", "/api/invites/{invite_id}/reject",
                    [&users, &groups](const HttpRequest& request) {
                        HttpResponse error;
                        AuthContext context;
                        if (!authenticate(request, users, &context, &error)) {
                            return error;
                        }
                        std::uint64_t invite_id = 0;
                        if (!pathId(request, "invite_id", &invite_id, &error)) {
                            return error;
                        }
                        ServiceError service_error;
                        if (!groups.rejectInvite(context.user.id, invite_id, &service_error)) {
                            return support::errorResult(service_error);
                        }
                        return support::jsonResult(200, 0, "ok");
                    });

    server.addRoute("PUT", "/api/groups/{gid}/members/{uid}/role",
                    [&users, &groups](const HttpRequest& request) {
                        HttpResponse error;
                        AuthContext context;
                        if (!authenticate(request, users, &context, &error)) {
                            return error;
                        }
                        std::uint64_t group_id = 0;
                        std::uint64_t target_id = 0;
                        if (!pathId(request, "gid", &group_id, &error) ||
                            !pathId(request, "uid", &target_id, &error)) {
                            return error;
                        }
                        json body;
                        if (!support::parseJsonObject(request, &body, &error)) {
                            return error;
                        }
                        std::string role;
                        if (!support::requireString(body, "role", &role, &error)) {
                            return error;
                        }
                        if (role != "owner") {
                            return support::badRequest("role 目前只支持 owner");
                        }
                        ServiceError service_error;
                        if (!groups.transferOwner(context.user.id, group_id, target_id,
                                                  &service_error)) {
                            return support::errorResult(service_error);
                        }
                        return support::jsonResult(200, 0, "ok");
                    });
}

}  // namespace api
}  // namespace cofinance
