#!/usr/bin/env python3
"""P1 验收：1000 条并发连接、逐条回显、空闲 CPU 与优雅退出。"""

import argparse
import os
import resource
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path


def raise_nofile() -> None:
    _, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    resource.setrlimit(resource.RLIMIT_NOFILE, (min(65535, hard), hard))


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
    raise RuntimeError("等待服务监听超时")


def recv_exact(sock: socket.socket, size: int) -> bytes:
    chunks = []
    remaining = size
    while remaining:
        chunk = sock.recv(remaining)
        if not chunk:
            raise RuntimeError(f"回显连接提前关闭，还差 {remaining} 字节")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def cpu_seconds(pid: int) -> float:
    fields = Path(f"/proc/{pid}/stat").read_text().split()
    ticks = int(fields[13]) + int(fields[14])
    return ticks / os.sysconf("SC_CLK_TCK")


def print_valgrind_summary(stderr: str) -> None:
    for line in stderr.splitlines():
        if any(
            marker in line
            for marker in ("definitely lost", "indirectly lost", "ERROR SUMMARY", "All heap blocks")
        ):
            print(f"valgrind: {line.strip()}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--connections", type=int, default=1000)
    parser.add_argument("--binary", default="build/cofinance")
    parser.add_argument("--valgrind", action="store_true")
    parser.add_argument("--skip-cpu", action="store_true")
    args = parser.parse_args()
    if args.connections < 1:
        parser.error("--connections 必须大于 0")

    root = Path(__file__).resolve().parents[2]
    port = pick_port()
    env = os.environ.copy()
    env.update(
        {
            "COFINANCE_SERVER_PORT": str(port),
            "COFINANCE_SERVER_IO_THREADS": "4",
            "COFINANCE_SERVER_MAX_CONNECTIONS": str(max(5000, args.connections + 100)),
            "COFINANCE_SERVER_KEEPALIVE_TIMEOUT_MS": "60000",
            "COFINANCE_LOG_LEVEL": "WARN",
            "COFINANCE_LOG_TO_TERMINAL": "0",
            "COFINANCE_LOG_DIR": "/tmp/cofinance_p1_logs",
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
        preexec_fn=raise_nofile,
    )
    connections = []
    try:
        wait_ready(proc, port)
        for _ in range(args.connections):
            conn = socket.create_connection(("127.0.0.1", port), timeout=5.0)
            conn.settimeout(10.0)
            connections.append(conn)
        print(f"已建立 {len(connections)} 条并发连接")

        if not args.skip_cpu:
            time.sleep(0.5)
            cpu_before = cpu_seconds(proc.pid)
            time.sleep(2.0)
            idle_cpu = cpu_seconds(proc.pid) - cpu_before
            print(f"空闲 2 秒 CPU 时间: {idle_cpu:.3f}s")
            if idle_cpu > 1.0:
                raise RuntimeError("空闲 CPU 过高，可能存在 busy-loop")

        payloads = []
        for i, conn in enumerate(connections):
            payload = f"client-{i:04d}|".encode("ascii") + bytes([i % 251]) * 997
            payloads.append(payload)
            conn.sendall(payload)
        for i, conn in enumerate(connections):
            echoed = recv_exact(conn, len(payloads[i]))
            if echoed != payloads[i]:
                raise RuntimeError(f"第 {i} 条连接回显内容不一致")
        print(f"回显校验通过: {args.connections} 条连接全部一致")

        large_payload = b"L" * (512 * 1024)
        connections[0].sendall(large_payload)
        if recv_exact(connections[0], len(large_payload)) != large_payload:
            raise RuntimeError("512 KiB 大数据回显不一致")
        print("512 KiB 大数据回显通过（覆盖 EPOLLOUT 半写路径）")

        proc.send_signal(signal.SIGINT)
        return_code = proc.wait(timeout=10.0)
        stdout, stderr = proc.communicate(timeout=2.0)
        if return_code != 0:
            raise RuntimeError(f"SIGINT 后退出码 {return_code}\n{stdout}\n{stderr}")
        print("SIGINT 优雅退出: 退出码 0")
        if args.valgrind:
            print_valgrind_summary(stderr)
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
        for conn in connections:
            conn.close()


if __name__ == "__main__":
    raise SystemExit(main())
