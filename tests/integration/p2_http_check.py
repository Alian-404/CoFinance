#!/usr/bin/env python3
"""P2 验收：HTTP 解析、keep-alive、pipeline、限制与静态文件。"""

import argparse
import os
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path


def pick_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def wait_ready(proc: subprocess.Popen, port: int, timeout: float = 5.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(f"服务启动失败，退出码 {proc.returncode}")
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError("等待 HTTP 服务监听超时")


class HttpReader:
    def __init__(self, sock: socket.socket):
        self.sock = sock
        self.buffer = b""

    def _receive(self) -> None:
        chunk = self.sock.recv(65536)
        if not chunk:
            raise RuntimeError("连接提前关闭")
        self.buffer += chunk

    def read_response(self):
        while b"\r\n\r\n" not in self.buffer:
            self._receive()
        head, self.buffer = self.buffer.split(b"\r\n\r\n", 1)
        lines = head.decode("iso-8859-1").split("\r\n")
        status = int(lines[0].split(" ", 2)[1])
        headers = {}
        for line in lines[1:]:
            name, value = line.split(":", 1)
            headers[name.strip().lower()] = value.strip()
        length = int(headers.get("content-length", "0"))
        while len(self.buffer) < length:
            self._receive()
        body = self.buffer[:length]
        self.buffer = self.buffer[length:]
        return status, headers, body


def connect(port: int) -> tuple:
    sock = socket.create_connection(("127.0.0.1", port), timeout=3.0)
    sock.settimeout(3.0)
    return sock, HttpReader(sock)


def request(path: str, close: bool = False) -> bytes:
    connection = "close" if close else "keep-alive"
    return (
        f"GET {path} HTTP/1.1\r\nHost: localhost\r\nConnection: {connection}\r\n\r\n"
    ).encode("ascii")


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
            "COFINANCE_SERVER_IO_THREADS": "4",
            "COFINANCE_SERVER_REQUEST_TIMEOUT_MS": "3000" if args.valgrind else "500",
            "COFINANCE_LOG_LEVEL": "WARN",
            "COFINANCE_LOG_TO_TERMINAL": "0",
            "COFINANCE_LOG_DIR": "/tmp/cofinance_p2_logs",
        }
    )
    command = [str(root / args.binary), "-c", "config/server.conf"]
    if args.valgrind:
        command = [
            "valgrind",
            "--leak-check=full",
            "--show-leak-kinds=all",
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
    sockets = []
    try:
        wait_ready(proc, port)

        # keep-alive：同一条连接连续跑 API 和静态文件。
        sock, reader = connect(port)
        sockets.append(sock)
        sock.sendall(request("/api/ping"))
        status, headers, body = reader.read_response()
        assert status == 200 and b"pong" in body, (status, body)
        assert headers["connection"] == "keep-alive"

        sock.sendall(request("/api/health"))
        status, _, body = reader.read_response()
        assert status == 200 and b'"ok"' in body, (status, body)

        sock.sendall(request("/"))
        status, headers, body = reader.read_response()
        assert status == 200 and b"CoFinance" in body, (status, body)
        assert headers["content-type"].startswith("text/html")
        print("keep-alive + /api/ping + 静态页: OK")

        # pipelining：一次写入两个请求，响应必须按顺序返回。
        sock2, reader2 = connect(port)
        sockets.append(sock2)
        sock2.sendall(request("/api/ping") + request("/api/health", close=True))
        status1, _, body1 = reader2.read_response()
        status2, headers2, body2 = reader2.read_response()
        assert status1 == 200 and b"pong" in body1
        assert status2 == 200 and b'"ok"' in body2
        assert headers2["connection"] == "close"
        print("pipelining 两请求顺序响应: OK")

        # 半包：请求行、头、空行分三次到达。
        sock3, reader3 = connect(port)
        sockets.append(sock3)
        sock3.sendall(b"GET /api/pi")
        time.sleep(0.05)
        sock3.sendall(b"ng HTTP/1.1\r\nHost: local")
        time.sleep(0.05)
        sock3.sendall(b"host\r\n\r\n")
        status, _, body = reader3.read_response()
        assert status == 200 and b"pong" in body
        print("半包请求: OK")

        # Content-Length body 被完整消费后，路由返回 405 而不是把 body 当新请求。
        sock4, reader4 = connect(port)
        sockets.append(sock4)
        sock4.sendall(
            b"POST /api/ping HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\n\r\nhello"
        )
        status, _, _ = reader4.read_response()
        assert status == 405
        print("Content-Length body 解析: OK")

        # 超限：Content-Length 直接超过 1 MiB。
        sock5, reader5 = connect(port)
        sockets.append(sock5)
        sock5.sendall(
            b"POST /api/ping HTTP/1.1\r\nHost: localhost\r\nContent-Length: 1048577\r\n\r\n"
        )
        status, headers5, _ = reader5.read_response()
        assert status == 413 and headers5["connection"] == "close"
        print("超大 body 拒绝: OK")

        # 超长头：没有 CRLF 的单行头超过 8 KiB。
        sock6, reader6 = connect(port)
        sockets.append(sock6)
        sock6.sendall(b"GET / HTTP/1.1\r\nHost: localhost\r\nX-Long: " + b"a" * 9000)
        status, _, _ = reader6.read_response()
        assert status == 431
        print("超大 header 拒绝: OK")

        # Transfer-Encoding 未实现，必须明确 501，不能静默按 Content-Length 处理。
        sock7, reader7 = connect(port)
        sockets.append(sock7)
        sock7.sendall(
            b"POST /api/ping HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n"
        )
        status, _, _ = reader7.read_response()
        assert status == 501
        print("Transfer-Encoding 拒绝: OK")

        # 慢速攻击：只发半个头，500ms 后必须 408。Valgrind 下跳过以控制耗时。
        if not args.valgrind:
            sock8, reader8 = connect(port)
            sockets.append(sock8)
            sock8.sendall(b"GET /api/ping HTTP/1.1\r\nHost: localhost\r\n")
            time.sleep(0.8)
            status, _, _ = reader8.read_response()
            assert status == 408
            print("请求超时 408: OK")

        # 路径穿越：解码后的 .. 不能读出项目配置。
        sock9, reader9 = connect(port)
        sockets.append(sock9)
        sock9.sendall(request("/%2e%2e/config/secrets.conf"))
        status, _, body9 = reader9.read_response()
        assert status == 404 and b"password" not in body9.lower()
        print("静态路径穿越拒绝: OK")

        # HTTP/1.0 默认 close。
        sock10, reader10 = connect(port)
        sockets.append(sock10)
        sock10.sendall(b"GET /api/ping HTTP/1.0\r\n\r\n")
        status, headers10, _ = reader10.read_response()
        assert status == 200 and headers10["connection"] == "close"
        print("HTTP/1.0 默认关闭: OK")

        proc.send_signal(signal.SIGINT)
        return_code = proc.wait(timeout=10.0)
        stdout, stderr = proc.communicate(timeout=2.0)
        if return_code != 0:
            raise RuntimeError(f"SIGINT 后退出码 {return_code}\n{stdout}\n{stderr}")
        print("SIGINT 优雅退出: 退出码 0")
        if args.valgrind:
            for line in stderr.splitlines():
                if any(marker in line for marker in
                       ("definitely lost", "indirectly lost", "ERROR SUMMARY", "All heap blocks")):
                    print(f"valgrind: {line.strip()}")
        return 0
    except Exception as exc:
        if proc.poll() is None:
            proc.kill()
        stdout, stderr = proc.communicate(timeout=2.0)
        print(f"验收失败: {exc}", file=sys.stderr)
        if stdout:
            print(stdout, file=sys.stderr)
        if stderr:
            print(stderr, file=sys.stderr)
        return 1
    finally:
        for sock in sockets:
            sock.close()


if __name__ == "__main__":
    raise SystemExit(main())
