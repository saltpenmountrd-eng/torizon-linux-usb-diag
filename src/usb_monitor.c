/*
 * usb_monitor.c - libudev-based USB add/remove monitoring and topology
 * snapshotting for pm_recovery_debug.
 *
 * We deliberately capture a struct device_snapshot for the target device
 * every time we see it appear (via udev "add" events, or an initial
 * enumeration at start-up). When a matching "remove" event later fires, the
 * device's own sysfs node is usually already gone by the time we can act on
 * it -- but its *parent hub* is normally still present, so the cached
 * topology (busid, port number, parent hub device node) is what lets the
 * hub-port power-cycle method work even though the device itself vanished.
 */
#define _GNU_SOURCE
#include "common.h"

#include <libudev.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/select.h>

/* Last known-good topology for the device we're tracking. Updated on every
 * matching "add" event and at start-up if the device is already present. */
static struct device_snapshot g_last_snapshot;

static int parse_hex_field(const char *s, unsigned long *out)
{
    if (!s || !*s)
        return -1;
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 16);
    if (end == s)
        return -1;
    *out = v;
    return 0;
}

/* Parses the kernel-generated uevent "PRODUCT=vvvv/pppp/bbbb" property,
 * present on both add and remove uevents for usb_device nodes (the kernel
 * fills it in from struct usb_device fields that are still valid at the
 * point the removal uevent is generated). */
static int parse_product_property(const char *product, unsigned short *vid, unsigned short *pid)
{
    if (!product)
        return -1;

    char buf[64];
    strncpy(buf, product, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *save = NULL;
    char *p_vid = strtok_r(buf, "/", &save);
    char *p_pid = strtok_r(NULL, "/", &save);
    if (!p_vid || !p_pid)
        return -1;

    unsigned long vv, pp;
    if (parse_hex_field(p_vid, &vv) != 0 || parse_hex_field(p_pid, &pp) != 0)
        return -1;

    *vid = (unsigned short)vv;
    *pid = (unsigned short)pp;
    return 0;
}

/* Derives the 1-based port number of `busid` on its immediate parent hub,
 * from the sysfs naming convention "<bus>-<port>[.<port>...]" e.g.
 * "1-1.3" -> port 3 on parent "1-1"; "1-2" -> port 2 on the root hub. */
static int derive_port_number(const char *busid, int *port_out)
{
    const char *last_dot = strrchr(busid, '.');
    const char *last_dash = strrchr(busid, '-');
    const char *sep = last_dot ? last_dot : last_dash;
    if (!sep || !*(sep + 1))
        return -1;

    char *end = NULL;
    long v = strtol(sep + 1, &end, 10);
    if (end == sep + 1 || v <= 0)
        return -1;

    *port_out = (int)v;
    return 0;
}

static unsigned long sysattr_hex(struct udev_device *dev, const char *attr)
{
    const char *v = udev_device_get_sysattr_value(dev, attr);
    unsigned long out = 0;
    if (v)
        parse_hex_field(v, &out);
    return out;
}

static unsigned long sysattr_dec(struct udev_device *dev, const char *attr)
{
    const char *v = udev_device_get_sysattr_value(dev, attr);
    if (!v)
        return 0;
    char *end = NULL;
    return strtoul(v, &end, 10);
}

/* Populates `snap` from a live udev_device (subsystem usb, devtype
 * usb_device). Only valid to call while the device is still enumerated,
 * since it reads live sysfs attributes. */
static void fill_snapshot_from_device(struct udev_device *dev, struct device_snapshot *snap)
{
    memset(snap, 0, sizeof(*snap));

    snap->vid = (unsigned short)sysattr_hex(dev, "idVendor");
    snap->pid = (unsigned short)sysattr_hex(dev, "idProduct");

    const char *sysname = udev_device_get_sysname(dev);
    if (sysname)
        strncpy(snap->busid, sysname, sizeof(snap->busid) - 1);

    snap->busnum = (int)sysattr_dec(dev, "busnum");
    snap->devnum = (int)sysattr_dec(dev, "devnum");

    const char *devnode = udev_device_get_devnode(dev);
    if (devnode)
        strncpy(snap->devnode, devnode, sizeof(snap->devnode) - 1);

    struct udev_device *parent =
        udev_device_get_parent_with_subsystem_devtype(dev, "usb", "usb_device");
    if (parent) {
        snap->has_parent_hub = 1;
        const char *psys = udev_device_get_sysname(parent);
        if (psys)
            strncpy(snap->hub_busid, psys, sizeof(snap->hub_busid) - 1);
        snap->hub_busnum = (int)sysattr_dec(parent, "busnum");
        snap->hub_devnum = (int)sysattr_dec(parent, "devnum");
        const char *pdevnode = udev_device_get_devnode(parent);
        if (pdevnode)
            strncpy(snap->hub_devnode, pdevnode, sizeof(snap->hub_devnode) - 1);
        /* Do not unref `parent`: owned by `dev`, freed with it. */
    }

    if (derive_port_number(snap->busid, &snap->port_number) != 0)
        snap->port_number = -1;

    snap->valid = 1;
}

int usb_monitor_find_current(unsigned short vid, unsigned short pid, struct device_snapshot *snap)
{
    struct udev *udev = udev_new();
    if (!udev)
        return 0;

    struct udev_enumerate *en = udev_enumerate_new(udev);
    udev_enumerate_add_match_subsystem(en, "usb");
    udev_enumerate_add_match_property(en, "DEVTYPE", "usb_device");
    udev_enumerate_scan_devices(en);

    struct udev_list_entry *entry;
    int found = 0;

    udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(en)) {
        const char *path = udev_list_entry_get_name(entry);
        struct udev_device *dev = udev_device_new_from_syspath(udev, path);
        if (!dev)
            continue;

        unsigned long dvid = sysattr_hex(dev, "idVendor");
        unsigned long dpid = sysattr_hex(dev, "idProduct");

        if (dvid == vid && dpid == pid) {
            fill_snapshot_from_device(dev, snap);
            found = 1;
        }

        udev_device_unref(dev);
        if (found)
            break;
    }

    udev_enumerate_unref(en);
    udev_unref(udev);
    return found;
}

int wait_for_device_reenumeration(unsigned short vid, unsigned short pid,
                                   int wait_seconds, double *elapsed_out)
{
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);

    struct device_snapshot tmp;
    const long step_ms = 200;
    long budget_ms = (long)wait_seconds * 1000;
    if (budget_ms <= 0)
        budget_ms = step_ms;

    int found = 0;
    long waited_ms = 0;
    while (waited_ms <= budget_ms) {
        if (usb_monitor_find_current(vid, pid, &tmp)) {
            found = 1;
            break;
        }
        struct timespec ts = { .tv_sec = step_ms / 1000, .tv_nsec = (step_ms % 1000) * 1000000L };
        nanosleep(&ts, NULL);
        waited_ms += step_ms;
    }

    clock_gettime(CLOCK_MONOTONIC, &now);
    double elapsed = (now.tv_sec - start.tv_sec) + (now.tv_nsec - start.tv_nsec) / 1e9;
    if (elapsed_out)
        *elapsed_out = elapsed;
    return found;
}

static void handle_add_event(struct udev_device *dev, const struct app_config *cfg)
{
    unsigned long vid = sysattr_hex(dev, "idVendor");
    unsigned long pid = sysattr_hex(dev, "idProduct");

    if (vid != cfg->vid || pid != cfg->pid)
        return;

    fill_snapshot_from_device(dev, &g_last_snapshot);
    log_line("EVENT: device connected/enumerated (%04lx:%04lx) busid=%s devnode=%s "
              "parent_hub=%s port=%d",
              vid, pid, g_last_snapshot.busid, g_last_snapshot.devnode,
              g_last_snapshot.has_parent_hub ? g_last_snapshot.hub_busid : "(none)",
              g_last_snapshot.port_number);
}

static void handle_remove_event(struct udev_device *dev, const struct app_config *cfg)
{
    const char *product = udev_device_get_property_value(dev, "PRODUCT");
    unsigned short vid = 0, pid = 0;
    int matched = 0;

    if (parse_product_property(product, &vid, &pid) == 0) {
        matched = (vid == cfg->vid && pid == cfg->pid);
    } else {
        /* Fallback: PRODUCT wasn't set on this uevent (seen on some kernel
         * versions/backends). Match by the last known busid instead. */
        const char *sysname = udev_device_get_sysname(dev);
        if (sysname && g_last_snapshot.valid && strcmp(sysname, g_last_snapshot.busid) == 0) {
            matched = 1;
            vid = cfg->vid;
            pid = cfg->pid;
        }
    }

    if (!matched)
        return;

    char ts[64];
    ts_now(ts, sizeof(ts));
    log_line("EVENT: device disconnect detected (%04x:%04x)", cfg->vid, cfg->pid);

    char dmesg_path[SYSFS_MAXLEN * 2];
    dmesg_snapshot_save(cfg->log_dir, cfg->dmesg_lines, dmesg_path, sizeof(dmesg_path));

    if (!g_last_snapshot.valid) {
        log_line("WARNING: no cached topology info available for this device "
                 "(it may have connected before this tool started). "
                 "Sysfs/devnode-based methods will be SKIPPED; hub_power "
                 "cannot be resolved either.");
    }

    recovery_run_all(cfg, &g_last_snapshot);

    /* The device slot is now "used up" until we see it reconnect; invalidate
     * so a second remove event without an intervening add doesn't reuse a
     * stale devnode/busid. */
    g_last_snapshot.valid = 0;
}

int usb_monitor_run(const struct app_config *cfg)
{
    struct udev *udev = udev_new();
    if (!udev) {
        fprintf(stderr, "%s: udev_new() failed\n", APP_NAME);
        return -1;
    }

    memset(&g_last_snapshot, 0, sizeof(g_last_snapshot));
    if (usb_monitor_find_current(cfg->vid, cfg->pid, &g_last_snapshot)) {
        log_line("Target device %04x:%04x already present at start-up: busid=%s "
                  "devnode=%s parent_hub=%s port=%d",
                  cfg->vid, cfg->pid, g_last_snapshot.busid, g_last_snapshot.devnode,
                  g_last_snapshot.has_parent_hub ? g_last_snapshot.hub_busid : "(none)",
                  g_last_snapshot.port_number);
    } else {
        log_line("Target device %04x:%04x not present at start-up; waiting for it to "
                  "connect before topology can be cached.", cfg->vid, cfg->pid);
    }

    struct udev_monitor *mon = udev_monitor_new_from_netlink(udev, "udev");
    if (!mon) {
        fprintf(stderr, "%s: udev_monitor_new_from_netlink() failed. This usually "
                        "means the process cannot open a netlink socket (permission or "
                        "container network namespace) or /run/udev is not accessible.\n",
                        APP_NAME);
        udev_unref(udev);
        return -1;
    }

    udev_monitor_filter_add_match_subsystem_devtype(mon, "usb", "usb_device");
    udev_monitor_enable_receiving(mon);

    int fd = udev_monitor_get_fd(mon);

    log_line("Monitoring udev for VID=0x%04x PID=0x%04x add/remove events "
              "(wait_seconds=%d, dmesg_lines=%d)...",
              cfg->vid, cfg->pid, cfg->wait_seconds, cfg->dmesg_lines);

    for (;;) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);

        int ret = select(fd + 1, &fds, NULL, NULL, NULL);
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            log_line("ERROR: select() on udev monitor fd failed: %s", strerror(errno));
            break;
        }

        if (!FD_ISSET(fd, &fds))
            continue;

        struct udev_device *dev = udev_monitor_receive_device(mon);
        if (!dev)
            continue;

        const char *action = udev_device_get_action(dev);
        if (action) {
            if (strcmp(action, "add") == 0)
                handle_add_event(dev, cfg);
            else if (strcmp(action, "remove") == 0)
                handle_remove_event(dev, cfg);
        }

        udev_device_unref(dev);
    }

    udev_monitor_unref(mon);
    udev_unref(udev);
    return 0;
}
