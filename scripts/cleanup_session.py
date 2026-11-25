#!/usr/bin/env python3
import ctypes, os, re, sys
from pathlib import Path

if len(sys.argv) != 2 or not re.fullmatch(r'[A-Za-z0-9_]{1,40}', sys.argv[1]):
    sys.exit('Usage: python3 scripts/cleanup_session.py SESSION (all session processes must be stopped)')
base = f'airport_{os.getuid()}_{sys.argv[1]}_'
libc = ctypes.CDLL(None, use_errno=True)
for directory in ['/dev/shm', '/tmp']:
    for p in Path(directory).iterdir():
        logical = p.name[4:] if p.name.startswith('sem.') else p.name
        if not logical.startswith(base): continue
        tail = logical[len(base):]
        if not re.fullmatch(r'(gate|claim)_\d+|ack|ready|world|request|reply', tail): continue
        if directory == '/tmp':
            if tail not in ('request', 'reply') or not p.is_fifo(): continue
            p.unlink()
        else:
            func = libc.sem_unlink if p.name.startswith('sem.') else libc.shm_unlink
            if func(('/' + logical).encode()) != 0: raise OSError(ctypes.get_errno(), str(p))
        print('Removed', logical)
