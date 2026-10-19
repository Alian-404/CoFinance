#!/usr/bin/env python3
"""P4 验收：注册、登录、Bearer 鉴权、改密、登出与注销。"""

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
import urllib.parse
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


def cleanup_users(root: Path, user_ids: list[int]) -> None:
    mysql = shutil.which("mysql")
    if not mysql or not user_ids:
        return
    server = load_kv(root / "config/server.conf")
    secrets = load_kv(root / "config/secrets.conf")
    fd, defaults_path = tempfile.mkstemp(prefix="cofinance_p4_mysql_")
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as out:
            out.write("[client]\n")
            out.write(f"host={server.get('db.host', '127.0.0.1')}\n")
            out.write(f"port={server.get('db.port', '3306')}\n")
            out.write(f"user={secrets.get('db.user', '')}\n")
            out.write(f"password={secrets.get('db.password', '')}\n")
            out.write(f"database={server.get('db.name', 'cofinance')}\n")
        os.chmod(defaults_path, 0o600)
        ids = ",".join(str(int(value)) for value in user_ids)
        sql = f"DELETE FROM sessions WHERE user_id IN ({ids}); DELETE FROM users WHERE id IN ({ids});"
        subprocess.run(
            [mysql, f"--defaults-extra-file={defaults_path}", "-e", sql],
            check=False,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
    finally:
        try:
            os.unlink(defaults_path)
        except FileNotFoundError:
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
            "COFINANCE_LOG_DIR": "/tmp/cofinance_p4_logs",
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
    user_ids = []
    try:
        wait_ready(proc, port, timeout=60 if args.valgrind else 8)
        suffix = f"{os.getpid() % 100000}{int(time.time() * 1000) % 1000000}"
        username = f"p4a{suffix}"
        second_name = f"p4b{suffix}"
        password = "Password123"
        new_password = "NewPassword456"

        status, body = client.call(
            "POST", "/api/auth/register", {"username": username, "password": password,
                                           "nickname": "Alice"}
        )
        assert status == 200 and body["code"] == 0, (status, body)
        user_id = int(body["data"]["id"])
        user_ids.append(user_id)

        status, body = client.call(
            "POST", "/api/auth/register", {"username": username, "password": password}
        )
        assert status == 409 and body["code"] == 1006, (status, body)

        status, body = client.call(
            "POST", "/api/auth/login", {"username": username, "password": "WrongPassword"}
        )
        assert status == 401 and body["code"] == 1007, (status, body)

        status, body = client.call(
            "POST", "/api/auth/login", {"username": username, "password": password}
        )
        assert status == 200 and body["code"] == 0, (status, body)
        token = body["data"]["token"]
        assert len(token) == 64 and body["data"]["expires_at"].endswith("Z"), body

        status, body = client.call("GET", "/api/users/me")
        assert status == 401 and body["code"] == 1002, (status, body)
        status, body = client.call("GET", "/api/users/me", token="f" * 64)
        assert status == 401 and body["code"] == 1002, (status, body)
        status, body = client.call("GET", "/api/users/me", token=token)
        assert status == 200 and body["data"]["username"] == username, (status, body)

        status, body = client.call(
            "PUT", "/api/users/me", {"nickname": "Alice New", "avatar_url": "/avatars/1.png"},
            token=token,
        )
        assert status == 200 and body["data"]["nickname"] == "Alice New", (status, body)

        status, body = client.call(
            "POST", "/api/auth/register", {"username": second_name, "password": password,
                                           "nickname": "Bob"}
        )
        assert status == 200 and body["code"] == 0, (status, body)
        second_id = int(body["data"]["id"])
        user_ids.append(second_id)
        status, body = client.call(
            "POST", "/api/auth/login", {"username": second_name, "password": password}
        )
        second_token = body["data"]["token"]
        query = urllib.parse.quote(username, safe="")
        status, body = client.call("GET", f"/api/users/search?q={query}", token=second_token)
        assert status == 200 and any(item["id"] == user_id for item in body["data"]["items"]), body

        status, body = client.call(
            "POST", "/api/auth/login", {"username": username, "password": password}
        )
        other_token = body["data"]["token"]
        status, body = client.call(
            "PUT", "/api/users/me/password",
            {"old_password": "WrongPassword", "new_password": new_password}, token=token,
        )
        assert status == 401 and body["code"] == 1007, (status, body)
        status, body = client.call(
            "PUT", "/api/users/me/password",
            {"old_password": password, "new_password": new_password}, token=token,
        )
        assert status == 200 and body["code"] == 0, (status, body)
        status, body = client.call("GET", "/api/users/me", token=token)
        assert status == 200, (status, body)
        status, body = client.call("GET", "/api/users/me", token=other_token)
        assert status == 401 and body["code"] == 1002, (status, body)

        status, body = client.call("POST", "/api/auth/logout", token=token)
        assert status == 200 and body["code"] == 0, (status, body)
        status, body = client.call("GET", "/api/users/me", token=token)
        assert status == 401 and body["code"] == 1002, (status, body)

        status, body = client.call(
            "POST", "/api/auth/login", {"username": username, "password": new_password}
        )
        token = body["data"]["token"]
        status, body = client.call("POST", "/api/auth/logout_all", token=token)
        assert status == 200 and body["code"] == 0, (status, body)
        status, body = client.call("GET", "/api/users/me", token=token)
        assert status == 401 and body["code"] == 1002, (status, body)

        status, body = client.call(
            "POST", "/api/auth/login", {"username": username, "password": new_password}
        )
        token = body["data"]["token"]
        status, body = client.call("DELETE", "/api/users/me", token=token)
        assert status == 200 and body["code"] == 0, (status, body)
        status, body = client.call(
            "POST", "/api/auth/login", {"username": username, "password": new_password}
        )
        assert status == 401 and body["code"] == 1007, (status, body)

        status, body = client.call("DELETE", "/api/users/me", token=second_token)
        assert status == 200 and body["code"] == 0, (status, body)
        print("P4 auth integration: 注册/登录/鉴权/改密/登出/注销 OK")
        return 0
    finally:
        cleanup_users(root, user_ids)
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
