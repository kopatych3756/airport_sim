#!/usr/bin/env python3
import argparse
from pathlib import Path
import re
import signal
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]


def line_with(process, stream, text, timeout=6):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if (line := next((row for row in stream.read_text().splitlines() if text in row), None)):
            return line
        if process.poll() is not None:
            break
        time.sleep(0.02)
    raise RuntimeError(f"Expected {text!r}. See {stream}")


def run(args):
    bin_dir, output = args.bin_dir.resolve(), args.output.resolve()
    config = args.config.resolve()
    output.mkdir(parents=True, exist_ok=True)
    processes = []
    server = None
    pending_signal = None

    def launch(role, path, *options):
        stream = path.open("w") if path else None
        try:
            process = subprocess.Popen([bin_dir / f"airport_{role}", *options], cwd=ROOT, stdout=stream,
                                       stderr=subprocess.STDOUT if stream else None, start_new_session=True)
        finally:
            if stream:
                stream.close()
        processes.append(process)
        return process

    def forward(signum, _frame):
        nonlocal pending_signal
        pending_signal = signum
        if server and server.poll() is None:
            server.send_signal(signum)

    old_int = signal.signal(signal.SIGINT, forward)
    old_term = signal.signal(signal.SIGTERM, forward)
    try:
        server_output = output / "server.txt"
        server = launch("server", server_output, "--bind", args.ip, "--port", str(args.port),
                        "-c", config, "--startup-ms", "30000", "--delay-ms", str(args.delay_ms),
                        "--log", output / "audit.log", "--quiet")
        if pending_signal:
            server.send_signal(pending_signal)
        ready = line_with(server, server_output, "READY ")
        port = re.search(r"\bport=(\d+)", ready)[1]
        print(ready, flush=True)

        def ctl(command):
            return subprocess.check_output([bin_dir / "airport_ctl", args.ip, port, command], cwd=ROOT,
                                           text=True, timeout=6)

        status = ctl("status")
        roster = re.findall(r"^(AIRCRAFT|RUNWAY) (\S+)", status, re.M)
        actors = []
        for i, (role, name) in enumerate(roster):
            path = output / f"actor-{i:03d}-{role.lower()}.txt"
            actor = launch(role.lower(), path, args.ip, port, name)
            actors.append((actor, path, "ACK REQUEST" if role == "AIRCRAFT" else "ACK READY"))
        for actor, path, ack in actors:
            line_with(actor, path, ack)

        watch = launch("watch", None, args.ip, port, "demo")
        deadline = time.monotonic() + 6
        while "WATCH demo connected=1" not in ctl("status"):
            if time.monotonic() >= deadline or watch.poll() is not None:
                raise RuntimeError("The demo observer did not register.")
            time.sleep(0.02)
        ctl("start")

        result = server.wait()
        client_codes = [process.wait(timeout=6) for process in processes if process is not server]
        return result if result else next((code for code in client_codes if code), 0)
    except (OSError, subprocess.SubprocessError, RuntimeError) as error:
        if pending_signal:
            return 130
        print(error, file=sys.stderr)
        return 3
    finally:
        if server and server.poll() is None:
            server.terminate()
        for process in processes:
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        signal.signal(signal.SIGINT, old_int)
        signal.signal(signal.SIGTERM, old_term)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-c", "--config", type=Path, default=ROOT / "configs/default.conf")
    parser.add_argument("--bin-dir", type=Path, default=ROOT / "build")
    parser.add_argument("--output", type=Path, default=ROOT / "build/tcp-demo")
    parser.add_argument("--ip", default="127.0.0.1", help="local IPv4 or IPv6 address")
    parser.add_argument("--port", type=int, default=0)
    parser.add_argument("--delay-ms", type=int, default=120)
    sys.exit(run(parser.parse_args()))
