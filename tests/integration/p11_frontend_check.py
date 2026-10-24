#!/usr/bin/env python3
"""P11 验收：静态前端资源、CSP、路由契约与设置页完整的 API 链路。"""

import argparse
import http.client
import json
import os
import re
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


def wait_ready(proc: subprocess.Popen, port: int, timeout: float = 12.0) -> None:
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
    def __init__(self, port: int, timeout: float = 8.0):
        self.port = port
        self.timeout = timeout

    def request(self, method: str, path: str, body=None, token=None):
        headers = {"Connection": "close"}
        payload = None
        if body is not None:
            headers["Content-Type"] = "application/json"
            payload = json.dumps(body, ensure_ascii=False).encode("utf-8")
        if token:
            headers["Authorization"] = f"Bearer {token}"
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=self.timeout)
        try:
            conn.request(method, path, body=payload, headers=headers)
            response = conn.getresponse()
            raw = response.read()
            return response.status, dict(response.getheaders()), raw
        finally:
            conn.close()

    def call(self, method: str, path: str, body=None, token=None):
        status, _, raw = self.request(method, path, body, token)
        try:
            parsed = json.loads(raw.decode("utf-8"))
        except Exception as exc:
            raise AssertionError(f"响应不是 JSON: {status} {raw[:300]!r}") from exc
        assert isinstance(parsed, dict), parsed
        assert {"code", "msg", "data"} <= set(parsed), parsed
        return status, parsed


def expect(result, status: int, code: int, context: str):
    actual_status, body = result
    assert actual_status == status and body["code"] == code, (context, actual_status, body)
    return body


def register(client: Client, username: str, nickname: str) -> int:
    body = expect(
        client.call(
            "POST",
            "/api/auth/register",
            {"username": username, "password": "Password123", "nickname": nickname},
        ),
        200,
        0,
        f"注册 {username}",
    )
    return int(body["data"]["id"])


def login(client: Client, username: str, password: str = "Password123") -> str:
    body = expect(
        client.call("POST", "/api/auth/login", {"username": username, "password": password}),
        200,
        0,
        f"登录 {username}",
    )
    return body["data"]["token"]


def cleanup(root: Path, user_ids: list[int], group_ids: list[int]) -> None:
    mysql = shutil.which("mysql")
    if not mysql:
        return
    server = load_kv(root / "config/server.conf")
    secrets = load_kv(root / "config/secrets.conf")
    fd, defaults_path = tempfile.mkstemp(prefix="cofinance_p11_mysql_")
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
            statements.extend(
                [
                    f"DELETE FROM settlements WHERE group_id IN ({groups});",
                    f"DELETE FROM messages WHERE group_id IN ({groups});",
                    "DELETE p FROM transaction_participants p JOIN transactions t "
                    f"ON t.id = p.transaction_id WHERE t.group_id IN ({groups});",
                    f"DELETE FROM transactions WHERE group_id IN ({groups});",
                    f"DELETE FROM categories WHERE group_id IN ({groups});",
                    f"DELETE FROM group_members WHERE group_id IN ({groups});",
                    f"DELETE FROM finance_groups WHERE id IN ({groups});",
                ]
            )
        if user_ids:
            users = ",".join(str(int(value)) for value in user_ids)
            statements.extend(
                [
                    f"DELETE FROM group_members WHERE user_id IN ({users});",
                    f"DELETE FROM sessions WHERE user_id IN ({users});",
                    f"DELETE FROM users WHERE id IN ({users});",
                ]
            )
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


def start_server(root: Path, binary: str, env: dict) -> tuple[subprocess.Popen, int]:
    port = pick_port()
    process_env = env.copy()
    process_env["COFINANCE_SERVER_PORT"] = str(port)
    proc = subprocess.Popen(
        [str(root / binary), "-c", "config/server.conf"],
        cwd=root,
        env=process_env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    wait_ready(proc, port)
    return proc, port


def stop_server(proc: subprocess.Popen) -> None:
    if proc.poll() is not None:
        return
    proc.send_signal(signal.SIGTERM)
    try:
        proc.wait(timeout=15)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=5)


def check_static(root: Path, client: Client) -> int:
    checks = 0
    status, headers, raw = client.request("GET", "/")
    assert status == 200, (status, raw[:200])
    html = raw.decode("utf-8")
    assert 'content="text/html; charset=utf-8"' not in headers.get("Content-Type", ""), headers
    assert "text/html" in headers.get("Content-Type", ""), headers
    assert "Content-Security-Policy" in html and "script-src 'self'" in html
    assert not re.search(r"\son[a-z]+\s*=", html, flags=re.I), "index 不应包含内联事件处理器"
    assert '<script type="module" src="/assets/js/app.js"></script>' in html
    checks += 4

    references = sorted(set(re.findall(r'(?:src|href)="(/assets/[^"?#]+)', html)))
    assert references, references
    mime_expectations = {
        ".css": "text/css",
        ".js": "application/javascript",
        ".svg": "image/svg+xml",
    }
    for url in references:
        status, response_headers, body = client.request("GET", url)
        assert status == 200 and body, (url, status)
        expected = mime_expectations.get(Path(url).suffix, "")
        if expected:
            assert expected in response_headers.get("Content-Type", ""), (url, response_headers)
        checks += 1

    www = root / "www"
    for path in sorted((www / "assets").rglob("*")):
        if not path.is_file():
            continue
        url = "/" + path.relative_to(www).as_posix()
        status, response_headers, body = client.request("GET", url)
        assert status == 200 and body, (url, status)
        expected = mime_expectations.get(path.suffix, "")
        if expected:
            assert expected in response_headers.get("Content-Type", ""), (url, response_headers)
        checks += 1

    for path in sorted((www / "assets/js").rglob("*.js")):
        source = path.read_text(encoding="utf-8")
        for target in re.findall(r"from\s+['\"](\.[^'\"]+)['\"]", source):
            resolved = (path.parent / target).resolve()
            assert resolved.exists() or resolved.with_suffix(".js").exists(), (path, target)
            checks += 1
    return checks


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", default="build/cofinance")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    env = os.environ.copy()
    env.update(
        {
            "COFINANCE_LOG_LEVEL": "WARN",
            "COFINANCE_LOG_TO_TERMINAL": "0",
            "COFINANCE_LOG_DIR": "/tmp/cofinance_p11_logs",
        }
    )

    proc = None
    user_ids: list[int] = []
    group_ids: list[int] = []
    try:
        proc, port = start_server(root, args.binary, env)
        client = Client(port)
        checks = check_static(root, client)

        suffix = f"{os.getpid() % 100000}{int(time.time() * 1000) % 1000000}"
        alice_name = f"p11alice{suffix}"
        bob_name = f"p11bob{suffix}"
        user_ids = [register(client, alice_name, "Alice"), register(client, bob_name, "Bob")]
        alice_id, bob_id = user_ids
        alice = login(client, alice_name)
        bob = login(client, bob_name)
        checks += 5

        group = expect(
            client.call("POST", "/api/groups", {"name": "P11 前端验收组"}, token=alice),
            200,
            0,
            "创建组",
        )
        group_id = int(group["data"]["id"])
        group_ids.append(group_id)
        renamed = expect(
            client.call(
                "PUT", f"/api/groups/{group_id}", {"name": "P11 设置页验收组"}, token=alice
            ),
            200,
            0,
            "修改组资料",
        )
        assert renamed["data"]["name"] == "P11 设置页验收组", renamed
        checks += 2

        invite = expect(
            client.call(
                "POST", f"/api/groups/{group_id}/members", {"username": bob_name}, token=alice
            ),
            200,
            0,
            "邀请成员",
        )
        invites = expect(client.call("GET", "/api/invites", token=bob), 200, 0, "待处理邀请")
        assert any(item["id"] == invite["data"]["id"] for item in invites["data"]["items"]), invites
        assert any(item["group_name"] == "P11 设置页验收组" for item in invites["data"]["items"]), invites
        expect(
            client.call("POST", f"/api/invites/{invite['data']['id']}/accept", token=bob),
            200,
            0,
            "接受邀请",
        )
        members = expect(
            client.call("GET", f"/api/groups/{group_id}/members", token=alice),
            200,
            0,
            "成员列表",
        )
        assert members["data"]["count"] == 2, members
        checks += 5

        categories = expect(
            client.call("GET", f"/api/categories?gid={group_id}", token=alice),
            200,
            0,
            "分类列表",
        )
        assert categories["data"]["items"], categories
        custom = expect(
            client.call(
                "POST",
                f"/api/groups/{group_id}/categories",
                {"name": "P11 分类", "icon": "more", "sort": 100},
                token=alice,
            ),
            200,
            0,
            "新增分类",
        )
        expect(
            client.call(
                "DELETE", f"/api/groups/{group_id}/categories/{custom['data']['id']}", token=alice
            ),
            200,
            0,
            "删除分类",
        )
        checks += 3

        category_id = int(categories["data"]["items"][0]["id"])
        token = "11111111-2222-4333-8444-555555555555"
        payload = {
            "type": 0,
            "amount_cents": 1000,
            "category_id": category_id,
            "note": "P11 交易",
            "payer_id": alice_id,
            "split_type": 0,
            "participants": [{"user_id": alice_id}, {"user_id": bob_id}],
            "client_token": token,
        }
        first = expect(
            client.call("POST", f"/api/groups/{group_id}/transactions", payload, token=alice),
            200,
            0,
            "新增交易",
        )
        replay = expect(
            client.call("POST", f"/api/groups/{group_id}/transactions", payload, token=alice),
            200,
            0,
            "幂等重放",
        )
        assert first["data"]["id"] == replay["data"]["id"], (first, replay)
        stats = expect(
            client.call("GET", f"/api/groups/{group_id}/stats", token=alice),
            200,
            0,
            "统计",
        )
        assert stats["data"]["summary"]["total_expense_cents"] == 1000, stats
        balances = expect(
            client.call("GET", f"/api/groups/{group_id}/balances", token=alice),
            200,
            0,
            "余额",
        )
        assert len(balances["data"]["items"]) == 2, balances
        checks += 6

        message = expect(
            client.call(
                "POST",
                f"/api/groups/{group_id}/messages",
                {"content": "P11 前端联调消息", "client_msg_id": token},
                token=alice,
            ),
            200,
            0,
            "发送消息",
        )
        listed = expect(
            client.call("GET", f"/api/groups/{group_id}/messages?limit=30", token=bob),
            200,
            0,
            "消息列表",
        )
        assert any(item["id"] == message["data"]["id"] for item in listed["data"]["items"]), listed
        expect(
            client.call("GET", f"/api/groups/{group_id}/ai/suggestions", token=alice),
            503,
            2002,
            "未配置 LLM 时 AI 降级",
        )
        checks += 4

        profile = expect(
            client.call(
                "PUT",
                "/api/users/me",
                {"nickname": "Alice P11", "avatar_url": "/assets/avatars/leaf.svg"},
                token=alice,
            ),
            200,
            0,
            "更新个人资料",
        )
        assert profile["data"]["nickname"] == "Alice P11", profile
        expect(
            client.call(
                "PUT",
                "/api/users/me/password",
                {"old_password": "Password123", "new_password": "NewPassword123"},
                token=alice,
            ),
            200,
            0,
            "修改密码",
        )
        alice = login(client, alice_name, "NewPassword123")
        checks += 3

        expect(
            client.call(
                "PUT", f"/api/groups/{group_id}/members/{bob_id}/role", {"role": "owner"}, token=alice
            ),
            200,
            0,
            "转让 owner",
        )
        group_after = expect(
            client.call("GET", f"/api/groups/{group_id}", token=bob),
            200,
            0,
            "转让后组资料",
        )
        assert group_after["data"]["role"] == "owner", group_after
        checks += 2

        other = expect(
            client.call("POST", "/api/groups", {"name": "P11 邀请卡片组"}, token=bob),
            200,
            0,
            "创建第二个组",
        )
        other_id = int(other["data"]["id"])
        group_ids.append(other_id)
        second_invite = expect(
            client.call(
                "POST", f"/api/groups/{other_id}/members", {"username": alice_name}, token=bob
            ),
            200,
            0,
            "发送待处理邀请",
        )
        expect(
            client.call("POST", f"/api/invites/{second_invite['data']['id']}/reject", token=alice),
            200,
            0,
            "拒绝邀请",
        )
        checks += 3

        print(f"P11 前端验收通过：{checks} 个检查点；静态资源、CSP、设置页与业务接口契约正常")
        return 0
    finally:
        if proc is not None:
            stop_server(proc)
        cleanup(root, user_ids, group_ids)


if __name__ == "__main__":
    raise SystemExit(main())
