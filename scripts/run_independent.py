#!/usr/bin/env python3
import argparse, os, re, signal, subprocess, sys
from pathlib import Path

root = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--mode', choices=['shm', 'fifo'], default='fifo')
p.add_argument('--session', default='demo' + str(os.getpid()))
args, rest = p.parse_known_args()
quiet = "--quiet" in rest
if quiet: rest.remove("--quiet")
os.chdir(root)
signal.signal(signal.SIGINT, lambda signum, frame: None)
controller = subprocess.Popen(
    [str(root / ('build/airport_' + args.mode)), '--session', args.session, '--external', *rest],
    stdout=subprocess.PIPE, text=True)
workers = []
try:
    for line in controller.stdout:
        if not quiet: print(line, end='', flush=True)
        if line.startswith('READY '):
            count = int(re.search(r'actors=(\d+)', line)[1])
            for actor in range(count):
                workers.append(
                    subprocess.Popen([str(root / 'build/airport'), '--worker', args.mode, args.session, str(actor)]))
    result = controller.wait()
    for worker in workers:
        worker.wait(timeout=10)
    sys.exit(result)
finally:
    if controller.poll() is None:
        controller.terminate();
        controller.wait(timeout=30)
    for worker in workers:
        if worker.poll() is None: worker.terminate();worker.wait(timeout=10)
