#!/usr/bin/env python3
import argparse
import os
from pathlib import Path
import re
import signal
import socket
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--bin-dir", type=Path, default=ROOT / "build")
parser.add_argument("--evidence-dir", type=Path, help="save selected demonstration logs")
args = parser.parse_args()
BIN = args.bin_dir.resolve()
BASE_ENV = dict(os.environ)
BASE_ENV.pop("AIRPORT_CONTROL_TOKEN", None)
TMP = ROOT / "build" / "tcp-test-tmp"
TMP.mkdir(parents=True, exist_ok=True)
EVENT = re.compile(r"^EVENT (\d+) (\[T=(\d+)\] \[([A-Z-]+) *\] (.*))$", re.M)
passed = 0


def wait_for(probe, label, timeout=8):
    until = time.monotonic() + timeout
    while time.monotonic() < until:
        result = probe()
        if result:
            return result
        time.sleep(0.01)
    raise AssertionError("Timeout: " + label)


class Process:
    def __init__(self, path, executable, *arguments, env=BASE_ENV):
        self.path = path
        with path.open("wb") as stream:
            self.proc = subprocess.Popen([BIN / executable, *arguments], cwd=ROOT, env=env,
                                         stdout=stream, stderr=subprocess.STDOUT)

    def read(self):
        return self.path.read_text(encoding="utf-8", errors="replace")

    def contains(self, expected, timeout=8):
        def found():
            text = self.read()
            if expected in text:
                return text
            if self.proc.poll() is not None:
                raise AssertionError((expected, self.proc.returncode, text[-3000:]))
            return None

        return wait_for(found, expected, timeout)

    def check(self, code=0):
        actual = self.proc.wait(timeout=16)
        text = self.read()
        assert actual == code, (actual, code, text[-3000:])
        return text


class Session:
    def __init__(self, config, *options, host="127.0.0.1", token=None, log=False):
        self.temp = tempfile.TemporaryDirectory(dir=TMP)
        self.directory = Path(self.temp.name)
        self.processes = []
        self.host = host
        self.env = dict(BASE_ENV)
        if token:
            self.env["AIRPORT_CONTROL_TOKEN"] = token
        log_options = ["--log", self.directory / "audit.log"] if log else ["--no-log"]
        self.server = self.launch("server", "--bind", host, "--port", "0", "-c", config,
                                  "--startup-ms", "20000", *log_options, *options)
        try:
            ready = self.server.contains("READY ")
            self.port = re.search(r"^READY .*? port=(\d+)", ready, re.M)[1]
        except BaseException:
            self.__exit__()
            raise

    def __enter__(self):
        return self

    def __exit__(self, *_):
        for process in self.processes:
            if process.proc.poll() is None:
                process.proc.kill()
        for process in self.processes:
            process.proc.wait(timeout=5)
        self.temp.cleanup()

    def launch(self, role, *options):
        proc = Process(self.directory / f"{len(self.processes):03d}-{role}.txt",
                       "airport_" + role, *options, env=self.env)
        self.processes.append(proc)
        return proc

    def client(self, role, name):
        return self.launch(role, self.host, self.port, name)

    def watch(self, name):
        watch = self.client("watch", name)
        watch.contains("SNAPSHOT END")
        return watch

    def ctl(self, *command, code=0, env=None):
        result = subprocess.run([BIN / "airport_ctl", self.host, self.port, *command],
                                env=self.env if env is None else env, capture_output=True, text=True, timeout=6)
        assert result.returncode == code, (command, result.returncode, result.stdout, result.stderr)
        if not code:
            assert "RESULT OK\nBYE CONTROL_COMPLETE\n" in result.stdout, result.stdout
        return result.stdout

    def record_evidence(self, name, **texts):
        if not args.evidence_dir:
            return
        directory = args.evidence_dir / name
        directory.mkdir(parents=True, exist_ok=True)
        for basename, text in texts.items():
            (directory / f"{basename}.txt").write_text(text)

    def roster(self):
        status = self.ctl("status")
        return [(role.lower(), name) for role, name in re.findall(r"^(AIRCRAFT|RUNWAY) (\S+)", status, re.M)]

    def activate_all(self):
        registrations = [(self.client(role, name), "ACK REQUEST" if role == "aircraft" else "ACK READY")
                         for role, name in self.roster()]
        for process, ack in registrations:
            process.contains(ack)
        return [process for process, _ in registrations]

    def check_end(self, reason, clients, code=0):
        text = self.server.check(code)
        assert f"SERVER_END reason={reason}" in text
        for client in clients:
            output = client.check()
            assert output.endswith(f"BYE {reason}\n"), output[-1500:]


def event_rows(text):
    return [(int(seq), int(tick), kind, message) for seq, _, tick, kind, message in EVENT.findall(text)]


def verify_invariants(text, config):
    active, next_free, required = {}, {}, {}
    started = set()
    runway_lengths = {}
    closures = {}
    for row in Path(config).read_text().splitlines():
        fields = row.split()
        if fields and fields[0] == "RUNWAY":
            runway_lengths[fields[1]] = int(fields[2])
        elif fields and fields[0] == "CLOSE":
            closures.setdefault(fields[1], []).append(tuple(map(int, fields[2:])))
    assigned = {}
    exhausted, diverted = set(), set()
    for _, tick, kind, message in event_rows(text):
        words = message.split()
        if kind == "REQUEST":
            required[words[0]] = int(re.search(r"required=(\d+)", message)[1])
            if "requests LANDING" in message and "fuel=0 " in message:
                exhausted.add(words[0])
        elif kind == "ASSIGN":
            plane, runway = words[2], words[4]
            assert required[plane] <= runway_lengths[runway] and runway not in active, message
            assigned[plane] = runway
        elif kind == "START":
            plane, runway = words[0], words[4].rstrip(";")
            assert assigned.pop(plane) == runway
            assert plane not in started and runway not in active
            assert tick >= next_free.get(runway, 0)
            assert all(not start <= tick < end for start, end in closures.get(runway, ()))
            started.add(plane)
            active[runway] = plane
        elif kind == "FINISH":
            assert active.pop(words[4]) == words[0], message
        elif kind == "RELEASE":
            next_free[words[0]] = int(message.split("T=")[1])
        elif kind == "FUEL":
            fuel = int(words[-1])
            assert fuel >= 0
            if fuel == 0:
                exhausted.add(words[0])
        elif kind == "DIVERT":
            diverted.add(words[0])
    assert exhausted == diverted


def verify_observer(text):
    assert "SNAPSHOT END" in text
    start = int(re.search(r"^SNAPSHOT BEGIN seq=(\d+)", text, re.M)[1])
    assert int(re.search(r"^SNAPSHOT END seq=(\d+)", text, re.M)[1]) == start
    seqs = [seq for seq, *_ in event_rows(text)]
    assert seqs == list(range(start + 1, start + 1 + len(seqs)))


def scenario(name, expected, reason="ALL_REQUESTS_COMPLETED"):
    config = ROOT / "configs" / name
    with Session(config, log=True) as session:
        actors = session.activate_all()
        watch = session.watch("tower")
        session.ctl("start")
        session.check_end(reason, [watch, *actors])
        text = watch.read()
        for fragment in expected:
            assert fragment in text, fragment
        if name == "test_emergency.conf":
            assert text.index("ATC assigned A001") < text.index("ATC assigned D002")
        if name == "test_nominal.conf":
            a001 = next(p for p in actors if "WELCOME AIRCRAFT A001\n" in p.read())
            assert "A001 started LANDING" in a001.read() and "D001 started" not in a001.read()
            runway = next(p for p in actors if "WELCOME RUNWAY R-LONG\n" in p.read())
            assert "D001 started TAKEOFF on R-LONG" in runway.read()
            assert "A001 started LANDING on R-SHORT" not in runway.read()
            assert "[ASSIGN" in session.server.read() and "[START" not in session.server.read()
        verify_observer(text)
        verify_invariants(text, config)
        audit = (session.directory / "audit.log").read_text()
        assert f"termination={reason}" in audit
        assert all(line in audit for _, line, *_ in EVENT.findall(text))
        if name == "test_nominal.conf":
            session.record_evidence("nominal", server=session.server.read(), observer=text,
                                    aircraft=a001.read(), runway=runway.read(), audit=audit)
        return [line for _, line, _, kind, _ in EVENT.findall(text) if kind != "SHIFT"]


def reconnect():
    config = ROOT / "configs/test_tcp_reconnect.conf"
    with Session(config, log=True) as session:
        actors = session.activate_all()
        aircraft = next(p for p in actors if "WELCOME AIRCRAFT A1\n" in p.read())
        runway = next(p for p in actors if "WELCOME RUNWAY R1\n" in p.read())
        main, removed, voluntary = [session.watch(name) for name in ["tower", "apron", "training"]]
        session.client("watch", "tower").check(2)
        session.ctl("start")
        main.contains("D1 started TAKEOFF")
        voluntary.proc.send_signal(signal.SIGINT)
        voluntary.check(130)
        control_log = []
        for role, name in [("RUNWAY", "R1"), ("AIRCRAFT", "A1"), ("WATCH", "apron")]:
            control_log.append(f"$ off {role} {name}\n" + session.ctl("off", role, name))
        assert "BYE ADMIN_OFFLINE" in runway.check()
        assert "BYE ADMIN_OFFLINE" in aircraft.check()
        assert "BYE ADMIN_OFFLINE" in removed.check()
        for role, name in [("runway", "R1"), ("aircraft", "A1"), ("watch", "apron")]:
            assert "ERROR ADMIN_OFFLINE" in session.client(role, name).check(2)

        main.contains("A1 left for alternate airport")
        main.contains("D1 completed TAKEOFF")
        status = session.ctl("status")
        assert re.search(r"^RUNWAY R1 .*active=- .*completed=1 .*ready=0 enabled=0$", status, re.M)
        assert re.search(r"^AIRCRAFT D2 .*state=WAITING", status, re.M)
        assert re.search(r"^AIRCRAFT A1 .*state=DIVERTED .*connected=0 requested=1 enabled=0$", status, re.M)
        assert "WATCH apron connected=0 enabled=0" in status
        assert "D2 started" not in main.read()

        control_log.append("$ status\n" + status)
        control_log.append("$ on WATCH apron\n" + session.ctl("on", "WATCH", "apron"))
        replacement = session.watch("apron")
        resumed = session.watch("training")
        assert re.search(r"^AIRCRAFT D2 .*state=WAITING", replacement.read(), re.M)
        control_log.append("$ on AIRCRAFT A1\n" + session.ctl("on", "AIRCRAFT", "A1"))
        new_plane = session.client("aircraft", "A1")
        new_plane.contains("ACK REQUEST ALREADY_ACCEPTED")
        control_log.append("$ on RUNWAY R1\n" + session.ctl("on", "RUNWAY", "R1"))
        new_runway = session.client("runway", "R1")
        new_runway.contains("ACK READY")
        still_connected = [p for p in actors if p not in (runway, aircraft)]
        session.check_end("ALL_REQUESTS_COMPLETED",
                          [main, replacement, resumed, new_plane, new_runway, *still_connected])
        for watcher in (main, removed, voluntary, replacement, resumed):
            verify_observer(watcher.read())
        main_events = dict((seq, (tick, kind, message)) for seq, tick, kind, message in event_rows(main.read()))
        for watcher in (replacement, resumed):
            for seq, tick, kind, message in event_rows(watcher.read()):
                assert main_events[seq] == (tick, kind, message)
        assert main.read().count("A1 requests LANDING") == 1
        assert "took_off=2, diverted=1" in main.read()
        verify_invariants(main.read(), config)
        session.record_evidence("reconnect", server=session.server.read(), observer=main.read(),
                                observer_before=removed.read(), observer_rejoined=replacement.read(),
                                runway_before=runway.read(), runway_rejoined=new_runway.read(),
                                aircraft_rejoined=new_plane.read(), control="\n".join(control_log))


def attached_shutdown(stop_signal):
    with Session(ROOT / "configs/test_tcp_reconnect.conf", "--max-ticks", "0") as session:
        actors = session.activate_all()
        watchers = [session.watch(f"station{i}") for i in range(3)]
        session.ctl("start")
        watchers[0].contains("D1 started TAKEOFF")
        session.server.proc.send_signal(stop_signal)
        session.check_end("USER_INTERRUPT", [*actors, *watchers], code=130)
        assert "operating=1" in watchers[1].read()
        assert "unfinished=D1 state=OPERATING" in watchers[2].read()
        if stop_signal == signal.SIGTERM:
            session.record_evidence("shutdown", server=session.server.read(), aircraft=actors[0].read(),
                                    observer=watchers[0].read())


def registration_and_framing():
    config = ROOT / "configs/test_tcp_reconnect.conf"
    with Session(config, "--max-ticks", "1", token="lab-access-token", host="::1") as session:
        assert "CONTROL_DENIED" in session.ctl("status", code=2, env=BASE_ENV)
        assert "CONTROL_DENIED" in session.ctl("status", code=2,
                                               env=dict(BASE_ENV, AIRPORT_CONTROL_TOKEN="different"))
        session.ctl("off", "AIRCRAFT", "D1")
        rejected = session.client("aircraft", "D1").check(2)
        assert "ADMIN_OFFLINE" in rejected
        assert "UNKNOWN_ACTOR" in session.client("aircraft", "nonexistent").check(2)
        session.ctl("on", "AIRCRAFT", "D1")
        with socket.create_connection((session.host, int(session.port)), timeout=4) as peer:
            peer.sendall(b"HELLO 1 CONT")
            time.sleep(0.015)
            peer.sendall(b"ROL lab-access-token\nSTATUS\n")
            with peer.makefile("r", encoding="ascii") as stream:
                response = stream.read()
            assert "WELCOME CONTROL" in response and "SNAPSHOT END" in response and "RESULT OK" in response
        observer = session.watch("ipv6")
        runway = session.client("runway", "R1")
        runway.contains("ACK READY")
        session.ctl("start")
        session.check_end("SHIFT_ENDED", [observer, runway])
        assert "pending=3" in observer.read() and "[START" not in observer.read()


def early_shutdown():
    with Session(ROOT / "configs/test_empty.conf") as session:
        watcher = session.watch("before-shift")
        session.server.proc.send_signal(signal.SIGINT)
        session.check_end("USER_INTERRUPT", [watcher], code=130)
        assert "phase=REGISTERING" in watcher.read()
        assert "[SHIFT " not in watcher.read()


def automatic_start_and_invalid_config():
    with Session(ROOT / "configs/test_empty.conf", "--startup-ms", "300") as session:
        observer = session.watch("automatic")
        session.check_end("ALL_REQUESTS_COMPLETED", [observer])
        assert "phase=REGISTERING" in observer.read() and "[SHIFT " in observer.read()
    invalid = subprocess.run([BIN / "airport_server", "--bind", "127.0.0.1", "--port", "0",
                              "--no-log", "-c", ROOT / "configs/test_duplicate.conf"],
                             cwd=ROOT, capture_output=True, text=True, timeout=5)
    assert invalid.returncode == 2 and "Duplicate aircraft number" in invalid.stderr
    assert "READY" not in invalid.stdout


def backpressured_observer():
    with tempfile.TemporaryDirectory(dir=TMP) as directory:
        config = Path(directory) / "stress.conf"
        config.write_text("SEED 31\nMAX_TICKS 170\nDELAY_MS 3\n"
                          "RUNWAY SHORT 1000 2\nTYPE HEAVY 3000 4 4\n"
                          "ARRIVAL_FUEL 180 180\nARRIVAL_BURN 1 1\nFLOW ARRIVAL 300 0 0 0\n")
        with Session(config, "--quiet") as session:
            actors = session.activate_all()
            fast = session.watch("following")
            with socket.socket() as stalled:
                stalled.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
                stalled.settimeout(10)
                stalled.connect((session.host, int(session.port)))
                stalled.sendall(b"HELLO 1 WATCH stalled\n")
                wait_for(lambda: "WATCH stalled connected=1" in session.ctl("status"), "stalled watch registered")
                session.ctl("start")
                fast.contains("[T=0120]", timeout=12)
                assert "WATCH stalled connected=1" not in session.ctl("status")
                session.check_end("SHIFT_ENDED", [fast, *actors])
                verify_observer(fast.read())
                assert "waiting=300" in fast.read()


def seed_replay():
    first = scenario("default.conf", ["landed=", "diverted="])
    second = scenario("default.conf", ["landed=", "diverted="])
    assert first == second
    legacy = subprocess.run([BIN / "airport_named", "--no-log"], cwd=ROOT, env=BASE_ENV,
                            text=True, capture_output=True, check=True, timeout=12).stdout
    assert first == [line for line in legacy.splitlines() if line.startswith("[T=")]


def record(name, test):
    global passed
    test()
    passed += 1
    print(f"PASS {name}", flush=True)


record("ordinary traffic, local views and audit log",
       lambda: scenario("test_nominal.conf", ["landed=2, took_off=2, diverted=0"]))
record("emergency aircraft before high-priority departure",
       lambda: scenario("test_emergency.conf", ["[EMERGENCY", "(EMERGENCY)"]))
record("incompatible runway, diversion and shift limit",
       lambda: scenario("test_incompatible.conf", ["diverted=1", "waiting=1"], "SHIFT_ENDED"))
record("closed runway preserves an active operation",
       lambda: scenario("test_closure.conf", ["[CLOSE", "[OPEN", "D001 completed TAKEOFF"]))
record("safety interval", lambda: scenario("test_safety.conf", ["took_off=3"]))
record("empty shift", lambda: scenario("test_empty.conf", ["aircraft_total=0"]))
record("arrival with zero reserve", lambda: scenario("test_zero_fuel.conf", ["diverted=1", "[DIVERT"]))
record("dynamic observers and administrative OFF/ON for all three roles", reconnect)
record("server SIGINT closes every attached client", lambda: attached_shutdown(signal.SIGINT))
record("server SIGTERM closes every attached client", lambda: attached_shutdown(signal.SIGTERM))
record("SIGINT during registration", early_shutdown)
record("IPv6, control token, registration and TCP frame boundaries", registration_and_framing)
record("automatic start and rejection of a duplicate aircraft number", automatic_start_and_invalid_config)
record("slow observer isolated while 300 aircraft progress", backpressured_observer)
record("seed replay and equivalence with the HW3 model", seed_replay)
print(f"{passed} TCP checks passed")
