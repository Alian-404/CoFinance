#!/usr/bin/env python3
"""P6 验收：交易、分摊、幂等、分页筛选与分类管理。"""

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
    fd, defaults_path = tempfile.mkstemp(prefix="cofinance_p6_mysql_")
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


def tx_body(type_, amount, payer, participants, split=0, category=0, note="", token=None):
    body = {
        "type": type_,
        "amount_cents": amount,
        "category_id": category,
        "note": note,
        "payer_id": payer,
        "participants": participants,
        "split_type": split,
    }
    if token is not None:
        body["client_token"] = token
    return body


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
            "COFINANCE_LOG_DIR": "/tmp/cofinance_p6_logs",
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
        names = [f"p6a{suffix}", f"p6b{suffix}", f"p6c{suffix}", f"p6d{suffix}"]
        user_ids = [
            register(client, names[0], "Alice"),
            register(client, names[1], "Bob"),
            register(client, names[2], "Carol"),
            register(client, names[3], "Dave"),
        ]
        tokens = [login(client, name) for name in names]
        alice, bob, carol, dave = user_ids

        created = expect(
            client.call("POST", "/api/groups", {"name": "账本验收组"}, token=tokens[0]),
            200,
            0,
            "创建组",
        )
        group_id = int(created["data"]["id"])
        group_ids.append(group_id)
        for username, user_token, user_id in (
            (names[1], tokens[1], bob),
            (names[2], tokens[2], carol),
        ):
            invite = expect(
                client.call(
                    "POST",
                    f"/api/groups/{group_id}/members",
                    {"username": username},
                    token=tokens[0],
                ),
                200,
                0,
                f"邀请 {username}",
            )
            expect(
                client.call(
                    "POST", f"/api/invites/{invite['data']['id']}/accept", token=user_token
                ),
                200,
                0,
                f"接受 {username}",
            )
            assert user_id > 0

        system = expect(client.call("GET", "/api/categories", token=tokens[0]), 200, 0, "系统分类")
        assert len(system["data"]["items"]) >= 9, system
        system_id = int(system["data"]["items"][0]["id"])
        expect(
            client.call("GET", "/api/categories?gid=bad", token=tokens[0]), 400, 1001, "坏 gid"
        )
        expect(
            client.call("GET", f"/api/categories?gid={group_id}", token=tokens[3]),
            403,
            1004,
            "非成员看分类",
        )
        custom = expect(
            client.call(
                "POST",
                f"/api/groups/{group_id}/categories",
                {"name": "旅行", "icon": "flight", "sort": 5},
                token=tokens[1],
            ),
            200,
            0,
            "成员新增分类",
        )
        custom_id = int(custom["data"]["id"])
        expect(
            client.call(
                "POST",
                f"/api/groups/{group_id}/categories",
                {"name": "旅行"},
                token=tokens[0],
            ),
            409,
            1016,
            "重复分类",
        )
        expect(
            client.call(
                "POST",
                f"/api/groups/{group_id}/categories",
                {"name": "越权"},
                token=tokens[3],
            ),
            403,
            1004,
            "非成员建分类",
        )
        visible = expect(
            client.call("GET", f"/api/categories?gid={group_id}", token=tokens[0]),
            200,
            0,
            "组分类",
        )
        assert any(item["id"] == custom_id for item in visible["data"]["items"]), visible

        token_one = "11111111-1111-4111-8111-111111111111"
        first_body = tx_body(
            0, 10001, alice, [alice, bob, carol], 0, custom_id, "晚餐", token_one
        )
        first = expect(
            client.call(
                "POST", f"/api/groups/{group_id}/transactions", first_body, token=tokens[0]
            ),
            200,
            0,
            "新增均分交易",
        )
        tx_one = int(first["data"]["id"])
        assert first["data"]["amount_cents"] == 10001, first
        assert [p["share_cents"] for p in first["data"]["participants"]] == [3334, 3334, 3333], first
        replay = expect(
            client.call(
                "POST", f"/api/groups/{group_id}/transactions", first_body, token=tokens[0]
            ),
            200,
            0,
            "幂等重放",
        )
        assert replay["data"]["id"] == tx_one, replay

        replay_upper = expect(
            client.call(
                "POST",
                f"/api/groups/{group_id}/transactions",
                {**first_body, "client_token": token_one.upper()},
                token=tokens[0],
            ),
            200,
            0,
            "幂等重放大写 UUID",
        )
        assert replay_upper["data"]["id"] == tx_one, replay_upper

        conflict_group = expect(
            client.call("POST", "/api/groups", {"name": "幂等冲突组"}, token=tokens[0]),
            200,
            0,
            "创建幂等冲突组",
        )
        conflict_group_id = int(conflict_group["data"]["id"])
        group_ids.append(conflict_group_id)
        expect(
            client.call(
                "POST",
                f"/api/groups/{conflict_group_id}/transactions",
                tx_body(0, 10001, alice, [alice], 0, 0, "跨组", token_one),
                token=tokens[0],
            ),
            409,
            1018,
            "幂等键跨组冲突",
        )
        expect(
            client.call(
                "POST",
                f"/api/groups/{group_id}/transactions",
                tx_body(0, 100, alice, [alice, dave]),
                token=tokens[0],
            ),
            400,
            1013,
            "非成员参与人",
        )
        expect(
            client.call(
                "POST",
                f"/api/groups/{group_id}/transactions",
                tx_body(0, 100, alice, [{"user_id": alice, "share_cents": 40}] * 1, 1),
                token=tokens[0],
            ),
            400,
            1010,
            "分摊不相等",
        )
        expect(
            client.call(
                "POST",
                f"/api/groups/{group_id}/transactions",
                tx_body(0, 100, alice, [alice], token="bad-token"),
                token=tokens[0],
            ),
            400,
            1001,
            "坏幂等键",
        )
        expect(
            client.call(
                "POST",
                f"/api/groups/{group_id}/transactions",
                tx_body(0, 100, alice, [alice]),
                token=tokens[3],
            ),
            403,
            1004,
            "非成员新增",
        )

        second = expect(
            client.call(
                "POST",
                f"/api/groups/{group_id}/transactions",
                tx_body(0, 5000, bob, [bob, carol], 0, 0, "交通"),
                token=tokens[1],
            ),
            200,
            0,
            "Bob 新增",
        )
        tx_two = int(second["data"]["id"])
        third = expect(
            client.call(
                "POST",
                f"/api/groups/{group_id}/transactions",
                tx_body(1, 20000, alice, [alice], 0, custom_id, "报销"),
                token=tokens[0],
            ),
            200,
            0,
            "Alice 新增收入",
        )
        tx_three = int(third["data"]["id"])

        page = expect(
            client.call(
                "GET", f"/api/groups/{group_id}/transactions?limit=1", token=tokens[0]
            ),
            200,
            0,
            "分页第一页",
        )
        assert len(page["data"]["items"]) == 1 and page["data"]["next_cursor"] is not None, page
        cursor = page["data"]["next_cursor"]
        page = expect(
            client.call(
                "GET",
                f"/api/groups/{group_id}/transactions?limit=1&cursor={cursor}",
                token=tokens[0],
            ),
            200,
            0,
            "分页第二页",
        )
        assert len(page["data"]["items"]) == 1 and page["data"]["next_cursor"] is not None, page
        page = expect(
            client.call(
                "GET",
                f"/api/groups/{group_id}/transactions?limit=1&cursor={page['data']['next_cursor']}",
                token=tokens[0],
            ),
            200,
            0,
            "分页第三页",
        )
        assert len(page["data"]["items"]) == 1 and page["data"]["next_cursor"] is None, page

        filtered = expect(
            client.call(
                "GET",
                f"/api/groups/{group_id}/transactions?category_id={custom_id}",
                token=tokens[0],
            ),
            200,
            0,
            "分类筛选",
        )
        assert {item["id"] for item in filtered["data"]["items"]} == {tx_one, tx_three}, filtered
        by_payer = expect(
            client.call(
                "GET", f"/api/groups/{group_id}/transactions?payer_id={bob}", token=tokens[0]
            ),
            200,
            0,
            "付款人筛选",
        )
        assert [item["id"] for item in by_payer["data"]["items"]] == [tx_two], by_payer
        ranged = expect(
            client.call(
                "GET",
                f"/api/groups/{group_id}/transactions?from=2000-01-01&to=2099-12-31",
                token=tokens[0],
            ),
            200,
            0,
            "时间筛选",
        )
        assert len(ranged["data"]["items"]) == 3, ranged
        empty = expect(
            client.call(
                "GET",
                f"/api/groups/{group_id}/transactions?from=2099-01-01",
                token=tokens[0],
            ),
            200,
            0,
            "空时间区间",
        )
        assert empty["data"]["items"] == [], empty
        expect(
            client.call(
                "GET",
                f"/api/groups/{group_id}/transactions?cursor=bad",
                token=tokens[0],
            ),
            400,
            1001,
            "坏游标",
        )
        expect(
            client.call(
                "GET",
                f"/api/groups/{group_id}/transactions?from=nope",
                token=tokens[0],
            ),
            400,
            1001,
            "坏时间",
        )
        expect(
            client.call(
                "GET", f"/api/groups/{group_id}/transactions/{tx_one}", token=tokens[3]
            ),
            403,
            1004,
            "非成员看详情",
        )
        detail = expect(
            client.call(
                "GET", f"/api/groups/{group_id}/transactions/{tx_one}", token=tokens[0]
            ),
            200,
            0,
            "交易详情",
        )
        assert len(detail["data"]["participants"]) == 3, detail

        update_body = tx_body(0, 6000, bob, [bob, carol], 0, 0, "改账")
        expect(
            client.call(
                "PUT",
                f"/api/groups/{group_id}/transactions/{tx_one}",
                update_body,
                token=tokens[1],
            ),
            403,
            1004,
            "成员改他人账",
        )
        updated = expect(
            client.call(
                "PUT",
                f"/api/groups/{group_id}/transactions/{tx_two}",
                update_body,
                token=tokens[0],
            ),
            200,
            0,
            "owner 改账",
        )
        assert [p["share_cents"] for p in updated["data"]["participants"]] == [3000, 3000], updated
        expect(
            client.call(
                "DELETE", f"/api/groups/{group_id}/transactions/{tx_one}", token=tokens[2]
            ),
            403,
            1004,
            "成员删他人账",
        )
        expect(
            client.call(
                "DELETE", f"/api/groups/{group_id}/transactions/{tx_two}", token=tokens[0]
            ),
            200,
            0,
            "owner 删账",
        )
        expect(
            client.call(
                "GET", f"/api/groups/{group_id}/transactions/{tx_two}", token=tokens[0]
            ),
            404,
            1005,
            "删除后详情",
        )
        expect(
            client.call(
                "DELETE", f"/api/groups/{group_id}/categories/{custom_id}", token=tokens[0]
            ),
            409,
            1014,
            "分类有交易保护",
        )
        expect(
            client.call(
                "DELETE", f"/api/groups/{group_id}/categories/{system_id}", token=tokens[0]
            ),
            403,
            1004,
            "系统分类不可删",
        )
        expect(
            client.call(
                "DELETE", f"/api/groups/{group_id}/transactions/{tx_one}", token=tokens[0]
            ),
            200,
            0,
            "删除第一笔",
        )
        expect(
            client.call(
                "DELETE", f"/api/groups/{group_id}/transactions/{tx_three}", token=tokens[0]
            ),
            200,
            0,
            "删除第三笔",
        )
        expect(
            client.call(
                "DELETE", f"/api/groups/{group_id}/categories/{custom_id}", token=tokens[1]
            ),
            200,
            0,
            "删除空分类",
        )
        expect(
            client.call(
                "GET", f"/api/groups/{group_id}/transactions", token=tokens[3]
            ),
            403,
            1004,
            "非成员列表",
        )

        print("P6 ledger integration: 分类/交易/分摊/幂等/分页/筛选/权限 OK")
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
