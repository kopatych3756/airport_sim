#!/usr/bin/env python3
import os, re, signal, subprocess, time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
os.chdir(ROOT)
BIN = str(ROOT / 'build/airport')
LOG = ROOT / 'report/test_results.txt'
results = []


def inventory():
    return {str(p) for directory in ['/dev/shm', '/tmp'] for p in Path(directory).glob('*airport_*')}


def check_events(text, config):
    active = {};
    free = {};
    starts = set();
    lengths = {};
    required = {}
    for line in (ROOT / 'configs' / config).read_text().splitlines():
        fields = line.split()
        if fields and fields[0] == 'RUNWAY': lengths[fields[1]] = int(fields[2])
    for line in text.splitlines():
        t = re.search(r'\[T=(\d+)\]', line)
        if not t: continue
        tick = int(t[1])
        if '[REQUEST' in line:
            m = re.search(r'\] (\S+) requests .*required=(\d+)', line);
            required[m[1]] = int(m[2])
        if '[START' in line:
            m = re.search(r'\] (\S+) started \S+ on (\S+);', line);
            plane, runway = m.groups()
            assert required[plane] <= lengths[runway], line
            assert runway not in active, line
            assert plane not in starts, line
            assert tick >= free.get(runway, 0), line
            starts.add(plane);
            active[runway] = plane
        if '[FINISH' in line:
            m = re.search(r'\] (\S+) completed \S+ on (\S+)', line)
            assert active.pop(m[2]) == m[1], line
        if '[RELEASE' in line:
            m = re.search(r'\] (\S+) released; next start not before T=(\d+)', line);
            free[m[1]] = int(m[2])
        if '[FUEL' in line: assert int(line.rsplit(' ', 1)[1]) >= 0
    assert '[ERROR' not in text, text


def run(mode, config, external=False, interrupt=False, extra=(), kill_worker=False, stop_signal=signal.SIGINT):
    if config == 'test_duplicate.conf': external = False
    before = inventory();
    session = f't{os.getpid()}_{len(results)}'
    args = [BIN, '--mode', mode, '--session', session, '-c', 'configs/' + config, '--no-log', *extra]
    if external: args += ['--external']
    workers = []
    p = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    prefix = ''
    try:
        if external:
            while True:
                line = p.stdout.readline();
                prefix += line
                if line.startswith('READY '):
                    count = int(re.search(r'actors=(\d+)', line)[1]);
                    break
                assert line, prefix
            for i in range(count):
                workers.append(subprocess.Popen([BIN, '--worker', mode, session, str(i)], stdout=subprocess.PIPE,
                                                stderr=subprocess.STDOUT, text=True))
        if interrupt or kill_worker:
            time.sleep(.3)
            if kill_worker:
                workers[0].kill()
            else:
                p.send_signal(stop_signal)
        text = prefix + p.communicate(timeout=40)[0]
        for w in workers:
            wt = w.communicate(timeout=10)[0]
            assert w.returncode == 0 or kill_worker, (w.returncode, wt)
        expected = 3 if kill_worker else 130 if interrupt else 2 if config == 'test_duplicate.conf' else 0
        assert p.returncode == expected, (mode, config, p.returncode, text)
        if expected in (0, 130): check_events(text, config)
        assert inventory() == before, (inventory() - before, text)
        return text
    finally:
        if p.poll() is None: p.kill();p.wait()
        for w in workers:
            if w.poll() is None: w.kill();w.wait()


def record(label, fn):
    fn();
    results.append('PASS ' + label);
    print(results[-1], flush=True)
    LOG.write_text('\n'.join(results) + '\n', encoding='utf-8')


def scenario(mode, config, external):
    text = run(mode, config, external)
    if config == 'test_nominal.conf': assert 'landed=2, took_off=2, diverted=0' in text
    if config == 'test_emergency.conf':
        assert 'EMERGENCY' in text
        assert text.index('ASSIGN   ] ATC assigned A001') < text.index('ASSIGN   ] ATC assigned D002')
    if config == 'test_incompatible.conf': assert 'termination=SHIFT_ENDED' in text and 'diverted=1' in text and 'waiting=1' in text
    if config == 'test_closure.conf':
        assert '[CLOSE' in text and '[OPEN' in text
        assert 'D001 completed TAKEOFF' in text
        for line in text.splitlines():
            if '[START' in line: assert not 2 <= int(re.search(r'T=(\d+)', line)[1]) < 8
    if config == 'test_empty.conf': assert 'aircraft_total=0' in text
    if config == 'test_zero_fuel.conf': assert 'diverted=1' in text
    if config == 'test_safety.conf': assert 'took_off=3' in text


configs = ['test_nominal.conf', 'test_emergency.conf', 'test_incompatible.conf', 'test_closure.conf',
           'test_safety.conf', 'test_empty.conf', 'test_zero_fuel.conf', 'test_duplicate.conf']
for mode in ['named', 'unnamed', 'shm', 'fifo']:
    external = mode in ('shm', 'fifo')
    for c in configs: record(f'{mode} {c} independent={external}', lambda: scenario(mode, c, external))
    for sig in [False, True]:
        if sig: record(f'{mode} SIGINT cleanup', lambda: run(mode, 'test_interrupt.conf', external, interrupt=True))
    record(f'{mode} default random flow', lambda: run(mode, 'default.conf', external))
for mode in ['shm', 'fifo']:
    record(f'{mode} killed worker -> IPC_FAILURE cleanup',
           lambda: run(mode, 'test_interrupt.conf', True, kill_worker=True))

streams = []
for mode in ['named', 'unnamed', 'shm', 'fifo']:
    text = run(mode, 'default.conf', mode in ('shm', 'fifo'))
    streams.append('\n'.join(l for l in text.splitlines() if l.startswith('[T=')))
record('identical seed -> identical event streams in all four modes',
       lambda: (_ for _ in ()).throw(AssertionError('different streams')) if len(set(streams)) != 1 else None)
for mode in ['named', 'unnamed', 'shm', 'fifo']:
    record(f'{mode} SIGTERM cleanup', lambda: run(mode, 'test_interrupt.conf', mode in ('shm', 'fifo'), interrupt=True,
                                                  stop_signal=signal.SIGTERM))


def group_interrupt(mode):
    before = inventory()
    p = subprocess.Popen([BIN, '--mode', mode, '-c', 'configs/test_interrupt.conf', '--no-log'], stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, text=True, start_new_session=True)
    time.sleep(.3);
    os.killpg(p.pid, signal.SIGINT)
    text = p.communicate(timeout=15)[0]
    assert p.returncode == 130, text
    assert inventory() == before


for mode in ['named', 'unnamed']: record(f'{mode} terminal process-group Ctrl+C', lambda: group_interrupt(mode))


def cancel_startup():
    before = inventory()
    p = subprocess.Popen([BIN, '--mode', 'fifo', '--external', '-c', 'configs/test_nominal.conf', '--no-log'],
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    time.sleep(.3);
    p.send_signal(signal.SIGINT)
    text = p.communicate(timeout=5)[0]
    assert p.returncode == 130, text
    assert inventory() == before


record('SIGINT while waiting for independent actors', cancel_startup)


def helper(mode, interrupt):
    before = inventory()
    args = ['python3', 'scripts/run_independent.py', '--mode', mode, '-c',
            'configs/' + ('test_interrupt.conf' if interrupt else 'test_nominal.conf'), '--no-log']
    p = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, start_new_session=True)
    if interrupt:
        time.sleep(.5);
        os.killpg(p.pid, signal.SIGINT)
    text = p.communicate(timeout=15)[0]
    assert p.returncode == (130 if interrupt else 0), text
    assert inventory() == before


for mode in ['shm', 'fifo']:
    for interrupted in [False, True]:
        record(f'{mode} independent launcher interrupt={interrupted}', lambda: helper(mode, interrupted))
print(f'{len(results)} tests passed')
