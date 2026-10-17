#!/usr/bin/env python3
import os
import socket
import sys
from pathlib import Path
import subprocess
import tempfile
import time
from fixture import create

try:
    probe = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    probe.close()
except PermissionError:
    print('SKIPPED daemon/CLI integration: environment denies Unix sockets; policy/controller tests still run.')
    sys.exit(0)

PROJECT = Path(__file__).resolve().parents[1]
checks = 0

def check(condition, message):
    global checks
    if not condition:
        raise AssertionError(message)
    checks += 1

with tempfile.TemporaryDirectory(prefix='kx-') as tmp:
    root = create(Path(tmp) / 'sys')
    run = Path(tmp) / 'run'
    log = open(Path(tmp) / 'daemon.log', 'w+')
    daemon = None

    def start():
        global daemon
        daemon = subprocess.Popen([PROJECT/'build/kernoryxd', '--simulate', root, '--runtime', run], stdout=log, stderr=log)
        end = time.monotonic() + 4
        while time.monotonic() < end:
            if daemon.poll() is not None:
                log.seek(0)
                raise AssertionError(log.read())
            if (run/'control.sock').exists():
                try:
                    cmd('status')
                    return
                except AssertionError:
                    pass
            time.sleep(.03)
        raise AssertionError('Daemon startup timeout')

    def cmd(*args, ok=True):
        p = subprocess.run([PROJECT/'build/kernoryx', '--socket', run/'control.sock', *args], capture_output=True, text=True, timeout=5)
        check((p.returncode == 0) == ok, f'{args}: {p.stdout} {p.stderr}')
        return p.stdout + p.stderr

    def value(name):
        return (root/name).read_text().strip()

    def change(name, data):
        (root/name).write_text(data+'\n')

    def wait_for(needle):
        end = time.monotonic() + 4
        while time.monotonic() < end:
            if needle in cmd('status'):
                return
            time.sleep(.1)
        raise AssertionError('Missing state: ' + needle)

    profile = 'firmware/acpi/platform_profile'
    epp = 'devices/system/cpu/cpufreq/policy0/energy_performance_preference'
    temp = 'class/hwmon/hwmon0/temp1_input'
    try:
        start()
        cmd('doctor')
        cmd('mode', 'list')
        cmd('mode', 'set', 'beast', '--dry-run')
        check(value(profile) == 'balanced' and not (run/'rollback').exists(), 'dry-run changed hardware/state')
        cmd('mode', 'set', 'beast', '--for', '2s')
        check(value(epp) == 'performance', 'Beast EPP')
        cmd('mode', 'set', 'eco', ok=False)
        wait_for('Requested: unmanaged')
        check(value(epp) == 'balance_power', 'expiry snapshot restore')
        cmd('mode', 'set', 'balanced')
        cmd('mode', 'set', 'beast', '--for', '2s')
        cmd('mode', 'set', 'eco', '--replace')
        time.sleep(2.1)
        check('Requested: eco' in cmd('status') and value(profile) == 'low-power', 'stale lease overwrite')
        cmd('mode', 'set', 'balanced')
        cmd('mode', 'set', 'beast', '--for', '1m')
        cmd('mode', 'set', 'performance', '--for', '1m', '--replace')
        cmd('mode', 'restore')
        check('Requested: balanced' in cmd('status'), 'replacement must retain pre-lease mode')
        cmd('mode', 'set', 'beast')
        change(temp, '88000')
        wait_for('Effective policy: balanced')
        change(temp, '96000')
        wait_for('Requested: eco')
        check('Lease remaining: 0 s' in cmd('status'), 'critical did not cancel lease')
        cmd('mode', 'set', 'beast', ok=False)
        change(temp, '60000')
        cmd('mode', 'set', 'performance')
        change('class/power_supply/AC/online', '0')
        wait_for('Effective policy: balanced')
        change('class/power_supply/AC/online', '1')
        (root/temp).unlink()
        wait_for('CPU temperature unavailable')
        change(temp, '55000')
        cmd('mode', 'release')
        cmd('mode', 'set', 'beast')
        change(profile, 'low-power')
        wait_for('Requested: external-change')
        cmd('mode', 'set', 'balanced', ok=False)
        cmd('mode', 'release')
        check(value(profile) == 'low-power', 'external change overwritten')
        change(profile, 'balanced')
        change(epp, 'balance_power')
        change('devices/system/cpu/cpufreq/policy1/energy_performance_preference', 'balance_power')
        cmd('mode', 'set', 'beast')
        daemon.kill(); daemon.wait()
        check((run/'rollback').exists(), 'no crash journal')
        start()
        check(value(profile) == 'balanced' and value(epp) == 'balance_power', 'crash recovery')
        check(not (run/'rollback').exists(), 'recovery journal not cleared')
        cmd('mode', 'set', 'eco')
        daemon.terminate(); daemon.wait(timeout=5)
        check(value(profile) == 'balanced', 'clean shutdown restoration')
        start()
        for args in [('mode','set','bad'), ('mode','set','beast','--for','3h'), ('mode','set','eco','--what'), ('mode','set','eco','--for','1m','--for','2m')]:
            cmd(*args, ok=False)
        cmd('mode', 'set', 'beast')
        daemon.kill(); daemon.wait()
        change(profile, 'low-power')
        failed = subprocess.run([PROJECT/'build/kernoryxd', '--simulate', root, '--runtime', run], capture_output=True, text=True, timeout=5)
        check(failed.returncode != 0 and (run/'rollback').exists(), 'recovery conflict must preserve journal')
        check(value(profile) == 'low-power', 'recovery overwrote external change')
        print(f'Integration passed: {checks} checks (including polling requests)')
    finally:
        if daemon and daemon.poll() is None:
            daemon.terminate(); daemon.wait(timeout=5)
        log.close()
