#!/usr/bin/env python3
"""Kernoryx typed maintenance runner. No shell evaluation; no hardware leases here."""

from contextlib import contextmanager
import argparse
import errno
import fcntl
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import time

BIN = "@CMAKE_INSTALL_PREFIX@/bin/kernoryx"  # replaced by CMake at install configuration
HOME = Path.home()


def run(argv, timeout=120):
    result = subprocess.run(
        argv,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=timeout,
        env={
            "PATH": "/usr/bin:/bin",
            "LC_ALL": "C",
            "HOME": str(HOME),
            "XDG_RUNTIME_DIR": f"/run/user/{os.getuid()}",
        },
    )
    if result.returncode:
        raise ValueError(f"{argv[0]} failed ({result.returncode}): {result.stdout.strip()}")
    return result.stdout.strip()


def native(name):
    path = Path("/usr/bin") / name
    if not path.is_file():
        raise ValueError(f"{name} unavailable; install its native package to enable this provider")
    return str(path)


def root_required():
    if os.geteuid() != 0:
        raise ValueError("This system operation requires sudo")


def user_required():
    if os.geteuid() == 0:
        raise ValueError("Run user cache/trash cleanup without sudo, as the owning user")


def natural(n, low, high, label):
    if type(n) is not int or not low <= n <= high:
        raise ValueError(f"{label} must be {low}..{high}")
    return n


def duration(value):
    if not isinstance(value, str) or not re.fullmatch(r"[1-9][0-9]{0,5}[smh]", value):
        raise ValueError("Duration needs s/m/h suffix")
    return natural(int(value[:-1]) * {"s": 1, "m": 60, "h": 3600}[value[-1]], 1, 7200, "seconds")


def secure_dir(path):
    """Pin every component without following symlinks, even during rename races."""
    path = Path(path)
    if not path.is_absolute() or ".." in path.parts:
        raise ValueError("Absolute, non-traversing directory required")
    fd = os.open("/", os.O_RDONLY | os.O_DIRECTORY)
    try:
        for part in path.parts[1:]:
            nxt = os.open(part, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=fd)
            os.close(fd)
            fd = nxt
        return fd
    except BaseException:
        os.close(fd)
        raise


def memory_status(proc=Path("/proc")):
    values = {}
    for line in (proc / "meminfo").read_text().splitlines():
        k, v = line.split(":", 1)
        values[k] = int(v.split()[0])
    lines = [
        f"Available: {values.get('MemAvailable',0)} kB / {values.get('MemTotal',0)} kB",
        "Cached RAM is reusable; no global cache flush is offered.",
    ]
    try:
        psi = (proc / "pressure/memory").read_text().strip()
        avgs = {line.split()[0]: float(line.split()[1].split("=")[1]) for line in psi.splitlines()}
        if any(not 0 <= v <= 100 for v in avgs.values()):
            raise ValueError("Invalid PSI")
        severity = (
            "high"
            if avgs.get("full", 0) >= 1 or avgs.get("some", 0) >= 10
            else "observed" if avgs.get("some", 0) > 0 else "not observed"
        )
        lines += [f"Memory stalls: {severity} (heuristic from PSI avg10, not a diagnosis)", psi]
    except FileNotFoundError:
        lines += ["PSI unavailable; available RAM alone does not establish pressure."]
    return "\n".join(lines)


def validate_reclaim(group, amount):
    if not isinstance(group, str) or not isinstance(amount, str):
        raise ValueError("Cgroup and amount must be strings")
    if not re.fullmatch(r"[1-9][0-9]{0,9}[KMG]?", amount):
        raise ValueError("Amount must be positive bytes or K/M/G")
    multiplier = {"K": 1024, "M": 1024**2, "G": 1024**3}.get(amount[-1], 1)
    size = int(amount[:-1] if multiplier > 1 else amount) * multiplier
    if size > 8 * 1024**3:
        raise ValueError("Single reclaim request capped at 8 GiB")
    parts = Path(group).parts
    if not parts or Path(group).is_absolute() or any(x in (".", "..") for x in parts):
        raise ValueError("Use a non-root cgroup path relative to /sys/fs/cgroup")


def memory_reclaim(group, amount, dry, base=Path("/sys/fs/cgroup")):
    validate_reclaim(group, amount)
    if not (base / "cgroup.controllers").exists():
        raise ValueError("cgroup v2 unavailable")
    fd = secure_dir(base / Path(group))
    try:
        # Merely opening for writing may need delegation privileges.
        if stat.S_ISLNK(os.stat("memory.reclaim", dir_fd=fd, follow_symlinks=False).st_mode):
            raise ValueError("Symlink reclaim control refused")
        if dry:
            return f"DRY RUN: request {amount} from {group}; actual reclaim may differ"
        out = os.open("memory.reclaim", os.O_WRONLY | os.O_NOFOLLOW, dir_fd=fd)
        try:
            try:
                os.write(out, amount.encode())
            except OSError as e:
                if e.errno == errno.EAGAIN:
                    return "Partial reclaim: kernel could not reclaim the full requested amount (EAGAIN)"
                raise
        finally:
            os.close(out)
    finally:
        os.close(fd)
    return "Reclaim request completed; this does not measure bytes freed"


def cache_candidates(path, days):
    cutoff = time.time() - days * 86400
    fd = secure_dir(path)
    try:
        device = os.fstat(fd).st_dev
        for parent, dirs, files, dfd in os.fwalk(".", topdown=True, follow_symlinks=False, dir_fd=fd):
            dirs[:] = [
                n
                for n in dirs
                if (lambda s: stat.S_ISDIR(s.st_mode) and s.st_dev == device)(
                    os.stat(n, dir_fd=dfd, follow_symlinks=False)
                )
            ]
            for name in files:
                s = os.stat(name, dir_fd=dfd, follow_symlinks=False)
                if (
                    stat.S_ISREG(s.st_mode)
                    and s.st_nlink == 1
                    and s.st_dev == device
                    and max(s.st_mtime, s.st_atime) < cutoff
                ):
                    yield str(Path(parent) / name), s
    finally:
        os.close(fd)


def cache_clean(path, days, apply):
    user_required()
    path = Path(path)
    cache = HOME / ".cache"
    if not path.is_absolute() or ".." in path.parts or path == cache or not path.is_relative_to(cache):
        raise ValueError("Explicit cache subdirectory under ~/.cache required; whole-cache cleanup refused")
    candidates = list(cache_candidates(path, days))
    print(
        f"Eligible: {len(candidates)} files, {sum(s.st_size for _,s in candidates)} logical bytes (allocated/free space may differ)"
    )
    removed = 0
    for relative, old in candidates:
        print(("DELETE " if apply else "WOULD DELETE ") + str(path / relative))
        if apply:
            fd = secure_dir((path / relative).parent)
            try:
                now = os.stat(Path(relative).name, dir_fd=fd, follow_symlinks=False)
                if (now.st_dev, now.st_ino, now.st_size, now.st_mtime_ns, now.st_atime_ns, now.st_nlink) != (
                    old.st_dev,
                    old.st_ino,
                    old.st_size,
                    old.st_mtime_ns,
                    old.st_atime_ns,
                    old.st_nlink,
                ):
                    raise ValueError("Cache changed after scan; rescan required")
                os.unlink(Path(relative).name, dir_fd=fd)
                removed += 1
            finally:
                os.close(fd)
    return f"{removed} files removed" if apply else "DRY RUN; directories and symlinks retained"


def trash_clean(days, apply):
    user_required()
    # Native trash-cli understands trashinfo timestamps, mount trash, and directory entries.
    tool = native("trash-empty")
    helptext = run([tool, "--help"])
    if "--dry-run" not in helptext:
        raise ValueError("Installed trash-cli lacks --dry-run; upgrade to enable safe trash preview")
    preview = run([tool, "--dry-run", str(days)])
    print(preview or "No eligible trash entries")
    if apply:
        return run([tool, str(days)]) or "Trash retention applied"
    return "DRY RUN; retention uses trash deletion dates"


def cleanup(provider, days=30, keep=3, path=None, apply=False):
    natural(days, 1, 3650, "days")
    natural(keep, 1, 100, "keep")
    if provider == "cache":
        if not path:
            raise ValueError("cache needs --path to an explicit cache directory")
        return cache_clean(path, days, apply)
    if path is not None:
        raise ValueError("--path is only supported for cache")
    if provider == "trash":
        return trash_clean(days, apply)
    if provider == "packages":
        tool = native("paccache")  # Arch/CachyOS: preserve newest package versions.
        args = ["-k", str(keep), "--min-mtime", f"{days} days ago"]
        print(run([tool, "-d", *args]) or "No eligible package archives")
        if apply:
            root_required()
            return run([tool, "-r", *args]) or "Package cache retention applied"
        return "DRY RUN; paccache retains newest versions per package"
    if provider == "journal":
        tool = native("journalctl")
        print(run([tool, "--disk-usage"]))
        print(
            f"Policy: vacuum archived journals older than {days} days; usage is not an exact reclaim estimate"
        )
        if apply:
            root_required()
            return run([tool, f"--vacuum-time={days}d"])
        return "DRY RUN; active journal files are retained"
    if provider == "temp":
        tool = native("systemd-tmpfiles")
        if "--dry-run" not in run([tool, "--help"]):
            raise ValueError("systemd-tmpfiles lacks --dry-run; this provider is unavailable")
        print(run([tool, "--clean", "--dry-run"]))
        print("Retention comes from system tmpfiles.d rules; --days does not override those rules")
        if apply:
            root_required()
            return run([tool, "--clean"])
        return "DRY RUN"
    raise ValueError("Unknown cleanup provider")


def validate_action(action):
    if not isinstance(action, dict):
        raise ValueError("Action must be an object")
    kind = action.get("type")
    schemas = {
        "cleanup": {"type", "provider", "days", "keep", "path"},
        "mode": {"type", "mode", "for", "replace"},
        "fan": {"type", "for", "replace", "experimental"},
        "memory-reclaim": {"type", "cgroup", "amount"},
    }
    if not isinstance(kind, str) or kind not in schemas or action.keys() - schemas[kind]:
        raise ValueError("Unknown action type/field")
    if kind == "cleanup":
        if not isinstance(action.get("provider"), str) or action.get("provider") not in {
            "cache",
            "trash",
            "packages",
            "journal",
            "temp",
        }:
            raise ValueError("Unknown cleanup provider")
        natural(action.get("days", 30), 1, 3650, "days")
        natural(action.get("keep", 3), 1, 100, "keep")
        if action["provider"] == "cache" and not isinstance(action.get("path"), str):
            raise ValueError("Cache path required")
        if "path" in action and action["provider"] != "cache":
            raise ValueError("Path is only valid for cache")
    if kind in {"mode", "fan"}:
        duration(action.get("for"))
        if type(action.get("replace", False)) is not bool:
            raise ValueError("replace must be boolean")
    if kind == "fan" and type(action.get("experimental", False)) is not bool:
        raise ValueError("experimental must be boolean")
    if kind == "mode" and (
        not isinstance(action.get("mode"), str)
        or action.get("mode") not in {"eco", "balanced", "performance", "beast"}
    ):
        raise ValueError("Invalid mode")
    if kind == "memory-reclaim":
        validate_reclaim(action.get("cgroup"), action.get("amount"))
    return action


def validate_workflow(data):
    if (
        not isinstance(data, dict)
        or set(data) != {"version", "actions"}
        or type(data["version"]) is not int
        or data["version"] != 1
    ):
        raise ValueError("Workflow needs version:1 and actions only")
    if not isinstance(data["actions"], list) or not 1 <= len(data["actions"]) <= 32:
        raise ValueError("Use 1..32 actions")
    for action in data["actions"]:
        validate_action(action)
    for kind in ("mode", "fan"):
        if sum(a["type"] == kind for a in data["actions"]) > 1:
            raise ValueError("A workflow may start at most one lease per hardware subsystem")
    # Slow work cannot unexpectedly outlive a hardware session in the same workflow.
    seen_hardware = False
    for a in data["actions"]:
        if a["type"] in {"mode", "fan"}:
            seen_hardware = True
        elif seen_hardware:
            raise ValueError("Put maintenance before temporary hardware actions")
    return data


def trusted_json(path):
    path = Path(path)
    fd = secure_dir(path.absolute().parent)
    try:
        f = os.open(path.name, os.O_RDONLY | os.O_NOFOLLOW, dir_fd=fd)
    finally:
        os.close(fd)
    with os.fdopen(f) as stream:
        s = os.fstat(stream.fileno())
        if not stat.S_ISREG(s.st_mode) or s.st_size > 65536 or s.st_uid != os.geteuid() or s.st_mode & 0o022:
            raise ValueError(
                "Configuration must be a regular file owned by caller, not group/world-writable, <=64KiB"
            )
        return json.load(stream)


def daemon(args):
    executable = BIN
    sibling = Path(__file__).resolve().parent / "kernoryx"
    if sibling.is_file():
        executable = str(sibling)
    return run([executable, *args], timeout=20)


def action_run(a, dry):
    validate_action(a)
    if a["type"] == "cleanup":
        return cleanup(a["provider"], a.get("days", 30), a.get("keep", 3), a.get("path"), not dry)
    if a["type"] == "memory-reclaim":
        return memory_reclaim(a["cgroup"], a["amount"], dry)
    cmd = ["mode", "set", a["mode"]] if a["type"] == "mode" else ["fan", "boost"]
    cmd += ["--for", a["for"]]
    if a.get("replace"):
        cmd += ["--replace"]
    if a.get("experimental"):
        cmd += ["--experimental"]
    if dry:
        cmd += ["--dry-run"]
    return daemon(cmd)


def workflow_run(data, dry):
    validate_workflow(data)
    # Full schema validation before the first side effect. Execution stops at first failure.
    for i, a in enumerate(data["actions"]):
        print(f"Action {i+1}/{len(data['actions'])}: {a['type']}")
        print(action_run(a, dry))
    return "Workflow complete" + (" (DRY RUN)" if dry else "")


def unit_name(name):
    if not re.fullmatch(r"[a-z][a-z0-9-]{0,47}", name):
        raise ValueError("Name: lowercase letters/digits/hyphens, max 48")
    return "kernoryx-" + name


def timer_text(name, calendar, persistent, executable):
    unit_name(name)
    if not calendar or any(c in calendar for c in "\r\n\x00%\\"):
        raise ValueError("Invalid calendar expression")
    if any(c in executable for c in ' "\r\n%\\'):
        raise ValueError("Unsupported executable path")
    return (
        f"[Unit]\nDescription=Kernoryx workflow {name}\n\n[Service]\nType=oneshot\n"
        f"ExecStart={executable} workflow run /etc/kernoryx/workflows/{name}.json --apply\n"
        "TimeoutStartSec=10min\n",
        f"[Unit]\nDescription=Kernoryx schedule {name}\n\n[Timer]\nOnCalendar={calendar}\n"
        f"Persistent={'true' if persistent else 'false'}\nRandomizedDelaySec=0\nUnit=kernoryx-{name}.service\n"
        "\n[Install]\nWantedBy=timers.target\n",
    )


def atomic_new(path, text):
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o644)
    with os.fdopen(fd, "w") as f:
        f.write(text)
        f.flush()
        os.fsync(f.fileno())


def schedule_install(args):
    data = validate_workflow(trusted_json(args.file))
    # Root timers cannot process someone else's home or infer a desktop session.
    if any(a["type"] == "cleanup" and a["provider"] in {"cache", "trash"} for a in data["actions"]):
        raise ValueError("System schedules support system cleanup only; run user cleanup without sudo")
    if args.persistent and any(a["type"] in {"mode", "fan", "memory-reclaim"} for a in data["actions"]):
        raise ValueError(
            "Catch-up scheduling is only supported for cleanup; missed performance jobs must not run at boot"
        )
    for action in data["actions"]:
        native_timer = {"packages": "paccache.timer", "temp": "systemd-tmpfiles-clean.timer"}.get(
            action.get("provider")
        )
        if native_timer:
            result = subprocess.run([native("systemctl"), "is-active", "--quiet", native_timer], timeout=5)
            if result.returncode == 0:
                raise ValueError(
                    f"{native_timer} already performs this maintenance; use it or explicitly disable it before adding a duplicate schedule"
                )
    service, timer = timer_text(args.name, args.calendar, args.persistent, BIN)
    print(run([native("systemd-analyze"), "calendar", args.calendar]))
    print(service + "\n" + timer)
    if not args.apply:
        return "DRY RUN; no timer files installed"
    root_required()
    base = Path("/etc/kernoryx/workflows")
    base.mkdir(parents=True, exist_ok=True, mode=0o755)
    fd = secure_dir(base)
    try:
        st = os.fstat(fd)
        if st.st_uid != 0 or st.st_mode & 0o022:
            raise ValueError("Untrusted workflow directory")
    finally:
        os.close(fd)
    unit = unit_name(args.name)
    items = [
        (base / (args.name + ".json"), json.dumps(data, indent=2) + "\n"),
        (Path("/etc/systemd/system") / (unit + ".service"), service),
        (Path("/etc/systemd/system") / (unit + ".timer"), timer),
    ]
    created = []
    try:
        for p, text in items:
            atomic_new(p, text)
            created.append(p)
        run([native("systemctl"), "daemon-reload"])
        run([native("systemctl"), "enable", "--now", unit + ".timer"])
    except BaseException:
        # Leave reviewable files after a systemctl failure; do not remove files while a timer may be active.
        if created:
            print("Partial installation retained; inspect schedule status before retrying", file=sys.stderr)
        raise
    return "Schedule installed and enabled"


def parse_args():
    p = argparse.ArgumentParser(description=__doc__)
    groups = p.add_subparsers(dest="group", required=True)
    mem = groups.add_parser("memory").add_subparsers(dest="operation", required=True)
    mem.add_parser("status")
    reclaim = mem.add_parser("reclaim")
    reclaim.add_argument("cgroup")
    reclaim.add_argument("amount")
    reclaim.add_argument("--dry-run", action="store_true")
    clean = groups.add_parser("cleanup")
    clean.add_argument("operation", choices=["scan", "run"])
    clean.add_argument("provider", choices=["packages", "journal", "temp", "trash", "cache"])
    clean.add_argument("--days", type=int, default=30)
    clean.add_argument("--keep", type=int, default=3)
    clean.add_argument("--path")
    clean.add_argument("--apply", action="store_true")
    clean.add_argument("--dry-run", action="store_true")
    flow = groups.add_parser("workflow")
    flow.add_argument("operation", choices=["validate", "run"])
    flow.add_argument("file")
    flow.add_argument("--apply", action="store_true")
    flow.add_argument("--dry-run", action="store_true")
    sched = groups.add_parser("schedule").add_subparsers(dest="operation", required=True)
    add = sched.add_parser("add")
    add.add_argument("name")
    add.add_argument("--file", required=True)
    add.add_argument("--calendar", required=True)
    add.add_argument("--persistent", action="store_true")
    add.add_argument("--apply", action="store_true")
    sched.add_parser("list")
    for op in ["status", "enable", "disable", "remove", "run", "history"]:
        sp = sched.add_parser(op)
        sp.add_argument("name")
    return p.parse_args()


def main(a):
    if a.group == "memory":
        return memory_status() if a.operation == "status" else memory_reclaim(a.cgroup, a.amount, a.dry_run)
    if a.group == "cleanup":
        return cleanup(a.provider, a.days, a.keep, a.path, a.operation == "run" and a.apply and not a.dry_run)
    if a.group == "workflow":
        data = validate_workflow(trusted_json(a.file))
        return "Workflow valid" if a.operation == "validate" else workflow_run(data, not a.apply or a.dry_run)
    if a.operation == "add":
        return schedule_install(a)
    if a.operation == "list":
        return run([native("systemctl"), "list-timers", "--all", "kernoryx-*.timer", "--no-pager"])
    unit = unit_name(a.name)
    if a.operation == "history":
        return run([native("journalctl"), "-u", unit + ".service", "-n", "50", "--no-pager"])
    if a.operation == "status":
        return run([native("systemctl"), "show", unit + ".timer", unit + ".service", "--no-pager"])
    root_required()
    if a.operation == "run":
        return run([native("systemctl"), "start", unit + ".service"], timeout=610)
    if a.operation in {"enable", "disable"}:
        return run([native("systemctl"), a.operation, "--now", unit + ".timer"])
    run([native("systemctl"), "disable", "--now", unit + ".timer"])
    # Wait for active job to stop before removing its configuration.
    run([native("systemctl"), "stop", unit + ".service"])
    for f in [
        Path("/etc/systemd/system") / (unit + ".timer"),
        Path("/etc/systemd/system") / (unit + ".service"),
        Path("/etc/kernoryx/workflows") / (a.name + ".json"),
    ]:
        f.unlink(missing_ok=True)
    run([native("systemctl"), "daemon-reload"])
    return "Schedule removed"


@contextmanager
def maintenance_lock(schedule=False):
    lockbase = Path("/run/kernoryx-maintenance") if os.geteuid() == 0 else HOME / ".local/state/kernoryx"
    lockbase.mkdir(parents=True, exist_ok=True, mode=0o700)
    directory = secure_dir(lockbase)
    try:
        info = os.fstat(directory)
        if info.st_uid != os.geteuid() or info.st_mode & 0o022:
            raise ValueError("Untrusted lock directory")
        lock = os.open(
            "schedule-lock" if schedule else "lock",
            os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW,
            0o600,
            dir_fd=directory,
        )
    finally:
        os.close(directory)
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        yield
    finally:
        os.close(lock)


def mutates(args):
    if args.group == "memory":
        return args.operation == "reclaim" and not args.dry_run
    if args.group in {"cleanup", "workflow"}:
        return args.operation == "run" and args.apply and not args.dry_run
    return args.operation in {"enable", "disable", "remove", "run"} or (
        args.operation == "add" and args.apply
    )


if __name__ == "__main__":
    try:
        args = parse_args()
        if mutates(args):
            with maintenance_lock(args.group == "schedule"):
                print(main(args))
        else:
            print(main(args))
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        print(f"kernoryx: {error}", file=sys.stderr)
        sys.exit(1)
