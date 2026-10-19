#!/usr/bin/env python3
"""P5 验收：组、邀请、成员状态机、owner 转让与 IDOR 越权。"""

import argparse
import http.client
import json
import os
import shutil
import signal
import socket
import subprocess
import tempfile
import time
from pathlib import Path


def pick_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def wait_ready(proc: subprocess.Popen, port: int, timeout: float = 8.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            stderr = proc.stderr.read() if proc.stderr else ""
            raise RuntimeError(f"服务启动失败，退出码 {proc.returncode}: {stderr}")
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError("等待 HTTP 服务监听超时")


def load_kv(path: Path) -> dict:
    values = {}
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        values[key.strip()] = value.strip()
    return values


class Client:
    def __init__(self, port: int):
        self.port = port

    def call(self, method: str, path: str, body=None, token=None):
        headers = {"Content-Type": "application/json", "Connection": "close"}
        payload = None if body is None else json.dumps(body, ensure_ascii=False).encode("utf-8")
        if token is not None:
            headers["Authorization"] = f"Bearer {token}"
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=5)
        try:
            conn.request(method, path, body=payload, headers=headers)
            response = conn.getresponse()
            raw = response.read()
            status = response.status
        finally:
            conn.close()
        try:
            parsed = json.loads(raw.decode("utf-8"))
        except Exception as exc:
            raise AssertionError(f"响应不是 JSON: {status} {raw!r}") from exc
        assert parsed.get("code") is not None and "msg" in parsed and "data" in parsed, parsed
        return status, parsed


def expect(result, status: int, code: int, context: str):
    actual_status, body = result
    assert actual_status == status and body["code"] == code, (context, actual_status, body)
    return body


def cleanup(root: Path, user_ids: list[int], group_ids: list[int]) -> None:
    mysql = shutil.which("mysql")
    if not mysql:
        return
    server = load_kv(root / "config/server.conf")
    secrets = load_kv(root / "config/secrets.conf")
    fd, defaults_path = tempfile.mkstemp(prefix="cofinance_p5_mysql_")
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as out:
            out.write("[client]\n")
            out.write(f"host={server.get('db.host', '127.0.0.1')}\n")
            out.write(f"port={server.get('db.port', '3306')}\n")
            out.write(f"user={secrets.get('db.user', '')}\n")
            out.write(f"password={secrets.get('db.password', '')}\n")
            out.write(f"database={server.get('db.name', 'cofinance')}\n")
        os.chmod(defaults_path, 0o600)
        statements = []
        if group_ids:
            groups = ",".join(str(int(value)) for value in group_ids)
            statements.append(f"DELETE FROM group_members WHERE group_id IN ({groups});")
            statements.append(f"DELETE FROM finance_groups WHERE id IN ({groups});")
        if user_ids:
            users = ",".join(str(int(value)) for value in user_ids)
            statements.append(f"DELETE FROM group_members WHERE user_id IN ({users});")
            statements.append(f"DELETE FROM sessions WHERE user_id IN ({users});")
            statements.append(f"DELETE FROM users WHERE id IN ({users});")
        if statements:
            subprocess.run(
                [mysql, f"--defaults-extra-file={defaults_path}", "-e", " ".join(statements)],
                check=False,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
    finally:
        try:
            os.unlink(defaults_path)
        except FileNotFoundError:
            pass


def register(client: Client, username: str, nickname: str) -> int:
    body = expect(
        client.call(
            "POST",
            "/api/auth/register",
            {"username": username, "password": "Password123", "nickname": nickname},
        ),
        200,
        0,
        f"register {username}",
    )
    return int(body["data"]["id"])


def login(client: Client, username: str) -> str:
    body = expect(
        client.call(
            "POST", "/api/auth/login", {"username": username, "password": "Password123"}
        ),
        200,
        0,
        f"login {username}",
    )
    return body["data"]["token"]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", default="build/cofinance")
    parser.add_argument("--valgrind", action="store_true")
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[2]
    port = pick_port()
    env = os.environ.copy()
    env.update(
        {
            "COFINANCE_SERVER_PORT": str(port),
            "COFINANCE_LOG_LEVEL": "WARN",
            "COFINANCE_LOG_TO_TERMINAL": "0",
            "COFINANCE_LOG_DIR": "/tmp/cofinance_p5_logs",
        }
    )
    command = [str(root / args.binary), "-c", "config/server.conf"]
    if args.valgrind:
        command = [
            "valgrind",
            "--leak-check=full",
            "--show-leak-kinds=definite,indirect",
            "--errors-for-leak-kinds=definite,indirect",
            "--error-exitcode=9",
            *command,
        ]
    proc = subprocess.Popen(
        command,
        cwd=root,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    client = Client(port)
    user_ids: list[int] = []
    group_ids: list[int] = []
    try:
        wait_ready(proc, port, timeout=60 if args.valgrind else 8)
        suffix = f"{os.getpid() % 100000}{int(time.time() * 1000) % 1000000}"
        names = [f"p5a{suffix}", f"p5b{suffix}", f"p5c{suffix}", f"p5d{suffix}"]
        user_ids = [
            register(client, names[0], "Alice"),
            register(client, names[1], "Bob"),
            register(client, names[2], "Carol"),
            register(client, names[3], "Dave"),
        ]
        tokens = [login(client, name) for name in names]

        expect(client.call("GET", "/api/groups"), 401, 1002, "未登录")
        created = expect(
            client.call("POST", "/api/groups", {"name": "  宿舍402  "}, token=tokens[0]),
            200,
            0,
            "创建组",
        )
        group_id = int(created["data"]["id"])
        group_ids.append(group_id)
        assert created["data"]["name"] == "宿舍402", created

        listing = expect(client.call("GET", "/api/groups", token=tokens[0]), 200, 0, "组列表")
        assert len(listing["data"]["items"]) == 1, listing
        assert listing["data"]["items"][0]["member_count"] == 1, listing

        detail = expect(
            client.call("GET", f"/api/groups/{group_id}", token=tokens[0]), 200, 0, "组详情"
        )
        assert detail["data"]["role"] == "owner", detail
        expect(
            client.call("GET", f"/api/groups/{group_id}", token=tokens[3]), 403, 1004, "IDOR"
        )
        expect(client.call("GET", "/api/groups/999999999", token=tokens[0]), 404, 1005, "不存在")
        expect(client.call("GET", "/api/groups/not-a-number", token=tokens[0]), 400, 1001, "坏 ID")

        expect(
            client.call(
                "POST", f"/api/groups/{group_id}/members", {"username": names[0]}, token=tokens[0]
            ),
            400,
            1011,
            "邀请自己",
        )
        invite_bob = expect(
            client.call(
                "POST", f"/api/groups/{group_id}/members", {"username": names[1]}, token=tokens[0]
            ),
            200,
            0,
            "邀请 Bob",
        )
        bob_invite = int(invite_bob["data"]["id"])
        expect(
            client.call(
                "POST", f"/api/groups/{group_id}/members", {"username": names[1]}, token=tokens[0]
            ),
            409,
            1008,
            "重复邀请",
        )
        invites = expect(client.call("GET", "/api/invites", token=tokens[1]), 200, 0, "邀请列表")
        assert [item["id"] for item in invites["data"]["items"]] == [bob_invite], invites
        expect(
            client.call("POST", f"/api/invites/{bob_invite}/accept", token=tokens[2]),
            409,
            1009,
            "冒领邀请",
        )
        expect(
            client.call("POST", f"/api/invites/{bob_invite}/accept", token=tokens[1]),
            200,
            0,
            "接受邀请",
        )
        members = expect(
            client.call("GET", f"/api/groups/{group_id}/members", token=tokens[0]),
            200,
            0,
            "成员列表",
        )
        assert members["data"]["count"] == 2, members
        assert {item["role"] for item in members["data"]["items"]} == {"owner", "member"}, members

        invite_carol = expect(
            client.call(
                "POST", f"/api/groups/{group_id}/members", {"username": names[2]}, token=tokens[1]
            ),
            200,
            0,
            "成员邀请 Carol",
        )
        expect(
            client.call("POST", f"/api/invites/{invite_carol['data']['id']}/accept", token=tokens[2]),
            200,
            0,
            "Carol 接受",
        )
        expect(
            client.call(
                "PUT", f"/api/groups/{group_id}", {"name": "Bob 改名"}, token=tokens[1]
            ),
            403,
            1004,
            "非 owner 改名",
        )
        renamed = expect(
            client.call(
                "PUT", f"/api/groups/{group_id}", {"name": "新宿舍"}, token=tokens[0]
            ),
            200,
            0,
            "owner 改名",
        )
        assert renamed["data"]["name"] == "新宿舍", renamed
        expect(
            client.call("GET", f"/api/groups/{group_id}/members", token=tokens[3]),
            403,
            1004,
            "非成员看成员",
        )
        expect(
            client.call(
                "DELETE", f"/api/groups/{group_id}/members/{user_ids[2]}", token=tokens[1]
            ),
            403,
            1004,
            "非 owner 移除",
        )
        expect(
            client.call(
                "DELETE", f"/api/groups/{group_id}/members/{user_ids[2]}", token=tokens[0]
            ),
            200,
            0,
            "owner 移除",
        )
        expect(
            client.call("GET", f"/api/groups/{group_id}", token=tokens[2]), 403, 1004, "被移除"
        )
        expect(
            client.call("POST", f"/api/groups/{group_id}/members/leave", token=tokens[0]),
            400,
            1017,
            "owner 禁止退出",
        )
        expect(
            client.call("POST", f"/api/groups/{group_id}/members/leave", token=tokens[1]),
            200,
            0,
            "成员退出",
        )
        expect(
            client.call("GET", f"/api/groups/{group_id}", token=tokens[1]), 403, 1004, "退出后"
        )

        bob_again = expect(
            client.call(
                "POST", f"/api/groups/{group_id}/members", {"username": names[1]}, token=tokens[0]
            ),
            200,
            0,
            "重新邀请 Bob",
        )
        expect(
            client.call("POST", f"/api/invites/{bob_again['data']['id']}/accept", token=tokens[1]),
            200,
            0,
            "Bob 再次加入",
        )

        invite_dave = expect(
            client.call(
                "POST", f"/api/groups/{group_id}/members", {"username": names[3]}, token=tokens[0]
            ),
            200,
            0,
            "邀请 Dave",
        )
        dave_invite = int(invite_dave["data"]["id"])
        expect(
            client.call("POST", f"/api/invites/{dave_invite}/reject", token=tokens[3]),
            200,
            0,
            "拒绝邀请",
        )
        expect(
            client.call("POST", f"/api/invites/{dave_invite}/accept", token=tokens[3]),
            409,
            1009,
            "拒绝后接受",
        )
        expect(
            client.call(
                "PUT",
                f"/api/groups/{group_id}/members/{user_ids[1]}/role",
                {"role": "member"},
                token=tokens[0],
            ),
            400,
            1001,
            "非法 role",
        )
        expect(
            client.call(
                "PUT",
                f"/api/groups/{group_id}/members/{user_ids[3]}/role",
                {"role": "owner"},
                token=tokens[0],
            ),
            400,
            1017,
            "转让给非成员",
        )
        expect(
            client.call(
                "PUT",
                f"/api/groups/{group_id}/members/{user_ids[1]}/role",
                {"role": "owner"},
                token=tokens[0],
            ),
            200,
            0,
            "转让 owner",
        )
        detail = expect(
            client.call("GET", f"/api/groups/{group_id}", token=tokens[1]), 200, 0, "新 owner 详情"
        )
        assert detail["data"]["owner_id"] == user_ids[1] and detail["data"]["role"] == "owner", detail
        expect(
            client.call("DELETE", f"/api/groups/{group_id}", token=tokens[0]),
            403,
            1004,
            "旧 owner 解散",
        )
        expect(client.call("DELETE", "/api/users/me", token=tokens[1]), 400, 1017, "owner 注销保护")
        expect(
            client.call(
                "PUT",
                f"/api/groups/{group_id}/members/{user_ids[0]}/role",
                {"role": "owner"},
                token=tokens[1],
            ),
            200,
            0,
            "转回 owner",
        )
        expect(
            client.call("DELETE", f"/api/groups/{group_id}", token=tokens[0]), 200, 0, "解散组"
        )
        listing = expect(client.call("GET", "/api/groups", token=tokens[0]), 200, 0, "解散后列表")
        assert listing["data"]["items"] == [], listing
        expect(
            client.call("GET", f"/api/groups/{group_id}", token=tokens[0]), 404, 1005, "解散后详情"
        )

        print("P5 group integration: 创建/邀请/接受/拒绝/退出/移除/转让/解散/越权 OK")
        return 0
    finally:
        cleanup(root, user_ids, group_ids)
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=5)
        if args.valgrind:
            valgrind_output = proc.stderr.read() if proc.stderr else ""
            if proc.returncode != 0 or "ERROR SUMMARY: 0 errors" not in valgrind_output:
                raise RuntimeError(f"Valgrind 检查失败，退出码 {proc.returncode}:\n{valgrind_output}")


if __name__ == "__main__":
    raise SystemExit(main())
