# pm_recovery_debug

USB disconnect detection & recovery diagnostic tool for **Torizon Embedded
Linux** on Toradex **i.MX8M Plus** SOMs, built to debug **PenMount PM171x**
USB touch controller (`VID=0x14E1 PID=0x3508`) dropouts observed during
medical EMI (RS, 28V) testing.

It watches the USB bus via `libudev`, and when the target device disappears
it automatically tries a sequence of recovery methods, logging the result of
each one, so you can determine which recovery technique actually works for
a given EMI test scenario. It uses only native Linux kernel mechanisms
(`libudev`, `libusb-1.0`, `sysfs`, `ioctl`) -- there is no dependency on the
`uhubctl` binary; the hub port power-cycle method reimplements the same
`SET_PORT_FEATURE` / `CLEAR_PORT_FEATURE` control transfers uhubctl uses,
directly via `libusb_control_transfer`.

## What it does

1. **Monitors** `libudev` for `add`/`remove` events on the given VID:PID.
2. On **disconnect**:
   - Logs a timestamped event.
   - Saves a snapshot of the last N lines of the kernel log (via the
     `klogctl(2)` syscall directly -- no dependency on a `dmesg` binary
     being installed in the container; falls back to `dmesg` if present
     and the syscall path is unavailable).
   - Runs recovery methods in order, stopping at the first one that gets
     the device to re-enumerate:
     1. `reset` -- `USBDEVFS_RESET` ioctl on the device node
     2. `unbind` -- sysfs driver `unbind` + `bind`
     3. `authorized` -- sysfs `authorized` 0 -> 1 toggle
     4. `hub_power` -- USB hub per-port power cycle via `libusb` control
        transfers (equivalent to `uhubctl`, implemented directly)
     5. `gpio` -- reserved stub for carrier-board VBUS GPIO control via
        `libgpiod` (disabled by default, not yet implemented)
3. Every method's outcome (`SUCCESS`/`FAIL`/`SKIPPED`) and a final
   `RECOVERY_SUMMARY` line are written to a timestamped log file, so a full
   test run can be zipped up and sent back for analysis.

Because a genuine disconnect usually means the device's own sysfs node is
already gone by the time the tool reacts, methods 1-3 target the device's
*own* node and will legitimately report `SKIPPED` once it's fully removed.
Method 4 targets the *parent hub*, which normally survives the child's
disconnect, so it is expected to be the most broadly applicable method for
real EMI-induced dropouts -- that's the point of running all of them and
comparing.

## Building

### Where to build

Torizon containers are Debian-based. The simplest approach is to build
**on-target** (or in a container running under the target's architecture,
e.g. via `docker buildx --platform linux/arm64` or Torizon's own dev
container tooling), since this tool has no special cross-compilation
requirements beyond the two libraries below.

### Dependencies

```
apt-get update
apt-get install -y build-essential pkg-config libudev-dev libusb-1.0-0-dev
```

### Build

```
make
```

This produces a single `pm_recovery_debug` binary in the project root.

```
make clean   # remove build artifacts
```

## Running inside a Torizon container

This tool needs low-level access that a default (non-privileged) Torizon
application container does not have:

- Read/write access to `/dev/bus/usb/*` device nodes (opening device nodes
  for `USBDEVFS_RESET`, and opening the hub's node with `libusb` for the
  port power-cycle method).
- Write access to `sysfs` paths under `/sys/bus/usb/...` (driver
  unbind/bind, `authorized` toggle) -- these are normally exposed read-only
  in unprivileged containers.
- Permission to open a netlink socket for the udev event monitor.
- `CAP_SYSLOG` (or root) to read the kernel ring buffer for the dmesg
  snapshot.

### Recommended: `--privileged`

Since this is a **field diagnostic/debug tool**, not a production
component, the simplest and most robust option is to run it in a
privileged container:

```bash
docker run --rm -it \
  --privileged \
  -v /dev/bus/usb:/dev/bus/usb \
  -v /sys:/sys \
  -v "$(pwd)":/work -w /work \
  <your-torizon-base-image> \
  ./pm_recovery_debug --log-dir=/work/logs
```

`--privileged` grants all capabilities and disables the device cgroup
filter, and combined with the `/sys` and `/dev/bus/usb` bind mounts ensures
sysfs writes, `USBDEVFS_RESET`, and hub control transfers all work without
further tuning.

### Tighter alternative (no `--privileged`)

If your deployment can't use `--privileged`, the minimal equivalent is:

```bash
docker run --rm -it \
  --cap-add=SYS_ADMIN \
  --cap-add=SYSLOG \
  --device-cgroup-rule='c 189:* rmw' \
  -v /dev/bus/usb:/dev/bus/usb \
  -v /sys/bus/usb:/sys/bus/usb \
  -v /sys/devices:/sys/devices \
  -v "$(pwd)":/work -w /work \
  <your-torizon-base-image> \
  ./pm_recovery_debug --log-dir=/work/logs
```

Notes:
- `189:*` is the kernel-reserved major number for `/dev/bus/usb/*` nodes.
- `/sys/bus/usb` contains the `drivers/<driver>/{unbind,bind}` and
  `devices/<busid>/authorized` files the tool writes to; `/sys/devices`
  needs to be mounted read-write too since `/sys/bus/usb/devices/*` are
  symlinks into it.
- If the udev monitor fails to open its netlink socket in this mode, add
  `--cap-add=NET_ADMIN` as well.
- If running as a non-root user inside the container, all of the above
  still requires the container's *user* to have permission on those
  mounted paths (typically means running as root inside the container,
  which is normal for a --privileged-style debug container).

The tool always logs an explicit, non-silent error (with an `errno`-based
hint) whenever a permission problem blocks a method, rather than treating
it as an unexplained failure -- check `DETAIL=` in the log if a method
unexpectedly reports `FAIL`.

## Usage

```
Usage: ./pm_recovery_debug [options]

Default mode: run as a foreground daemon, monitor udev for the target
device's add/remove events, and automatically run all recovery methods
in order when it disconnects.

Options:
  --vid=<hex>          Target vendor ID, e.g. 0x14E1 (default 0x14E1)
  --pid=<hex>          Target product ID, e.g. 0x3508 (default 0x3508)
  --wait-seconds=<n>   Seconds to wait after each method before checking
                       for re-enumeration (default 3)
  --dmesg-lines=<n>    Lines of dmesg to snapshot on disconnect (default 200)
  --log-dir=<path>     Directory to write log files into (default '.')
  --enable-gpio        Enable the (stub) GPIO VBUS recovery method
  --method=<name>      Run a single method once against the currently
                       connected device and exit (see --list-methods)
  --list-methods       List available recovery methods and exit
  -h, --help           Show this help
```

### Daemon mode (default) -- watch and auto-recover

```bash
./pm_recovery_debug --log-dir=./logs
```

Runs in the foreground, printing and logging every event. Stop with
`Ctrl-C`.

### Manual single-method testing

Requires the device to currently be connected (so its topology can be
resolved):

```bash
./pm_recovery_debug --method=reset
./pm_recovery_debug --method=unbind
./pm_recovery_debug --method=authorized
./pm_recovery_debug --method=hub_power
```

Exit codes for `--method=...`: `0` success, `1` fail, `2` unknown method /
bad arguments, `3` device not currently connected, `4` method skipped.

### Reusing this tool for another VID:PID

```bash
./pm_recovery_debug --vid=0x1234 --pid=0x5678
```

### Adjusting the re-enumeration wait time

```bash
./pm_recovery_debug --wait-seconds=5
```

## Log format

One log file per run: `pm_recovery_<YYYYMMDD_HHMMSS>.log`, plus one
`dmesg_snapshot_<YYYYMMDD_HHMMSS>.log` file per disconnect event. Example:

```
[2026-07-14 09:12:03.114] EVENT: device disconnect detected (14e1:3508)
[2026-07-14 09:12:03.140] dmesg snapshot (last 200 lines, via klogctl(SYSLOG_ACTION_READ_ALL)) saved to ./dmesg_snapshot_20260714_091203.log
[2026-07-14 09:12:03.140] RECOVERY: starting recovery pass (up to 5 methods, wait_seconds=3 per method)
[2026-07-14 09:12:03.141] METHOD=reset RESULT=SKIPPED DETAIL=device node /dev/bus/usb/001/007 not present (device fully removed from bus; reset cannot target a node that no longer exists)
[2026-07-14 09:12:03.142] METHOD=unbind RESULT=SKIPPED DETAIL=sysfs driver symlink /sys/bus/usb/devices/1-1.3/driver not present (device likely fully removed already; nothing to unbind)
[2026-07-14 09:12:03.143] METHOD=authorized RESULT=SKIPPED DETAIL=sysfs path /sys/bus/usb/devices/1-1.3/authorized not present (device likely fully removed already)
[2026-07-14 09:12:06.645] METHOD=hub_power RESULT=SUCCESS DETAIL=hub 1-1 port 3 power cycle (mode=individual) succeeded; device reenumerated after 3.00s
[2026-07-14 09:12:06.645] RECOVERY_SUMMARY: first successful method = hub_power
```

Every disconnect event runs at most one pass through the methods (capped at
5 per event) and stops early on the first success, so a single EMI-induced
dropout never triggers an unbounded retry loop.

## Files

- `src/common.h` -- shared types (device topology snapshot, config, logging
  and recovery-method interfaces)
- `src/logutil.c` -- timestamped log file + dmesg ring-buffer snapshot
- `src/usb_monitor.c` -- `libudev` add/remove monitoring, device topology
  caching (busid, devnode, parent hub, port number), re-enumeration polling
- `src/recovery.c` -- the five recovery methods and the run-all dispatcher
- `src/main.c` -- CLI argument parsing and entry point
- `Makefile` -- `pkg-config`-based build (`libudev`, `libusb-1.0`)

## Known limitations / things to check on-site

- If the device connected *before* this tool started, its topology is
  discovered via an initial enumeration at start-up; if the tool itself is
  started *after* a disconnect has already happened, there is nothing to
  recover from and it simply waits for the next connect/disconnect cycle.
- `hub_power` reports `SKIPPED` (not an error) if the parent hub rejects
  the `CLEAR_FEATURE(PORT_POWER)` request -- this is expected for hubs/root
  hubs that don't implement per-port power switching (e.g. always-on root
  hubs, common on some SoC USB controllers). The log records the hub's
  reported power-switching mode (`ganged`/`individual`/`unknown`) either
  way.
- The `gpio` method is an intentional stub (`--enable-gpio` just documents
  intent; it does not perform any GPIO action yet). Once the carrier
  board's schematic confirms which GPIO drives VBUS for this specific USB
  port, that method can be implemented with `libgpiod` without changing any
  other part of the tool.
