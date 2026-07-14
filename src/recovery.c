/*
 * recovery.c - the five USB disconnect-recovery methods and the dispatcher
 * that runs them in order for one disconnect event.
 *
 * Methods 1-3 operate on the device's own sysfs/devfs nodes and therefore
 * only apply if those nodes are still present (a "soft" failure where the
 * kernel hasn't fully torn down the device object yet). Method 4 operates
 * on the *parent hub*, which normally survives a child disconnect, so it is
 * the most broadly applicable method for genuine EMI-induced dropouts.
 */
#define _GNU_SOURCE
#include "common.h"

#include <libusb-1.0/libusb.h>
#include <linux/usbdevice_fs.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

/* USB 2.0 spec (Table 11-17): port feature selector for port power,
 * used with (Clear|Set)PortFeature class requests (recipient = OTHER). */
#define USB_PORT_FEAT_POWER 8
#define USB_DT_HUB_TYPE      0x29

static void perm_hint(char *detail, size_t len, const char *action, int err)
{
    if (err == EACCES || err == EPERM) {
        snprintf(detail, len,
                 "%s: permission denied (errno=%d %s). This tool needs root "
                 "privileges / CAP_SYS_ADMIN and read-write access to the "
                 "relevant sysfs and /dev/bus/usb nodes -- see README for "
                 "required container flags.",
                 action, err, strerror(err));
    } else {
        snprintf(detail, len, "%s: %s (errno=%d)", action, strerror(err), err);
    }
}

static int write_small_file(const char *path, const char *value, char *detail, size_t detail_len)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        perm_hint(detail, detail_len, path, errno);
        return -1;
    }
    ssize_t n = write(fd, value, strlen(value));
    int saved_errno = errno;
    close(fd);
    if (n < 0) {
        perm_hint(detail, detail_len, path, saved_errno);
        return -1;
    }
    return 0;
}

static method_result_t finish_with_wait(const struct app_config *cfg, const char *action_desc,
                                         char *detail, size_t detail_len, double *elapsed_out)
{
    double elapsed = 0;
    int reappeared = wait_for_device_reenumeration(cfg->vid, cfg->pid, cfg->wait_seconds, &elapsed);
    if (elapsed_out)
        *elapsed_out = elapsed;

    if (reappeared) {
        snprintf(detail, detail_len, "%s; device reenumerated after %.2fs", action_desc, elapsed);
        return METHOD_RESULT_SUCCESS;
    }

    snprintf(detail, detail_len, "%s; device did NOT reenumerate within %.2fs", action_desc, elapsed);
    return METHOD_RESULT_FAIL;
}

/* ---- Method 1: USBDEVFS_RESET ------------------------------------------ */

static method_result_t method_reset(const struct app_config *cfg, const struct device_snapshot *snap,
                                     char *detail, size_t detail_len, double *elapsed_out)
{
    if (!snap->valid || snap->devnode[0] == '\0') {
        snprintf(detail, detail_len, "no cached device node available (topology unknown)");
        return METHOD_RESULT_SKIPPED;
    }

    struct stat st;
    if (stat(snap->devnode, &st) != 0) {
        snprintf(detail, detail_len,
                 "device node %s not present (device fully removed from bus; "
                 "reset cannot target a node that no longer exists)", snap->devnode);
        return METHOD_RESULT_SKIPPED;
    }

    int fd = open(snap->devnode, O_WRONLY);
    if (fd < 0) {
        perm_hint(detail, detail_len, snap->devnode, errno);
        return METHOD_RESULT_FAIL;
    }

    int rc = ioctl(fd, USBDEVFS_RESET, NULL);
    int saved_errno = errno;
    close(fd);

    if (rc != 0) {
        perm_hint(detail, detail_len, "USBDEVFS_RESET ioctl", saved_errno);
        return METHOD_RESULT_FAIL;
    }

    char action_desc[SYSFS_MAXLEN + 64];
    snprintf(action_desc, sizeof(action_desc), "USBDEVFS_RESET on %s succeeded", snap->devnode);
    return finish_with_wait(cfg, action_desc, detail, detail_len, elapsed_out);
}

/* ---- Method 2: sysfs driver unbind/rebind ------------------------------ */

static method_result_t method_unbind_rebind(const struct app_config *cfg, const struct device_snapshot *snap,
                                             char *detail, size_t detail_len, double *elapsed_out)
{
    if (!snap->valid || snap->busid[0] == '\0') {
        snprintf(detail, detail_len, "no cached busid available (topology unknown)");
        return METHOD_RESULT_SKIPPED;
    }

    char driver_link[SYSFS_MAXLEN + 64];
    snprintf(driver_link, sizeof(driver_link), "/sys/bus/usb/devices/%s/driver", snap->busid);

    char driver_path[SYSFS_MAXLEN];
    ssize_t len = readlink(driver_link, driver_path, sizeof(driver_path) - 1);
    if (len < 0) {
        snprintf(detail, detail_len,
                 "sysfs driver symlink %s not present (device likely fully "
                 "removed already; nothing to unbind)", driver_link);
        return METHOD_RESULT_SKIPPED;
    }
    driver_path[len] = '\0';

    const char *slash = strrchr(driver_path, '/');
    const char *driver_name = slash ? slash + 1 : driver_path;

    char unbind_path[SYSFS_MAXLEN + 64];
    char bind_path[SYSFS_MAXLEN + 64];
    snprintf(unbind_path, sizeof(unbind_path), "/sys/bus/usb/drivers/%s/unbind", driver_name);
    snprintf(bind_path, sizeof(bind_path), "/sys/bus/usb/drivers/%s/bind", driver_name);

    if (write_small_file(unbind_path, snap->busid, detail, detail_len) != 0)
        return METHOD_RESULT_FAIL;

    struct timespec pause = { .tv_sec = 0, .tv_nsec = 300000000L };
    nanosleep(&pause, NULL);

    if (write_small_file(bind_path, snap->busid, detail, detail_len) != 0)
        return METHOD_RESULT_FAIL;

    char action_desc[SYSFS_MAXLEN * 2 + 64];
    snprintf(action_desc, sizeof(action_desc), "unbind+bind via driver '%s' for %s succeeded",
             driver_name, snap->busid);
    return finish_with_wait(cfg, action_desc, detail, detail_len, elapsed_out);
}

/* ---- Method 3: sysfs authorized toggle --------------------------------- */

static method_result_t method_authorized_toggle(const struct app_config *cfg, const struct device_snapshot *snap,
                                                 char *detail, size_t detail_len, double *elapsed_out)
{
    if (!snap->valid || snap->busid[0] == '\0') {
        snprintf(detail, detail_len, "no cached busid available (topology unknown)");
        return METHOD_RESULT_SKIPPED;
    }

    char auth_path[SYSFS_MAXLEN + 64];
    snprintf(auth_path, sizeof(auth_path), "/sys/bus/usb/devices/%s/authorized", snap->busid);

    struct stat st;
    if (stat(auth_path, &st) != 0) {
        snprintf(detail, detail_len,
                 "sysfs path %s not present (device likely fully removed already)", auth_path);
        return METHOD_RESULT_SKIPPED;
    }

    if (write_small_file(auth_path, "0", detail, detail_len) != 0)
        return METHOD_RESULT_FAIL;

    sleep(1);

    if (write_small_file(auth_path, "1", detail, detail_len) != 0)
        return METHOD_RESULT_FAIL;

    char action_desc[SYSFS_MAXLEN + 64];
    snprintf(action_desc, sizeof(action_desc), "authorized 0->1 toggle on %s succeeded", snap->busid);
    return finish_with_wait(cfg, action_desc, detail, detail_len, elapsed_out);
}

/* ---- Method 4: hub port power cycle via libusb ------------------------- */

static libusb_device *find_libusb_device(libusb_device **list, int count, int busnum, int devnum)
{
    for (int i = 0; i < count; i++) {
        if (libusb_get_bus_number(list[i]) == busnum &&
            libusb_get_device_address(list[i]) == devnum)
            return list[i];
    }
    return NULL;
}

/* Best-effort read of wHubCharacteristics for diagnostic logging only; not
 * used to gate the attempt (we let the actual CLEAR_FEATURE call tell us
 * whether power switching is supported). */
static int read_hub_power_switching_mode(libusb_device_handle *handle, int *mode_out)
{
    unsigned char buf[71];
    int rc = libusb_control_transfer(handle,
                                      LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_DEVICE,
                                      LIBUSB_REQUEST_GET_DESCRIPTOR,
                                      (USB_DT_HUB_TYPE << 8), 0, buf, sizeof(buf), 1000);
    if (rc < 4)
        return -1;

    unsigned short characteristics = buf[3] | (buf[4] << 8);
    *mode_out = characteristics & 0x3;
    return 0;
}

static method_result_t method_hub_power_cycle(const struct app_config *cfg, const struct device_snapshot *snap,
                                               char *detail, size_t detail_len, double *elapsed_out)
{
    if (!snap->valid || !snap->has_parent_hub) {
        snprintf(detail, detail_len,
                 "no cached parent-hub info available (topology unknown or device "
                 "was directly on a controller with no addressable parent hub)");
        return METHOD_RESULT_SKIPPED;
    }

    if (snap->port_number <= 0) {
        snprintf(detail, detail_len,
                 "could not determine port number from busid '%s'", snap->busid);
        return METHOD_RESULT_SKIPPED;
    }

    libusb_context *ctx = NULL;
    int rc = libusb_init(&ctx);
    if (rc != 0) {
        snprintf(detail, detail_len, "libusb_init failed: %s", libusb_error_name(rc));
        return METHOD_RESULT_FAIL;
    }

    libusb_device **list = NULL;
    ssize_t count = libusb_get_device_list(ctx, &list);
    if (count < 0) {
        snprintf(detail, detail_len, "libusb_get_device_list failed: %s", libusb_error_name((int)count));
        libusb_exit(ctx);
        return METHOD_RESULT_FAIL;
    }

    libusb_device *hub_dev = find_libusb_device(list, (int)count, snap->hub_busnum, snap->hub_devnum);
    if (!hub_dev) {
        snprintf(detail, detail_len,
                 "parent hub bus=%d dev=%d (busid %s) not found via libusb; it may "
                 "also have dropped off the bus", snap->hub_busnum, snap->hub_devnum, snap->hub_busid);
        libusb_free_device_list(list, 1);
        libusb_exit(ctx);
        return METHOD_RESULT_FAIL;
    }

    libusb_device_handle *handle = NULL;
    rc = libusb_open(hub_dev, &handle);
    if (rc != 0) {
        if (rc == LIBUSB_ERROR_ACCESS) {
            snprintf(detail, detail_len,
                     "libusb_open on hub %s failed: permission denied. Need root / "
                     "device cgroup access to %s -- see README.", snap->hub_busid, snap->hub_devnode);
        } else {
            snprintf(detail, detail_len, "libusb_open on hub %s failed: %s",
                     snap->hub_busid, libusb_error_name(rc));
        }
        libusb_free_device_list(list, 1);
        libusb_exit(ctx);
        return METHOD_RESULT_FAIL;
    }

    int mode = -1;
    read_hub_power_switching_mode(handle, &mode);
    const char *mode_str = (mode == 0) ? "ganged" : (mode == 1) ? "individual" : "unknown";

    rc = libusb_control_transfer(handle,
                                  LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_OTHER,
                                  LIBUSB_REQUEST_CLEAR_FEATURE,
                                  USB_PORT_FEAT_POWER, snap->port_number, NULL, 0, 1000);
    if (rc != 0) {
        snprintf(detail, detail_len,
                 "hub %s (power-switching mode=%s) rejected CLEAR_FEATURE(PORT_POWER) "
                 "on port %d: %s -- hub likely does not support per-port power "
                 "switching (e.g. always-on root hub); not treated as a tool error",
                 snap->hub_busid, mode_str, snap->port_number, libusb_error_name(rc));
        libusb_close(handle);
        libusb_free_device_list(list, 1);
        libusb_exit(ctx);
        return METHOD_RESULT_SKIPPED;
    }

    struct timespec pause = { .tv_sec = 1, .tv_nsec = 500000000L };
    nanosleep(&pause, NULL);

    rc = libusb_control_transfer(handle,
                                  LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_OTHER,
                                  LIBUSB_REQUEST_SET_FEATURE,
                                  USB_PORT_FEAT_POWER, snap->port_number, NULL, 0, 1000);

    libusb_close(handle);
    libusb_free_device_list(list, 1);
    libusb_exit(ctx);

    if (rc != 0) {
        snprintf(detail, detail_len,
                 "CRITICAL: port %d power was cleared but re-enabling SET_FEATURE"
                 "(PORT_POWER) on hub %s failed: %s -- port may be left unpowered, "
                 "manual re-plug/power-cycle of the hub may be required",
                 snap->port_number, snap->hub_busid, libusb_error_name(rc));
        return METHOD_RESULT_FAIL;
    }

    char action_desc[SYSFS_MAXLEN + 64];
    snprintf(action_desc, sizeof(action_desc),
             "hub %s port %d power cycle (mode=%s) succeeded", snap->hub_busid, snap->port_number, mode_str);
    return finish_with_wait(cfg, action_desc, detail, detail_len, elapsed_out);
}

/* ---- Method 5: GPIO VBUS control (stub) -------------------------------- */

static method_result_t method_gpio_stub(const struct app_config *cfg, const struct device_snapshot *snap,
                                         char *detail, size_t detail_len, double *elapsed_out)
{
    (void)snap;
    (void)elapsed_out;

    if (!cfg->enable_gpio) {
        snprintf(detail, detail_len,
                 "GPIO VBUS control disabled by default (no --enable-gpio); interface "
                 "reserved for a future libgpiod-based implementation once the carrier "
                 "board's GPIO-to-USB-port VBUS mapping is confirmed");
        return METHOD_RESULT_SKIPPED;
    }

    snprintf(detail, detail_len,
             "--enable-gpio was set but this method is not yet implemented "
             "(stub only)");
    return METHOD_RESULT_SKIPPED;
}

/* ---- Dispatch table ----------------------------------------------------- */

static const struct recovery_method g_methods[] = {
    { "reset",      "USBDEVFS_RESET ioctl on the device node", method_reset },
    { "unbind",     "sysfs driver unbind + bind (forces re-probe)", method_unbind_rebind },
    { "authorized",  "sysfs 'authorized' 0/1 toggle (deauthorize/reauthorize)", method_authorized_toggle },
    { "hub_power",  "USB hub per-port power cycle via libusb control transfers (uhubctl-equivalent)", method_hub_power_cycle },
    { "gpio",       "(stub) carrier board VBUS GPIO control via libgpiod", method_gpio_stub },
};

const struct recovery_method *recovery_methods_table(int *count_out)
{
    if (count_out)
        *count_out = (int)(sizeof(g_methods) / sizeof(g_methods[0]));
    return g_methods;
}

const struct recovery_method *recovery_method_find(const char *name)
{
    int count;
    const struct recovery_method *methods = recovery_methods_table(&count);
    for (int i = 0; i < count; i++) {
        if (strcmp(methods[i].name, name) == 0)
            return &methods[i];
    }
    return NULL;
}

const char *recovery_run_all(const struct app_config *cfg, const struct device_snapshot *snap)
{
    int count;
    const struct recovery_method *methods = recovery_methods_table(&count);
    int cap = count < MAX_METHODS_PER_EVENT ? count : MAX_METHODS_PER_EVENT;

    log_line("RECOVERY: starting recovery pass (up to %d methods, wait_seconds=%d per method)",
              cap, cfg->wait_seconds);

    const char *first_success = NULL;

    for (int i = 0; i < cap; i++) {
        char detail[320];
        double elapsed = 0;
        method_result_t r = methods[i].fn(cfg, snap, detail, sizeof(detail), &elapsed);

        const char *rstr = (r == METHOD_RESULT_SUCCESS) ? "SUCCESS"
                           : (r == METHOD_RESULT_FAIL)   ? "FAIL"
                                                          : "SKIPPED";

        log_line("METHOD=%s RESULT=%s DETAIL=%s", methods[i].name, rstr, detail);

        if (r == METHOD_RESULT_SUCCESS) {
            first_success = methods[i].name;
            break;
        }
    }

    log_line("RECOVERY_SUMMARY: first successful method = %s", first_success ? first_success : "NONE");
    return first_success;
}
