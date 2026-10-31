# Kernoryx

Kernoryx is a Linux utility for temporary performance sessions, thermal monitoring,
fan modes, and routine maintenance. It is built first for a Lenovo XiaoXinPro 14
IRH8 (83AL) running CachyOS, with supported interfaces kept separate so other
hardware can be added without changing the session logic.

The useful part is coordinating changes: request a profile for a bounded time,
restore the previous state, inspect memory pressure, or run a maintenance workflow.
Kernoryx uses existing system tools where possible. It does not replace firmware
thermal protection or treat cached RAM as wasted memory.

## Getting started

You need a C++20 compiler, Make, CMake 3.20+, and Python 3.10+. On CachyOS/Arch:

```sh
sudo pacman -S --needed base-devel cmake python
make
make test
sudo make install
sudo systemctl daemon-reload
sudo systemctl enable --now kernoryxd
```

Build as your normal user. Installation only copies already-built files, so
`sudo make install` will not compile or leave root-owned build objects behind.
Use `make JOBS=4` for more build workers. `PREFIX` and `BUILD_DIR` can be set when
building; installation uses the prefix recorded by that build.

If upgrading from an older installation, check `systemctl cat kernoryxd` for an
old unit under `/etc/systemd/system` that overrides the newly installed unit.
Restart the daemon after updating an existing installation. To uninstall, stop
and disable `kernoryxd`, run `sudo make uninstall`, then reload systemd. Recovery
state and your workflow/timer files are deliberately preserved.

Keep your existing power-profile service running. Kernoryx delegates through
`powerprofilesctl` when available and refuses to compete with unsupported active
power managers. Package cleanup uses optional `pacman-contrib`; trash cleanup
uses `trash-cli` with dry-run support.

## A few examples

```sh
kernoryx doctor
sudo kernoryx mode set performance --for 20m
kernoryx thermal status
kernoryx memory status
kernoryx cleanup scan packages --keep 3 --days 30
```

Cleanup is a preview until explicitly applied. User cache/trash cleanup runs as
your normal user; system cleanup needs the corresponding privileges. The complete
command list and options are in `kernoryx --help` and each maintenance group's
`--help`. The build-tree CLI also supports maintenance without installation.

### Fan support on the XiaoXinPro

The IdeaPad driver exposes `fan_mode` outside hwmon. Kernoryx detects that device
and the 83AL model instead of requiring a ThinkPad sensor. It can request firmware
mode 4 (Efficient Thermal Dissipation), then restore mode 1 (Standard):

```sh
kernoryx fan status
kernoryx fan boost --for 30s --dry-run
sudo kernoryx fan boost --for 30s --experimental
sudo kernoryx fan restore
```

This is **experimental firmware mode selection**, not a fan percentage or verified
RPM boost. The file exists on this laptop, but its physical behavior has not yet
been tested here. Other IdeaPad models have reported inconsistent mode behavior.
Start with a short session at normal temperature and observe it locally. Mode 0
(Super Silent) and mode 2 (Dust Cleaning) are not exposed as actions.

IdeaPad sessions require Standard mode initially, default to 60 seconds, and are
limited to 10 minutes. There is no documented kernel watchdog for this interface:
restoration depends on the daemon running or restarting. `fan restore` returns an
owned session to Standard; `fan release` only abandons ownership. `fan auto` is
reserved for the separate ThinkPad backend and is not an alias for IdeaPad mode 1.
Other IdeaPad models are discoverable but not enabled for writes.

## How it works

The C++ CLI talks to a privileged daemon over a local Unix socket. The daemon
checks the caller's credentials and owns hardware sessions. Monotonic boot-time
deadlines include time spent suspended. Replacing a session replaces its deadline
while retaining the original restoration target.

Before changes, Kernoryx records recovery state and verifies writes by reading
back the interface. It restores only while the observed state still matches its
own change. For IdeaPad fans, this includes the platform profile: an external
fan/profile change ends tracking without overwriting it. Same-value or
away-and-back changes between polls cannot be detected. Recovery journals live
in `/var/lib/kernoryx`; interrupted sessions are cancelled on restart.

Thermal checks end fan sessions on lost CPU readings, CPU temperature at or above
85 C, or a reported critical sensor limit. These are additional application checks,
not a substitute for firmware safeguards. Supported ThinkPads use their separate
PWM interface and a kernel watchdog. Unsupported hardware retains monitoring.

Memory reporting uses MemAvailable and PSI stalls. Reclaim targets an explicitly
selected cgroup; there is no global cache-flush action. Cleanup delegates retention
to native tools or handles a named user-cache directory. JSON workflows accept
only defined action types, run in order, and stop on failure. Completed cleanup
cannot be rolled back. Calendar schedules use systemd timers; missed performance
sessions are not replayed at boot.

## Source layout

| Files | Responsibility |
|---|---|
| `system.*` | File I/O, durable state, durations, directory discovery, telemetry |
| `power.*` | Power backends, policy, and temporary sessions; formerly `sysfs.*` |
| `fan.*` | IdeaPad and ThinkPad driver operations |
| `health.*` | Thermal reporting, fan sessions, ownership, and recovery |
| `process.*` | Bounded subprocess execution without a shell |
| `main.cpp`, `daemon.cpp` | CLI routing and authenticated hardware command loop |
| `scripts/maintenance.py` | Memory, cleanup, workflows, and systemd scheduling |
| `tests/` | Shared fixtures and feature-focused checks |

CMake is the single build definition; the Makefile provides short entry points.
The code follows the existing four-space, opening-brace-on-the-same-line style,
with explicit blocks and one statement per line. `.clang-format` records that
layout.

The fixture and subprocess suites pass locally. Socket integration is included
but skipped in this execution environment because Unix socket creation is denied.
Real laptop fan behavior, desktop-service interaction, and live timers still need
target-machine verification; fixture readback does not prove a physical fan changed.

Driver references: [IdeaPad ABI](https://www.kernel.org/doc/Documentation/ABI/testing/sysfs-platform-ideapad-laptop),
[ThinkPad driver](https://www.kernel.org/doc/html/latest/admin-guide/laptops/thinkpad-acpi.html),
and [reported IdeaPad firmware inconsistency](https://lkml.iu.edu/hypermail/linux/kernel/2605.2/00031.html).
