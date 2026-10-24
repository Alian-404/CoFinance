#!/usr/bin/env python3
"""P9 验收：WebSocket 握手、双向消息、分片、心跳帧、断线补拉与订阅清理。"""

import argparse
import base64
import hashlib
import http.client
import json
import os
import shutil
import signal
import socket
import struct
import subprocess
import tempfile
import time
from pathlib import Path
from urllib.parse import urlencode


WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


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
    raise RuntimeError("等待服务监听超时")


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
        parsed = json.loads(raw.decode("utf-8"))
        assert parsed.get("code") is not None and "msg" in parsed and "data" in parsed, parsed
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


def recv_until(sock: socket.socket, marker: bytes) -> tuple[bytes, bytes]:
    data = bytearray()
    while marker not in data:
        chunk = sock.recv(4096)
        if not chunk:
            raise AssertionError("连接在响应头完成前关闭")
        data.extend(chunk)
    index = data.find(marker) + len(marker)
    return bytes(data[:index]), bytes(data[index:])


def parse_http_response(raw: bytes) -> tuple[int, dict, bytes]:
    head, _, rest = raw.partition(b"\r\n\r\n")
    lines = head.decode("latin1").split("\r\n")
    status = int(lines[0].split(" ", 2)[1])
    headers = {}
    for line in lines[1:]:
        name, _, value = line.partition(":")
        headers[name.strip().lower()] = value.strip()
    return status, headers, rest


def handshake(
    port: int, token: str | None, group_id: int, upgrade: bool = True
) -> tuple[socket.socket, int, dict, bytes, str]:
    sock = socket.create_connection(("127.0.0.1", port), timeout=5)
    key = base64.b64encode(os.urandom(16)).decode("ascii")
    query = {"group_id": group_id}
    if token is not None:
        query["token"] = token
    headers = [f"GET /api/ws?{urlencode(query)} HTTP/1.1", f"Host: 127.0.0.1:{port}"]
    if upgrade:
        headers.extend(
            [
                "Upgrade: websocket",
                "Connection: Upgrade",
                f"Sec-WebSocket-Key: {key}",
                "Sec-WebSocket-Version: 13",
            ]
        )
    request = "\r\n".join(headers) + "\r\n\r\n"
    sock.sendall(request.encode("ascii"))
    raw, rest = recv_until(sock, b"\r\n\r\n")
    status, response_headers, _ = parse_http_response(raw)
    body = rest
    if status != 101:
        length = int(response_headers.get("content-length", "0"))
        while len(body) < length:
            chunk = sock.recv(length - len(body))
            if not chunk:
                raise AssertionError(
                    f"HTTP 拒绝响应 body 未读完即关闭: headers={response_headers} body={body!r}"
                )
            body += chunk
        body = body[:length]
    return sock, status, response_headers, body, key


class WSClient:
    def __init__(self, sock: socket.socket, headers: dict, leftover: bytes, key: str):
        self.sock = sock
        self.buffer = bytearray(leftover)
        assert headers.get("upgrade", "").lower() == "websocket", headers
        assert headers.get("connection", "").lower() == "upgrade", headers
        accept = base64.b64encode(
            hashlib.sha1((key + WS_GUID).encode()).digest()
        ).decode()
        assert headers.get("sec-websocket-accept") == accept, (headers, accept)
        self.sock.settimeout(3)

    def send_frame(self, opcode: int, payload: bytes, fin: bool = True) -> None:
        first = (0x80 if fin else 0) | opcode
        mask = os.urandom(4)
        length = len(payload)
        if length < 126:
            header = bytes([first, 0x80 | length])
        elif length <= 0xFFFF:
            header = bytes([first, 0x80 | 126]) + struct.pack("!H", length)
        else:
            header = bytes([first, 0x80 | 127]) + struct.pack("!Q", length)
        masked = bytes(byte ^ mask[index % 4] for index, byte in enumerate(payload))
        self.sock.sendall(header + mask + masked)

    def send_json(self, value: dict) -> None:
        self.send_frame(1, json.dumps(value, ensure_ascii=False).encode("utf-8"))

    def _recv_exact(self, size: int) -> bytes:
        while len(self.buffer) < size:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise EOFError("WebSocket 连接已关闭")
            self.buffer.extend(chunk)
        out = bytes(self.buffer[:size])
        del self.buffer[:size]
        return out

    def recv_frame(self, timeout: float | None = None) -> tuple[int, bytes]:
        old_timeout = self.sock.gettimeout()
        self.sock.settimeout(timeout)
        try:
            first, second = self._recv_exact(2)
            assert second & 0x80 == 0, "服务端帧不应带掩码"
            opcode = first & 0x0F
            length = second & 0x7F
            if length == 126:
                length = struct.unpack("!H", self._recv_exact(2))[0]
            elif length == 127:
                length = struct.unpack("!Q", self._recv_exact(8))[0]
            return opcode, self._recv_exact(length)
        finally:
            self.sock.settimeout(old_timeout)

    def recv_json(self, predicate=None, timeout: float = 3.0) -> dict:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            remaining = max(0.01, deadline - time.monotonic())
            opcode, payload = self.recv_frame(remaining)
            if opcode == 0x9:
                self.send_frame(0xA, payload)
                continue
            if opcode == 0x8:
                raise AssertionError(f"收到关闭帧: {payload!r}")
            assert opcode == 0x1, opcode
            value = json.loads(payload.decode("utf-8"))
            if predicate is None or predicate(value):
                return value
        raise AssertionError("等待 WebSocket JSON 事件超时")

    def expect_ready(self) -> dict:
        ready = self.recv_json(lambda value: value.get("type") == "ready")
        assert int(ready["server_max_id"]) >= 0
        return ready

    def assert_silent(self, timeout: float = 0.6) -> None:
        try:
            opcode, payload = self.recv_frame(timeout)
        except socket.timeout:
            return
        raise AssertionError(f"预期无推送，实际收到 opcode={opcode} payload={payload!r}")

    def expect_close(self, code: int = 1000, timeout: float = 3.0) -> None:
        self.send_frame(0x8, struct.pack("!H", code))
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            opcode, payload = self.recv_frame(max(0.01, deadline - time.monotonic()))
            if opcode == 0x9:
                self.send_frame(0xA, payload)
                continue
            if opcode == 0x8:
                assert len(payload) >= 2
                assert struct.unpack("!H", payload[:2])[0] == code, payload
                return

    def close(self, code: int = 1000) -> None:
        try:
            self.send_frame(0x8, struct.pack("!H", code))
        except OSError:
            pass
        self.sock.close()


def wait_metric(client: Client, token: str, expected: int, timeout: float = 5.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        body = expect(client.call("GET", "/api/metrics", token=token), 200, 0, "读取实时连接数")
        if int(body["data"]["realtime_connections"]) == expected:
            return
        time.sleep(0.05)
    raise AssertionError(f"realtime_connections 未达到 {expected}")


def cleanup(root: Path, user_ids: list[int], group_ids: list[int]) -> None:
    mysql = shutil.which("mysql")
    if not mysql:
        return
    server = load_kv(root / "config/server.conf")
    secrets = load_kv(root / "config/secrets.conf")
    fd, defaults_path = tempfile.mkstemp(prefix="cofinance_p9_mysql_")
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
                    f"DELETE FROM messages WHERE group_id IN ({groups});",
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
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[2]
    port = pick_port()
    env = os.environ.copy()
    env.update(
        {
            "COFINANCE_SERVER_PORT": str(port),
            "COFINANCE_LOG_LEVEL": "WARN",
            "COFINANCE_LOG_TO_TERMINAL": "0",
            "COFINANCE_LOG_DIR": "/tmp/cofinance_p9_logs",
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
    client = Client(port, timeout=20 if args.valgrind else 8)
    user_ids: list[int] = []
    group_ids: list[int] = []
    sockets: list[socket.socket] = []
    try:
        wait_ready(proc, port, timeout=60 if args.valgrind else 10)
        suffix = f"{os.getpid() % 100000}{int(time.time() * 1000) % 1000000}"
        names = [f"p9a{suffix}", f"p9b{suffix}", f"p9c{suffix}"]
        user_ids = [register(client, name, name[-1].upper()) for name in names]
        tokens = [login(client, name) for name in names]
        alice, bob, carol = user_ids

        created = expect(
            client.call("POST", "/api/groups", {"name": "P9 实时组"}, token=tokens[0]),
            200,
            0,
            "创建实时组",
        )
        group_id = int(created["data"]["id"])
        group_ids.append(group_id)
        invite = expect(
            client.call(
                "POST", f"/api/groups/{group_id}/members", {"username": names[1]}, token=tokens[0]
            ),
            200,
            0,
            "邀请 Bob",
        )
        expect(
            client.call("POST", f"/api/invites/{invite['data']['id']}/accept", token=tokens[1]),
            200,
            0,
            "Bob 接受邀请",
        )

        bad_sock, status, _, body, _ = handshake(port, tokens[0], group_id, upgrade=False)
        bad_sock.close()
        assert status == 400 and json.loads(body)["code"] == 1001, body
        bad_sock, status, _, body, _ = handshake(port, "invalid-token", group_id)
        bad_sock.close()
        assert status == 401 and json.loads(body)["code"] == 1002, body
        bad_sock, status, _, body, _ = handshake(port, tokens[2], group_id)
        bad_sock.close()
        assert status == 403 and json.loads(body)["code"] == 1004, body

        alice_sock, status, headers, frame_bytes, key = handshake(port, tokens[0], group_id)
        assert status == 101 and headers.get("upgrade", "").lower() == "websocket"
        alice_ws = WSClient(alice_sock, headers, frame_bytes, key)
        sockets.append(alice_sock)
        alice_ready = alice_ws.expect_ready()
        bob_sock, status, headers, frame_bytes, key = handshake(port, tokens[1], group_id)
        bob_ws = WSClient(bob_sock, headers, frame_bytes, key)
        sockets.append(bob_sock)
        bob_ws.expect_ready()
        wait_metric(client, tokens[0], 2)

        push_started = time.monotonic()
        http_message = expect(
            client.call(
                "POST",
                f"/api/groups/{group_id}/messages",
                {"content": "HTTP 到 WebSocket"},
                token=tokens[0],
            ),
            200,
            0,
            "HTTP 发消息",
        )
        pushed = bob_ws.recv_json(lambda value: value.get("type") == "message")
        push_ms = (time.monotonic() - push_started) * 1000
        assert int(pushed["data"]["id"]) == int(http_message["data"]["id"])
        assert pushed["data"]["content"] == "HTTP 到 WebSocket", pushed

        client_message_id = f"11111111-2222-3333-4444-{int(time.time() * 1000) % 1000000000000:012d}"
        bob_ws.send_json(
            {"type": "send", "content": "WebSocket 到 HTTP", "client_msg_id": client_message_id}
        )
        ack = bob_ws.recv_json(lambda value: value.get("type") == "ack")
        ws_message = bob_ws.recv_json(lambda value: value.get("type") == "message")
        assert ack["client_msg_id"] == client_message_id
        assert int(ack["message_id"]) == int(ws_message["data"]["id"])
        assert ws_message["data"]["content"] == "WebSocket 到 HTTP"

        history = expect(
            client.call("GET", f"/api/groups/{group_id}/messages?limit=10", token=tokens[0]),
            200,
            0,
            "HTTP 读取 WS 消息",
        )
        assert [item["content"] for item in reversed(history["data"]["items"])] == [
            "HTTP 到 WebSocket",
            "WebSocket 到 HTTP",
        ], history

        fragmented = json.dumps({"type": "ping"}).encode("utf-8")
        bob_ws.send_frame(1, fragmented[:4], fin=False)
        bob_ws.send_frame(0, fragmented[4:], fin=True)
        assert bob_ws.recv_json(lambda value: value.get("type") == "pong")
        bob_ws.send_frame(0x9, b"client-ping")
        opcode, payload = bob_ws.recv_frame()
        assert opcode == 0xA and payload == b"client-ping", (opcode, payload)

        offline_base = int(alice_ready["server_max_id"])
        alice_sock.close()
        alice_ws = None
        wait_metric(client, tokens[0], 1)
        offline_message = expect(
            client.call(
                "POST",
                f"/api/groups/{group_id}/messages",
                {"content": "Alice 离线期间的消息"},
                token=tokens[1],
            ),
            200,
            0,
            "离线期间发消息",
        )
        bob_offline = bob_ws.recv_json(lambda value: value.get("type") == "message")
        assert int(bob_offline["data"]["id"]) == int(offline_message["data"]["id"])
        alice_sock, status, headers, frame_bytes, key = handshake(port, tokens[0], group_id)
        assert status == 101
        alice_ws = WSClient(alice_sock, headers, frame_bytes, key)
        sockets.append(alice_sock)
        reconnected = alice_ws.expect_ready()
        assert int(reconnected["server_max_id"]) >= int(offline_message["data"]["id"])
        catch_up = expect(
            client.call(
                "GET",
                f"/api/groups/{group_id}/messages?after_id={offline_base}",
                token=tokens[0],
            ),
            200,
            0,
            "重连后 HTTP 补拉",
        )
        assert int(offline_message["data"]["id"]) in [
            int(item["id"]) for item in catch_up["data"]["items"]
        ]

        expect(
            client.call(
                "DELETE", f"/api/groups/{group_id}/members/{bob}", token=tokens[0]
            ),
            200,
            0,
            "移除 Bob",
        )
        wait_metric(client, tokens[0], 1)
        expect(
            client.call(
                "POST",
                f"/api/groups/{group_id}/messages",
                {"content": "移除后不应推送"},
                token=tokens[0],
            ),
            200,
            0,
            "移除后发消息",
        )
        bob_ws.assert_silent()

        bob_ws.expect_close()
        bob_ws.sock.close()

        alice_ws.expect_close()
        alice_ws.sock.close()
        alice_ws = None
        wait_metric(client, tokens[0], 0)

        print(f"P9 WebSocket 实时推送延迟: {push_ms:.1f}ms")
        print("P9 WebSocket 验收通过：握手/实时双向/分片/PingPong/断线补拉/权限清理 OK")
        return 0
    finally:
        for sock in sockets:
            try:
                sock.close()
            except OSError:
                pass
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
