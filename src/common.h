/*
 * common.h - shared types and constants for pm_recovery_debug
 */
#ifndef PM_RECOVERY_COMMON_H
#define PM_RECOVERY_COMMON_H

#include <stdio.h>
#include <stdint.h>

#define APP_NAME "pm_recovery_debug"

#define DEFAULT_VID 0x14E1
#define DEFAULT_PID 0x3508

#define DEFAULT_WAIT_SECONDS 3
#define DEFAULT_DMESG_LINES  200
#define MAX_METHODS_PER_EVENT 5

#define SYSFS_MAXLEN 256

/* Snapshot of a USB device's topology, captured while it is still present
 * on the bus. This is what lets the recovery methods operate on a device
 * that may have already fully disappeared from sysfs by the time we act. */
struct device_snapshot {
    int valid;

    unsigned short vid;
    unsigned short pid;

    char busid[SYSFS_MAXLEN];      /* e.g. "1-1.3" */
    int  busnum;
    int  devnum;
    char devnode[SYSFS_MAXLEN];    /* e.g. /dev/bus/usb/001/005 */

    /* Immediate parent hub, used for the hub port power-cycle method. */
    int  has_parent_hub;
    char hub_busid[SYSFS_MAXLEN];  /* e.g. "1-1" or "usb1" for a root hub */
    int  hub_busnum;
    int  hub_devnum;
    char hub_devnode[SYSFS_MAXLEN];
    int  port_number;              /* 1-based port number on hub_busid */
};

struct app_config {
    unsigned short vid;
    unsigned short pid;
    int wait_seconds;
    int dmesg_lines;
    char log_dir[SYSFS_MAXLEN];
    int enable_gpio;                /* stub method toggle, default off */
};

/* Logging */
int  log_init(const char *log_dir, char *out_path, size_t out_path_len);
void log_close(void);
void log_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void ts_now(char *buf, size_t len);

/* dmesg capture, native kernel syslog ring buffer (klogctl), falls back to
 * popen("dmesg") only if the syscall path is unavailable. */
int dmesg_snapshot_save(const char *log_dir, int n_lines, char *out_path, size_t out_path_len);

typedef enum {
    METHOD_RESULT_SUCCESS = 0,
    METHOD_RESULT_FAIL,
    METHOD_RESULT_SKIPPED
} method_result_t;

typedef method_result_t (*recovery_method_fn)(const struct app_config *cfg,
                                               const struct device_snapshot *snap,
                                               char *detail, size_t detail_len,
                                               double *elapsed_out);

struct recovery_method {
    const char *name;
    const char *description;
    recovery_method_fn fn;
};

const struct recovery_method *recovery_methods_table(int *count_out);
const struct recovery_method *recovery_method_find(const char *name);

/* Runs the full ordered recovery pass for one disconnect event. Stops early
 * on first success. Returns the name of the first successful method, or
 * NULL if none succeeded. */
const char *recovery_run_all(const struct app_config *cfg, const struct device_snapshot *snap);

/* Waits up to wait_seconds, polling for the device (vid:pid) to reappear on
 * the bus. Returns 1 if it reappeared, 0 otherwise. elapsed_out receives the
 * actual time spent waiting. */
int wait_for_device_reenumeration(unsigned short vid, unsigned short pid,
                                   int wait_seconds, double *elapsed_out);

/* USB monitor (usb_monitor.c) */
int usb_monitor_find_current(unsigned short vid, unsigned short pid,
                              struct device_snapshot *snap);

/* Runs the udev monitor loop forever, invoking recovery_run_all() on every
 * matching disconnect event. */
int usb_monitor_run(const struct app_config *cfg);

#endif /* PM_RECOVERY_COMMON_H */
