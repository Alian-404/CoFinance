#!/usr/bin/env python3
"""P7 验收：统计、净额、最少笔数建议、结算复核与生命周期余额闸门。"""

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
    fd, defaults_path = tempfile.mkstemp(prefix="cofinance_p7_mysql_")
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
            statements.append(f"DELETE FROM settlements WHERE group_id IN ({groups});")
            statements.append(f"DELETE FROM messages WHERE group_id IN ({groups});")
            statements.append(
                "DELETE p FROM transaction_participants p JOIN transactions t "
                f"ON t.id = p.transaction_id WHERE t.group_id IN ({groups});"
            )
            statements.append(f"DELETE FROM transactions WHERE group_id IN ({groups});")
            statements.append(f"DELETE FROM categories WHERE group_id IN ({groups});")
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
        f"注册 {username}",
    )
    return int(body["data"]["id"])


def login(client: Client, username: str) -> str:
    body = expect(
        client.call(
            "POST", "/api/auth/login", {"username": username, "password": "Password123"}
        ),
        200,
        0,
        f"登录 {username}",
    )
    return body["data"]["token"]


def tx_body(type_, amount, payer, participants, category=0):
    return {
        "type": type_,
        "amount_cents": amount,
        "category_id": category,
        "note": "P7 验收",
        "payer_id": payer,
        "participants": [{"user_id": user_id} for user_id in participants],
        "split_type": 0,
    }


def balance_of(body, user_id: int) -> dict:
    matches = [item for item in body["data"]["items"] if item["user_id"] == user_id]
    assert len(matches) == 1, body
    return matches[0]


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
            "COFINANCE_LOG_DIR": "/tmp/cofinance_p7_logs",
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
        names = [f"p7a{suffix}", f"p7b{suffix}", f"p7c{suffix}", f"p7d{suffix}"]
        user_ids = [
            register(client, names[0], "Alice"),
            register(client, names[1], "Bob"),
            register(client, names[2], "Carol"),
            register(client, names[3], "Dave"),
        ]
        tokens = [login(client, name) for name in names]
        alice, bob, carol, dave = user_ids

        created = expect(
            client.call("POST", "/api/groups", {"name": "P7 统计结算组"}, token=tokens[0]),
            200,
            0,
            "创建结算组",
        )
        group_id = int(created["data"]["id"])
        group_ids.append(group_id)
        for index in (1, 2):
            invite = expect(
                client.call(
                    "POST",
                    f"/api/groups/{group_id}/members",
                    {"username": names[index]},
                    token=tokens[0],
                ),
                200,
                0,
                f"邀请 {names[index]}",
            )
            expect(
                client.call(
                    "POST", f"/api/invites/{invite['data']['id']}/accept", token=tokens[index]
                ),
                200,
                0,
                f"接受邀请 {names[index]}",
            )
        categories = expect(
            client.call("GET", "/api/categories", token=tokens[0]), 200, 0, "系统分类"
        )
        category_id = int(categories["data"]["items"][0]["id"])

        expect(
            client.call(
                "POST",
                f"/api/groups/{group_id}/transactions",
                tx_body(0, 9000, alice, [alice, bob, carol], category_id),
                token=tokens[0],
            ),
            200,
            0,
            "支出 9000",
        )
        expect(
            client.call(
                "POST",
                f"/api/groups/{group_id}/transactions",
                tx_body(1, 3000, bob, [alice, bob], category_id),
                token=tokens[1],
            ),
            200,
            0,
            "收入 3000",
        )

        balances = expect(
            client.call("GET", f"/api/groups/{group_id}/balances", token=tokens[0]),
            200,
            0,
            "净额列表",
        )
        assert balances["data"]["total_balance_cents"] == 0, balances
        assert balance_of(balances, alice)["balance_cents"] == 7500, balances
        assert balance_of(balances, bob)["balance_cents"] == -4500, balances
        assert balance_of(balances, carol)["balance_cents"] == -3000, balances
        expect(
            client.call("GET", f"/api/groups/{group_id}/balances", token=tokens[3]),
            403,
            1004,
            "非成员看余额",
        )
        expect(
            client.call("GET", "/api/groups/bad/balances", token=tokens[0]),
            400,
            1001,
            "坏组 ID",
        )

        stats = expect(
            client.call("GET", f"/api/groups/{group_id}/stats", token=tokens[0]),
            200,
            0,
            "统计",
        )
        assert stats["data"]["summary"]["total_expense_cents"] == 9000, stats
        assert stats["data"]["summary"]["total_income_cents"] == 3000, stats
        assert stats["data"]["summary"]["net_expense_cents"] == 6000, stats
        assert stats["data"]["summary"]["transaction_count"] == 2, stats
        assert stats["data"]["expense_categories"][0]["amount_cents"] == 9000, stats
        assert stats["data"]["income_categories"][0]["amount_cents"] == 3000, stats
        bob_stats = [item for item in stats["data"]["members"] if item["user_id"] == bob][0]
        assert bob_stats["transaction_balance_cents"] == -4500, bob_stats
        empty_stats = expect(
            client.call(
                "GET",
                f"/api/groups/{group_id}/stats?from=2000-01-01&to=2000-01-31",
                token=tokens[0],
            ),
            200,
            0,
            "空时间统计",
        )
        assert empty_stats["data"]["summary"]["transaction_count"] == 0, empty_stats
        expect(
            client.call(
                "GET", f"/api/groups/{group_id}/stats?from=bad", token=tokens[0]
            ),
            400,
            1001,
            "坏时间",
        )
        expect(
            client.call("GET", f"/api/groups/{group_id}/stats", token=tokens[3]),
            403,
            1004,
            "非成员看统计",
        )

        suggestions = expect(
            client.call(
                "POST", f"/api/groups/{group_id}/settlements/suggest", token=tokens[0]
            ),
            200,
            0,
            "结算建议",
        )
        items = suggestions["data"]["items"]
        assert len(items) == 2, suggestions
        assert items[0]["from_user_id"] == bob and items[0]["amount_cents"] == 4500, items
        assert items[1]["from_user_id"] == carol and items[1]["amount_cents"] == 3000, items
        expect(
            client.call(
                "POST", f"/api/groups/{group_id}/settlements/suggest", token=tokens[3]
            ),
            403,
            1004,
            "非成员看建议",
        )
        expect(
            client.call("DELETE", "/api/users/me", token=tokens[1]),
            409,
            1015,
            "有余额注销账号",
        )

        def settle(from_id: int, to_id: int, amount: int, token: str, status=200, code=0):
            return expect(
                client.call(
                    "POST",
                    f"/api/groups/{group_id}/settlements",
                    {"from_user_id": from_id, "to_user_id": to_id, "amount_cents": amount},
                    token=token,
                ),
                status,
                code,
                f"结算 {from_id}->{to_id} {amount}",
            )

        settle(bob, alice, 4501, tokens[0], 409, 1015)
        settle(carol, bob, 100, tokens[0], 409, 1015)
        settle(bob, bob, 100, tokens[0], 400, 1001)
        settle(dave, alice, 100, tokens[0], 400, 1001)
        first = settle(bob, alice, 4000, tokens[0])
        assert first["data"]["status"] == 1 and first["data"]["amount_cents"] == 4000, first
        partial = expect(
            client.call("GET", f"/api/groups/{group_id}/balances", token=tokens[0]),
            200,
            0,
            "部分结算后余额",
        )
        assert balance_of(partial, bob)["balance_cents"] == -500, partial
        settle(bob, alice, 1000, tokens[0], 409, 1015)
        settle(bob, alice, 500, tokens[0])
        settle(carol, alice, 3000, tokens[0])
        settled = expect(
            client.call("GET", f"/api/groups/{group_id}/balances", token=tokens[0]),
            200,
            0,
            "结清后余额",
        )
        assert all(item["balance_cents"] == 0 for item in settled["data"]["items"]), settled
        history = expect(
            client.call("GET", f"/api/groups/{group_id}/settlements", token=tokens[0]),
            200,
            0,
            "结算历史",
        )
        assert len(history["data"]["items"]) == 3, history
        expect(
            client.call("GET", f"/api/groups/{group_id}/settlements", token=tokens[3]),
            403,
            1004,
            "非成员看历史",
        )

        lifecycle = expect(
            client.call("POST", "/api/groups", {"name": "P7 生命周期组"}, token=tokens[0]),
            200,
            0,
            "创建生命周期组",
        )
        lifecycle_id = int(lifecycle["data"]["id"])
        group_ids.append(lifecycle_id)
        for index in (1, 2):
            invite = expect(
                client.call(
                    "POST",
                    f"/api/groups/{lifecycle_id}/members",
                    {"username": names[index]},
                    token=tokens[0],
                ),
                200,
                0,
                f"生命周期邀请 {names[index]}",
            )
            expect(
                client.call(
                    "POST", f"/api/invites/{invite['data']['id']}/accept", token=tokens[index]
                ),
                200,
                0,
                f"生命周期接受 {names[index]}",
            )
        expect(
            client.call(
                "POST",
                f"/api/groups/{lifecycle_id}/transactions",
                tx_body(0, 1000, alice, [alice, bob], category_id),
                token=tokens[0],
            ),
            200,
            0,
            "生命周期支出",
        )
        expect(
            client.call(
                "POST", f"/api/groups/{lifecycle_id}/members/leave", token=tokens[1]
            ),
            409,
            1015,
            "有余额退组",
        )
        expect(
            client.call(
                "DELETE", f"/api/groups/{lifecycle_id}/members/{bob}", token=tokens[0]
            ),
            409,
            1015,
            "有余额移除",
        )
        expect(
            client.call(
                "POST",
                f"/api/groups/{lifecycle_id}/settlements",
                {"from_user_id": bob, "to_user_id": alice, "amount_cents": 500},
                token=tokens[1],
            ),
            200,
            0,
            "生命周期结算",
        )
        expect(
            client.call(
                "POST", f"/api/groups/{lifecycle_id}/members/leave", token=tokens[1]
            ),
            200,
            0,
            "结清后退组",
        )
        expect(
            client.call(
                "POST",
                f"/api/groups/{lifecycle_id}/transactions",
                tx_body(0, 1000, alice, [alice, carol], category_id),
                token=tokens[0],
            ),
            200,
            0,
            "制造未结清余额",
        )
        expect(
            client.call(
                "DELETE", f"/api/groups/{lifecycle_id}", token=tokens[2]
            ),
            403,
            1004,
            "非 owner 强制解散",
        )
        expect(
            client.call(
                "DELETE", f"/api/groups/{lifecycle_id}", token=tokens[0]
            ),
            409,
            1015,
            "有余额默认解散",
        )
        expect(
            client.call(
                "DELETE", f"/api/groups/{lifecycle_id}?force=maybe", token=tokens[0]
            ),
            400,
            1001,
            "非法 force",
        )
        expect(
            client.call(
                "DELETE", f"/api/groups/{lifecycle_id}?force=true", token=tokens[0]
            ),
            200,
            0,
            "强制解散",
        )
        expect(
            client.call("GET", f"/api/groups/{lifecycle_id}/balances", token=tokens[0]),
            404,
            1005,
            "解散后不可查余额",
        )

        empty_group = expect(
            client.call("POST", "/api/groups", {"name": "P7 零余额解散"}, token=tokens[0]),
            200,
            0,
            "创建零余额组",
        )
        empty_id = int(empty_group["data"]["id"])
        group_ids.append(empty_id)
        expect(
            client.call("DELETE", f"/api/groups/{empty_id}", token=tokens[0]),
            200,
            0,
            "零余额默认解散",
        )

        print(f"P7 HTTP 验收通过：用户={len(user_ids)} 组={len(group_ids)}")
        return 0
    finally:
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        cleanup(root, user_ids, group_ids)
        if proc.returncode not in (0, -15, 143):
            stderr = proc.stderr.read() if proc.stderr else ""
            raise RuntimeError(f"服务异常退出 {proc.returncode}: {stderr}")


if __name__ == "__main__":
    raise SystemExit(main())
