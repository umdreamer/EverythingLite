#!/usr/bin/env python3
"""Actual Mac helper, explicit jobs only; never reads the user's index."""
import errno
import fcntl
import os
from pathlib import Path
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import time

cli, fixture = map(str, sys.argv[1:3])


def run(args, env, success=False):
    result = subprocess.run([cli, *args], env=env, capture_output=True, text=True, timeout=15)
    assert (result.returncode == 0) == success, (args, result.returncode, result.stdout, result.stderr)
    if not success:
        assert 'completed=' not in result.stdout and '完成' not in result.stdout
    return result


def make(base, name, env):
    root = base / name
    root.mkdir(mode=0o700)
    subprocess.run([fixture, '--fixture', str(root)], env=env, capture_output=True, check=True, timeout=15)
    return root, ['scan-helper', '--request', str(root / 'job/request.bin'), '--nonce', 'synthetic-nonce']


def lock_busy(fd):
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError as error:
        assert error.errno in (errno.EAGAIN, errno.EACCES)
        return True
    fcntl.flock(fd, fcntl.LOCK_UN)
    return False


with tempfile.TemporaryDirectory(prefix='everything-lite-cli-scan-', dir='/private/tmp') as temporary:
    base = Path(temporary)
    sentinel = base / 'environment-main.sqlite'
    sentinel.write_bytes(b'malformed main index sentinel\x00immutable')
    before = sentinel.read_bytes()
    env = os.environ.copy()
    env['EVERYTHING_LITE_DB'] = str(sentinel)
    root, args = make(base, 'success', env)
    result = run(args, env, success=True)
    assert result.stdout == 'completed=1 staged=2\n'
    assert sentinel.read_bytes() == before
    with sqlite3.connect(f'file:{root / "job/snapshot.sqlite"}?mode=ro', uri=True) as db:
        assert db.execute('SELECT complete FROM snapshot_metadata').fetchall() == [(1,)]
        assert len(db.execute('PRAGMA table_info(snapshot_files)').fetchall()) == 12
        assert db.execute('SELECT COUNT(*) FROM snapshot_files').fetchone() == (2,)
        assert db.execute('SELECT nonce FROM snapshot_proof').fetchone() == ('synthetic-nonce',)
        assert db.execute("SELECT COUNT(*) FROM sqlite_master WHERE name='files'").fetchone() == (0,)
    inode = (root / 'locks/source-7.scan.lock').stat().st_ino
    run(args, env)  # existing snapshot cannot be overwritten
    assert (root / 'locks/source-7.scan.lock').stat().st_ino == inode
    root, args = make(base, 'bad-nonce', env)
    run(args[:-1] + ['wrong-nonce'], env)
    assert not (root / 'job/snapshot.sqlite').exists()
    for extra in (['--db', str(base / 'forbidden.sqlite')], ['--trace-search']):
        run(extra + args, env)
        run(args + extra, env)
    for malformed in (['scan-helper'], args[:-1], args + ['extra'], args + ['--request', args[2]], ['--db']):
        run(malformed, env)
    assert not (base / 'forbidden.sqlite').exists()
    value = run(['--db', 'scan-helper', 'db-path'], env, success=True)
    assert value.stdout == 'scan-helper\n'  # database value is not a command
    root, args = make(base, 'missing-root', env)
    shutil.rmtree(root / 'root')
    run(args, env)
    assert not (root / 'job/snapshot.sqlite').exists()
    root, args = make(base, 'malformed', env)
    (root / 'job/request.bin').write_bytes(b'malformed explicit task')
    run(args, env)
    root, args = make(base, 'fifo-manifest', env)
    (root / 'job/request.bin').unlink()
    os.mkfifo(root / 'job/request.bin', 0o600)
    run(args, env)
    root, args = make(base, 'busy', env)
    lock = root / 'locks/source-7.scan.lock'
    with lock.open('w+b') as holder:
        os.chmod(lock, 0o600)
        fcntl.flock(holder, fcntl.LOCK_EX | fcntl.LOCK_NB)
        inode = lock.stat().st_ino
        run(args, env)
        assert not (root / 'job/snapshot.sqlite').exists()
    run(args, env, success=True)
    assert lock.stat().st_ino == inode
    absent = base / 'absent-environment.sqlite'
    env['EVERYTHING_LITE_DB'] = str(absent)
    root, args = make(base, 'absent-main', env)
    run(args, env, success=True)
    assert not absent.exists() and sentinel.read_bytes() == before

    # Fill stdout pipe before spawn. Once snapshot complete=1 is visible and the
    # child is still alive, it is blocked on completion output/exit flush. The
    # source lock must remain held until terminate AND actual wait/reap.
    root, args = make(base, 'exit-boundary', env)
    lock = root / 'locks/source-7.scan.lock'
    read_fd, write_fd = os.pipe()
    child = None
    try:
        os.set_blocking(write_fd, False)
        while True:
            try:
                os.write(write_fd, b'x' * 4096)
            except BlockingIOError:
                break
        os.set_blocking(write_fd, True)
        child = subprocess.Popen([cli, *args], env=env, stdout=write_fd, stderr=subprocess.PIPE)
        os.close(write_fd)
        write_fd = -1
        deadline = time.monotonic() + 15
        complete = False
        while time.monotonic() < deadline:
            assert child.poll() is None, 'helper exited before blocking output boundary'
            snapshot = root / 'job/snapshot.sqlite'
            if snapshot.exists():
                try:
                    with sqlite3.connect(f'file:{snapshot}?mode=ro', uri=True, timeout=0.1) as db:
                        complete = db.execute('SELECT complete FROM snapshot_metadata').fetchone() == (1,)
                except sqlite3.Error:
                    pass
            if complete:
                break
            time.sleep(0.02)
        assert complete and child.poll() is None
        time.sleep(0.15)  # allow local cleanup/exit flush to reach blocked stdout
        assert child.poll() is None
        inode = lock.stat().st_ino
        with lock.open('r+b') as other:
            assert lock_busy(other.fileno()), 'lock released before actual helper exit'
            child.terminate()
            child.wait(timeout=10)
            assert not lock_busy(other.fileno())
        assert lock.stat().st_ino == inode
    finally:
        if child is not None:
            if child.poll() is None:
                child.kill()
            child.wait(timeout=10)
            child.stderr.close()
        if write_fd >= 0:
            os.close(write_fd)
        os.close(read_fd)
    assert not absent.exists() and sentinel.read_bytes() == before
print('actual scan helper tests passed; stdout-blocked child retained lock until actual exit')
