#!/usr/bin/env python3
"""P8 验收：聊天消息、历史分页、已读水位线与事件驱动长轮询。"""

import argparse
import http.client
import json
import os
import selectors
import shutil
import signal
import socket
import subprocess
import tempfile
import threading
import time
from pathlib import Path
from urllib.parse import urlencode


def pick_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def wait_ready(proc: subprocess.Popen, port: int, timeout: float = 10.0) -> None:
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

    def call(self, method: str, path: str, body=None, token=None):
        status, parsed = self.raw_call(method, path, body, token)
        assert parsed.get("code") is not None and "msg" in parsed and "data" in parsed, parsed
        return status, parsed

    def raw_call(self, method: str, path: str, body=None, token=None):
        headers = {"Content-Type": "application/json", "Connection": "close"}
        payload = None if body is None else json.dumps(body, ensure_ascii=False).encode("utf-8")
        if token is not None:
            headers["Authorization"] = f"Bearer {token}"
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=self.timeout)
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


def poll_path(group_id: int, after_id=None, timeout: int = 5, limit: int = 100) -> str:
    query = {"timeout": timeout, "limit": limit}
    if after_id is not None:
        query["after_id"] = after_id
    return f"/api/groups/{group_id}/messages/poll?{urlencode(query)}"


def start_poll(port: int, token: str, path: str, read_timeout: float = 10.0) -> socket.socket:
    sock = socket.create_connection(("127.0.0.1", port), timeout=read_timeout)
    request = (
        f"GET {path} HTTP/1.1\r\n"
        f"Host: 127.0.0.1:{port}\r\n"
        f"Authorization: Bearer {token}\r\n"
        "Connection: close\r\n\r\n"
    ).encode("ascii")
    sock.sendall(request)
    return sock


def read_http_json(sock: socket.socket) -> tuple[int, dict]:
    data = bytearray()
    header_end = -1
    content_length = 0
    while header_end < 0:
        chunk = sock.recv(4096)
        if not chunk:
            raise AssertionError("响应在头部完成前断开")
        data.extend(chunk)
        header_end = data.find(b"\r\n\r\n")
    header = bytes(data[:header_end]).decode("latin1")
    lines = header.split("\r\n")
    status = int(lines[0].split(" ", 2)[1])
    for line in lines[1:]:
        name, _, value = line.partition(":")
        if name.lower() == "content-length":
            content_length = int(value.strip())
    body_start = header_end + 4
    while len(data) - body_start < content_length:
        chunk = sock.recv(65536)
        if not chunk:
            raise AssertionError("响应 body 未读完即断开")
        data.extend(chunk)
    body = bytes(data[body_start : body_start + content_length]).decode("utf-8")
    sock.close()
    return status, json.loads(body)


def run_stress(port: int, token: str, group_id: int, count: int) -> None:
    print(f"[stress] 建立 {count} 条长轮询挂起连接...")
    client = Client(port, timeout=30)
    history = expect(
        client.call("GET", f"/api/groups/{group_id}/messages?limit=1", token=token),
        200,
        0,
        "压测读取最大消息 id",
    )
    after_id = int(history["data"]["server_max_id"])
    sockets = []
    try:
        for _ in range(count):
            sockets.append(start_poll(port, token, poll_path(group_id, after_id, 30, 100), 40))
        # 2000 个新连接各自需要完成初始鉴权/快照查询；等指标确认全部进入挂起态，
        # 避免把“建立连接”的数据库排队时间算进消息送达延迟。
        registered_start = time.monotonic()
        deadline = registered_start + 40
        while time.monotonic() < deadline:
            metrics = expect(
                client.call("GET", "/api/metrics", token=token),
                200,
                0,
                "读取挂起数",
            )
            if int(metrics["data"]["pending_polls"]) >= count:
                break
            time.sleep(0.1)
        else:
            metrics = expect(
                client.call("GET", "/api/metrics", token=token), 200, 0, "读取挂起数"
            )
            raise AssertionError(f"长轮询未全部登记: {metrics['data']}")
        registration_ms = (time.monotonic() - registered_start) * 1000
        print(f"[stress] {count} 条已全部登记，耗时 {registration_ms:.1f}ms")
        print("[stress] 全部请求已发送，触发一条新消息")
        started = time.monotonic()
        sent = expect(
            client.call(
                "POST",
                f"/api/groups/{group_id}/messages",
                {"content": "P8 2000 并发唤醒"},
                token=token,
            ),
            200,
            0,
            "压测发送消息",
        )
        send_finished = time.monotonic()
        post_ms = (send_finished - started) * 1000
        message_id = int(sent["data"]["id"])

        selector = selectors.DefaultSelector()
        for sock in sockets:
            selector.register(sock, selectors.EVENT_READ)
        completed = 0
        first_response_at = None
        buffers = {sock.fileno(): bytearray() for sock in sockets}
        headers_done = {sock.fileno(): False for sock in sockets}
        expected_lengths = {sock.fileno(): 0 for sock in sockets}
        body_offsets = {sock.fileno(): 0 for sock in sockets}
        deadline = time.monotonic() + 30
        while completed < count and time.monotonic() < deadline:
            events = selector.select(timeout=1.0)
            if not events:
                continue
            for key, _ in events:
                sock = key.fileobj
                fd = sock.fileno()
                chunk = sock.recv(65536)
                if not chunk:
                    raise AssertionError(f"长轮询 {completed}/{count} 提前断开")
                buffers[fd].extend(chunk)
                if not headers_done[fd]:
                    header_end = buffers[fd].find(b"\r\n\r\n")
                    if header_end < 0:
                        continue
                    header = bytes(buffers[fd][:header_end]).decode("latin1").split("\r\n")
                    status = int(header[0].split(" ", 2)[1])
                    assert status == 200, header[0]
                    for line in header[1:]:
                        name, _, value = line.partition(":")
                        if name.lower() == "content-length":
                            expected_lengths[fd] = int(value.strip())
                    headers_done[fd] = True
                    body_offsets[fd] = header_end + 4
                if len(buffers[fd]) - body_offsets[fd] < expected_lengths[fd]:
                    continue
                body = json.loads(
                    bytes(
                        buffers[fd][body_offsets[fd] : body_offsets[fd] + expected_lengths[fd]]
                    ).decode("utf-8")
                )
                assert body["code"] == 0, body
                items = body["data"]["items"]
                assert len(items) == 1 and int(items[0]["id"]) == message_id, body
                selector.unregister(sock)
                sock.close()
                completed += 1
                if first_response_at is None:
                    first_response_at = time.monotonic()
        elapsed = time.monotonic() - started
        assert completed == count, f"仅收到 {completed}/{count} 条唤醒响应"
        first_ms = (first_response_at - send_finished) * 1000 if first_response_at else -1
        print(
            f"[stress] 通过：{count}/{count} 条全部送达，POST {post_ms:.1f}ms，"
            f"发送响应后首条 {first_ms:.1f}ms，全部 {elapsed * 1000:.1f}ms"
        )
        if first_ms > 100:
            print(f"[stress][WARN] 首条送达超过 100ms：{first_ms:.1f}ms")
    finally:
        selector = locals().get("selector")
        if selector is not None:
            selector.close()
        for sock in sockets:
            try:
                sock.close()
            except OSError:
                pass


def cleanup(root: Path, user_ids: list[int], group_ids: list[int]) -> None:
    mysql = shutil.which("mysql")
    if not mysql:
        return
    server = load_kv(root / "config/server.conf")
    secrets = load_kv(root / "config/secrets.conf")
    fd, defaults_path = tempfile.mkstemp(prefix="cofinance_p8_mysql_")
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
                    f"DELETE FROM sessions WHERE user_id IN ({users});",
                    f"DELETE FROM group_members WHERE user_id IN ({users});",
                    f"DELETE FROM users WHERE id IN ({users});",
                ]
            )
        subprocess.run(
            [mysql, f"--defaults-extra-file={defaults_path}", "-e", " ".join(statements)],
            check=False,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
    finally:
        try:
            os.unlink(defaults_path)
        except OSError:
            pass


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", default="build/cofinance")
    parser.add_argument("--valgrind", action="store_true")
    parser.add_argument("--stress", type=int, default=0, help="额外建立 N 条并发挂起连接")
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[2]
    port = pick_port()
    stress = max(0, args.stress)
    env = os.environ.copy()
    env.update(
        {
            "COFINANCE_SERVER_PORT": str(port),
            "COFINANCE_LOG_LEVEL": "WARN",
            "COFINANCE_LOG_TO_TERMINAL": "0",
            "COFINANCE_LOG_DIR": "/tmp/cofinance_p8_logs",
        }
    )
    if stress:
        env.update(
            {
                "COFINANCE_SERVER_LONG_POLL_MAX_PER_USER": str(stress + 100),
                "COFINANCE_SERVER_LONG_POLL_MAX_PER_GROUP": str(stress + 100),
                "COFINANCE_SERVER_LONG_POLL_MAX_TOTAL": str(stress + 200),
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
    client = Client(port, timeout=60 if args.valgrind else 8)
    user_ids: list[int] = []
    group_ids: list[int] = []
    try:
        wait_ready(proc, port, timeout=60 if args.valgrind else 10)
        suffix = f"{os.getpid() % 100000}{int(time.time() * 1000) % 1000000}"
        names = [f"p8a{suffix}", f"p8b{suffix}", f"p8c{suffix}"]
        user_ids = [
            register(client, names[0], "Alice"),
            register(client, names[1], "Bob"),
            register(client, names[2], "Carol"),
        ]
        tokens = [login(client, name) for name in names]
        alice, bob, _ = user_ids

        created = expect(
            client.call("POST", "/api/groups", {"name": "P8 聊天组"}, token=tokens[0]),
            200,
            0,
            "创建聊天组",
        )
        group_id = int(created["data"]["id"])
        group_ids.append(group_id)
        invite = expect(
            client.call(
                "POST",
                f"/api/groups/{group_id}/members",
                {"username": names[1]},
                token=tokens[0],
            ),
            200,
            0,
            "邀请 Bob",
        )
        expect(
            client.call(
                "POST", f"/api/invites/{invite['data']['id']}/accept", token=tokens[1]
            ),
            200,
            0,
            "Bob 接受邀请",
        )

        first = expect(
            client.call(
                "POST",
                f"/api/groups/{group_id}/messages",
                {"content": "第一条消息"},
                token=tokens[0],
            ),
            200,
            0,
            "发送第一条消息",
        )
        first_id = int(first["data"]["id"])
        idempotent_uuid = "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"
        second = expect(
            client.call(
                "POST",
                f"/api/groups/{group_id}/messages",
                {"content": "幂等消息", "client_msg_id": idempotent_uuid},
                token=tokens[0],
            ),
            200,
            0,
            "发送幂等消息",
        )
        second_id = int(second["data"]["id"])
        replay = expect(
            client.call(
                "POST",
                f"/api/groups/{group_id}/messages",
                {"content": "不应重复", "client_msg_id": idempotent_uuid},
                token=tokens[0],
            ),
            200,
            0,
            "重放幂等消息",
        )
        assert int(replay["data"]["id"]) == second_id
        assert replay["data"]["content"] == "幂等消息"

        history = expect(
            client.call(
                "GET", f"/api/groups/{group_id}/messages?limit=1", token=tokens[1]
            ),
            200,
            0,
            "历史消息第一页",
        )
        assert [int(item["id"]) for item in history["data"]["items"]] == [second_id]
        assert int(history["data"]["server_max_id"]) == second_id
        older = expect(
            client.call(
                "GET",
                f"/api/groups/{group_id}/messages?cursor={history['data']['next_cursor']}",
                token=tokens[1],
            ),
            200,
            0,
            "历史消息下一页",
        )
        assert [int(item["id"]) for item in older["data"]["items"]] == [first_id]
        after = expect(
            client.call(
                "GET", f"/api/groups/{group_id}/messages?after_id={first_id}", token=tokens[1]
            ),
            200,
            0,
            "增量消息",
        )
        assert [int(item["id"]) for item in after["data"]["items"]] == [second_id]
        expect(
            client.call(
                "POST",
                f"/api/groups/{group_id}/messages",
                {"content": "越权"},
                token=tokens[2],
            ),
            403,
            1004,
            "非成员发送消息",
        )
        expect(
            client.call("GET", f"/api/groups/{group_id}/messages", token=tokens[2]),
            403,
            1004,
            "非成员读取消息",
        )

        unread = expect(
            client.call(
                "GET", f"/api/groups/{group_id}/messages/unread", token=tokens[1]
            ),
            200,
            0,
            "读取未读数",
        )
        assert int(unread["data"]["unread_count"]) == 2
        read = expect(
            client.call(
                "POST",
                f"/api/groups/{group_id}/messages/read",
                {"last_read_message_id": first_id},
                token=tokens[1],
            ),
            200,
            0,
            "上报已读水位线",
        )
        assert int(read["data"]["last_read_message_id"]) == first_id
        assert int(read["data"]["unread_count"]) == 1
        read_older = expect(
            client.call(
                "POST",
                f"/api/groups/{group_id}/messages/read",
                {"last_read_message_id": 1},
                token=tokens[1],
            ),
            200,
            0,
            "已读水位线不回退",
        )
        assert int(read_older["data"]["last_read_message_id"]) == first_id

        current_max = int(history["data"]["server_max_id"])
        poll_socket = start_poll(
            port, tokens[1], poll_path(group_id, current_max, timeout=5)
        )
        time.sleep(0.25)
        started = time.monotonic()
        delivered = expect(
            client.call(
                "POST",
                f"/api/groups/{group_id}/messages",
                {"content": "长轮询即时送达"},
                token=tokens[0],
            ),
            200,
            0,
            "长轮询期间发消息",
        )
        poll_status, poll_body = read_http_json(poll_socket)
        elapsed_ms = (time.monotonic() - started) * 1000
        assert poll_status == 200 and poll_body["code"] == 0, poll_body
        assert [int(item["id"]) for item in poll_body["data"]["items"]] == [
            int(delivered["data"]["id"])
        ]
        assert elapsed_ms < 2000, elapsed_ms

        no_cursor_socket = start_poll(port, tokens[1], poll_path(group_id, None, timeout=5))
        time.sleep(0.25)
        only_new = expect(
            client.call(
                "POST",
                f"/api/groups/{group_id}/messages",
                {"content": "缺省游标只拉新消息"},
                token=tokens[0],
            ),
            200,
            0,
            "缺省游标期间发消息",
        )
        _, no_cursor_body = read_http_json(no_cursor_socket)
        assert [int(item["id"]) for item in no_cursor_body["data"]["items"]] == [
            int(only_new["data"]["id"])
        ]

        timeout_started = time.monotonic()
        timeout_socket = start_poll(port, tokens[1], poll_path(group_id, None, timeout=1))
        timeout_status, timeout_body = read_http_json(timeout_socket)
        timeout_elapsed = time.monotonic() - timeout_started
        assert timeout_status == 200 and timeout_body["code"] == 0
        assert timeout_body["data"]["items"] == []
        assert timeout_elapsed >= 0.8, timeout_elapsed

        if not stress:
            limit_sockets = [
                start_poll(port, tokens[1], poll_path(group_id, None, timeout=5))
                for _ in range(5)
            ]
            time.sleep(0.35)
            expect(
                client.call(
                    "GET", poll_path(group_id, None, timeout=5), token=tokens[1]
                ),
                429,
                3002,
                "同一用户第 6 条挂起连接",
            )
            wake_all = expect(
                client.call(
                    "POST",
                    f"/api/groups/{group_id}/messages",
                    {"content": "唤醒五条连接"},
                    token=tokens[0],
                ),
                200,
                0,
                "唤醒五条长轮询",
            )
            for sock in limit_sockets:
                status, body = read_http_json(sock)
                assert status == 200 and body["code"] == 0
                assert [int(item["id"]) for item in body["data"]["items"]] == [
                    int(wake_all["data"]["id"])
                ]
        else:
            print("[stress] 配额验收已在默认模式单独执行，本模式提高上限用于压测")

        disconnected = start_poll(port, tokens[1], poll_path(group_id, None, timeout=30))
        disconnected.close()
        time.sleep(0.25)
        expect(
            client.call(
                "POST",
                f"/api/groups/{group_id}/messages",
                {"content": "断开后继续工作"},
                token=tokens[0],
            ),
            200,
            0,
            "断开清理后发消息",
        )
        health_status, health_body = client.raw_call("GET", "/api/health")
        assert health_status == 200 and health_body.get("code") == 0, health_body

        groups = expect(
            client.call("GET", "/api/groups", token=tokens[1]), 200, 0, "组列表未读数"
        )
        summary = next(item for item in groups["data"]["items"] if int(item["id"]) == group_id)
        assert summary["last_message"]["content"] == "断开后继续工作"
        assert "unread_count" in summary and summary["unread_count"] >= 1

        if stress:
            run_stress(port, tokens[0], group_id, stress)

        print(f"P8 HTTP 验收通过：用户={len(user_ids)} 组={len(group_ids)}")
        return 0
    finally:
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=30 if args.valgrind else 15)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        cleanup(root, user_ids, group_ids)
        if proc.returncode not in (0, -15, 143):
            stderr = proc.stderr.read() if proc.stderr else ""
            raise RuntimeError(f"服务异常退出 {proc.returncode}: {stderr}")


if __name__ == "__main__":
    raise SystemExit(main())
