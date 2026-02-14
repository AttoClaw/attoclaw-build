#!/usr/bin/env python3
from __future__ import annotations

import json
import os
import socket
import statistics
import subprocess
import sys
import threading
import time
from dataclasses import dataclass
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

import psutil


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return int(s.getsockname()[1])


class MockOpenAIHandler(BaseHTTPRequestHandler):
    server_version = "MockOpenAI/0.1"

    def do_POST(self) -> None:  # noqa: N802
        if self.path != "/v1/chat/completions":
            self.send_response(404)
            self.end_headers()
            self.wfile.write(b'{"error":"not found"}')
            return

        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length) if length > 0 else b"{}"
        try:
            payload = json.loads(body.decode("utf-8", errors="replace"))
        except Exception:
            payload = {}

        model = str(payload.get("model", "mock-model"))
        response = {
            "id": "chatcmpl_mock",
            "object": "chat.completion",
            "created": int(time.time()),
            "model": model,
            "choices": [
                {
                    "index": 0,
                    "message": {"role": "assistant", "content": "benchmark-ok"},
                    "finish_reason": "stop",
                }
            ],
            "usage": {"prompt_tokens": 12, "completion_tokens": 2, "total_tokens": 14},
        }
        out = json.dumps(response).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(out)))
        self.end_headers()
        self.wfile.write(out)

    def log_message(self, fmt: str, *args: object) -> None:
        return


@dataclass
class CmdResult:
    name: str
    ok_runs: int
    total_runs: int
    avg_ms: Optional[float]
    p95_ms: Optional[float]
    avg_peak_mb: Optional[float]
    median_peak_mb: Optional[float]
    last_exit: Optional[int]
    last_output: str


def run_once(cmd: Sequence[str], env: Dict[str, str], timeout_s: int) -> Tuple[int, float, float, str]:
    start = time.perf_counter()
    proc = subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
        env=env,
    )
    peak_rss = 0
    p = psutil.Process(proc.pid)
    try:
        while proc.poll() is None:
            try:
                rss = p.memory_info().rss
                for c in p.children(recursive=True):
                    try:
                        rss += c.memory_info().rss
                    except Exception:
                        pass
                peak_rss = max(peak_rss, rss)
            except Exception:
                pass
            if time.perf_counter() - start > timeout_s:
                proc.kill()
                break
            time.sleep(0.01)
    finally:
        try:
            out, _ = proc.communicate(timeout=2)
        except Exception:
            out = ""
    elapsed_ms = (time.perf_counter() - start) * 1000.0
    if peak_rss == 0:
        try:
            peak_rss = p.memory_info().rss
        except Exception:
            peak_rss = 0
    return proc.returncode if proc.returncode is not None else -9, elapsed_ms, peak_rss / (1024 * 1024), out.strip()


def summarize(name: str, rows: List[Tuple[int, float, float, str]]) -> CmdResult:
    oks = [r for r in rows if r[0] == 0]
    times = [r[1] for r in oks]
    peaks = [r[2] for r in oks]
    p95 = None
    if times:
        sorted_t = sorted(times)
        idx = max(0, int(round(0.95 * (len(sorted_t) - 1))))
        p95 = sorted_t[idx]
    return CmdResult(
        name=name,
        ok_runs=len(oks),
        total_runs=len(rows),
        avg_ms=(statistics.mean(times) if times else None),
        p95_ms=p95,
        avg_peak_mb=(statistics.mean(peaks) if peaks else None),
        median_peak_mb=(statistics.median(peaks) if peaks else None),
        last_exit=(rows[-1][0] if rows else None),
        last_output=(rows[-1][3][:260].replace("\n", " ") if rows else ""),
    )


def fmt(v: Optional[float]) -> str:
    return "-" if v is None else f"{v:.1f}"


def write_json(path: Path, obj: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(obj, indent=2), encoding="utf-8")


def backup(path: Path) -> Optional[str]:
    if path.exists():
        return path.read_text(encoding="utf-8", errors="replace")
    return None


def restore(path: Path, content: Optional[str]) -> None:
    if content is None:
        if path.exists():
            path.unlink()
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


def openclaw_entry() -> Optional[List[str]]:
    direct = Path(r"C:\nvm4w\nodejs\node_modules\openclaw\openclaw.mjs")
    if direct.exists():
        return ["node", str(direct)]
    try:
        out = subprocess.check_output(["npm", "root", "-g"], text=True, encoding="utf-8", errors="replace")
    except Exception:
        return None
    mjs = Path(out.strip()) / "openclaw" / "openclaw.mjs"
    if not mjs.exists():
        return None
    return ["node", str(mjs)]


def safe_text(s: str) -> str:
    return s.encode("ascii", "replace").decode("ascii")


def main() -> int:
    quick = "--quick" in sys.argv
    runs = 1 if quick else 4
    warmup = 0 if quick else 1
    timeout_s = 60 if quick else 90

    root = Path(__file__).resolve().parents[1]
    attoclaw_exe = root / "build" / "Release" / "attoclaw.exe"
    if not attoclaw_exe.exists():
        print(f"Missing {attoclaw_exe}. Build AttoClaw first.")
        return 1

    port = free_port()
    server = ThreadingHTTPServer(("127.0.0.1", port), MockOpenAIHandler)
    t = threading.Thread(target=server.serve_forever, daemon=True)
    t.start()
    base = f"http://127.0.0.1:{port}/v1"

    home = Path.home()
    attoclaw_cfg = home / ".attoclaw" / "config.json"
    nanobot_cfg = home / ".nanobot" / "config.json"

    attoclaw_backup = backup(attoclaw_cfg)
    nanobot_backup = backup(nanobot_cfg)

    try:
        write_json(
            attoclaw_cfg,
            {
                "providers": {"openai": {"apiKey": "mock-key", "apiBase": base}},
                "agents": {"defaults": {"model": "openai/gpt-4o-mini", "maxTokens": 128}},
            },
        )
        write_json(
            nanobot_cfg,
            {
                "providers": {"openai": {"apiKey": "mock-key", "apiBase": base}},
                "agents": {"defaults": {"model": "openai/gpt-4o-mini", "maxTokens": 128}},
            },
        )

        env = os.environ.copy()
        env["PYTHONUTF8"] = "1"
        env["NANOBOT_NO_COLOR"] = "1"
        env["OPENCLAW_NO_COLOR"] = "1"

        openclaw_cmd = openclaw_entry()

        commands: List[Tuple[str, List[str]]] = [
            ("attoclaw_version", [str(attoclaw_exe), "--version"]),
            ("nanobot_version", ["nanobot", "--version"]),
            ("attoclaw_agent", [str(attoclaw_exe), "agent", "-m", "benchmark ping"]),
            ("nanobot_agent", ["nanobot", "agent", "-m", "benchmark ping", "--no-markdown"]),
        ]
        if openclaw_cmd is not None:
            commands.append(("openclaw_version", [*openclaw_cmd, "--version"]))
            commands.append(
                (
                    "openclaw_agent_local",
                    [
                        *openclaw_cmd,
                        "agent",
                        "--local",
                        "--session-id",
                        "bench",
                        "--thinking",
                        "off",
                        "--message",
                        "benchmark ping",
                        "--json",
                    ],
                )
            )

        all_results: List[CmdResult] = []
        for name, cmd in commands:
            for _ in range(warmup):
                run_once(cmd, env, timeout_s)
            rows = [run_once(cmd, env, timeout_s) for _ in range(runs)]
            all_results.append(summarize(name, rows))

        mode = "quick" if quick else "full"
        print(f"\nBenchmark Summary ({mode}; lower is better for time/memory)")
        print("name,ok_runs,total_runs,avg_ms,p95_ms,avg_peak_mb,median_peak_mb,last_exit")
        for r in all_results:
            print(
                f"{r.name},{r.ok_runs},{r.total_runs},{fmt(r.avg_ms)},{fmt(r.p95_ms)},"
                f"{fmt(r.avg_peak_mb)},{fmt(r.median_peak_mb)},{r.last_exit}"
            )

        print("\nLast output sample per command:")
        for r in all_results:
            print(f"- {r.name}: {safe_text(r.last_output)}")
        return 0
    finally:
        server.shutdown()
        restore(attoclaw_cfg, attoclaw_backup)
        restore(nanobot_cfg, nanobot_backup)


if __name__ == "__main__":
    sys.exit(main())
