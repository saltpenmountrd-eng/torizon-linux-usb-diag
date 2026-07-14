/*
 * pm_recovery_debug - USB disconnect detection & recovery diagnostic tool
 *
 * Built for field debugging of PenMount PM171x (VID=0x14E1 PID=0x3508)
 * dropouts on Torizon / i.MX8M Plus under EMI (RS 28V) testing. Watches for
 * the device leaving the USB bus and tries a sequence of recovery methods
 * (USBDEVFS_RESET, sysfs unbind/rebind, sysfs authorized toggle, USB hub
 * per-port power cycle via libusb, and a GPIO VBUS stub), logging the
 * outcome of each so the customer's test log shows which method actually
 * works for their EMI scenario.
 */
#define _GNU_SOURCE
#include "common.h"

#include <getopt.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

static void print_usage(const char *argv0)
{
    printf("Usage: %s [options]\n"
           "\n"
           "Default mode: run as a foreground daemon, monitor udev for the target\n"
           "device's add/remove events, and automatically run all recovery methods\n"
           "in order when it disconnects.\n"
           "\n"
           "Options:\n"
           "  --vid=<hex>          Target vendor ID, e.g. 0x14E1 (default 0x%04X)\n"
           "  --pid=<hex>          Target product ID, e.g. 0x3508 (default 0x%04X)\n"
           "  --wait-seconds=<n>   Seconds to wait after each method before checking\n"
           "                       for re-enumeration (default %d)\n"
           "  --dmesg-lines=<n>    Lines of dmesg to snapshot on disconnect (default %d)\n"
           "  --log-dir=<path>     Directory to write log files into (default '.')\n"
           "  --enable-gpio        Enable the (stub) GPIO VBUS recovery method\n"
           "  --method=<name>      Run a single method once against the currently\n"
           "                       connected device and exit (see --list-methods)\n"
           "  --list-methods       List available recovery methods and exit\n"
           "  -h, --help           Show this help\n"
           "\n"
           "Examples:\n"
           "  %s\n"
           "  %s --method=hub_power --wait-seconds=5\n"
           "  %s --vid=0x14E1 --pid=0x3508 --log-dir=/data/logs\n",
           argv0, DEFAULT_VID, DEFAULT_PID, DEFAULT_WAIT_SECONDS, DEFAULT_DMESG_LINES,
           argv0, argv0, argv0);
}

static void print_method_list(void)
{
    int count;
    const struct recovery_method *methods = recovery_methods_table(&count);
    printf("Available recovery methods (run in this order in daemon mode):\n");
    for (int i = 0; i < count; i++) {
        printf("  %-12s %s\n", methods[i].name, methods[i].description);
    }
}

static int parse_id(const char *s, unsigned short *out)
{
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 0); /* accepts 0x14E1 or plain decimal */
    if (end == s || *end != '\0' || v > 0xFFFF)
        return -1;
    *out = (unsigned short)v;
    return 0;
}

static void check_root_and_warn(void)
{
    if (geteuid() != 0) {
        fprintf(stderr,
                "%s: WARNING: not running as root (euid=%d). Sysfs writes, "
                "USBDEVFS_RESET, and opening the hub's /dev/bus/usb node will "
                "very likely fail with EACCES/EPERM. Methods will report this "
                "explicitly rather than failing silently. See README for the "
                "container privileges this tool needs.\n",
                APP_NAME, geteuid());
    }
}

int main(int argc, char **argv)
{
    struct app_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.vid = DEFAULT_VID;
    cfg.pid = DEFAULT_PID;
    cfg.wait_seconds = DEFAULT_WAIT_SECONDS;
    cfg.dmesg_lines = DEFAULT_DMESG_LINES;
    strncpy(cfg.log_dir, ".", sizeof(cfg.log_dir) - 1);
    cfg.enable_gpio = 0;

    const char *method_name = NULL;
    int list_methods = 0;

    static struct option long_opts[] = {
        { "vid",          required_argument, NULL, 'v' },
        { "pid",          required_argument, NULL, 'p' },
        { "wait-seconds", required_argument, NULL, 'w' },
        { "dmesg-lines",  required_argument, NULL, 'n' },
        { "log-dir",      required_argument, NULL, 'l' },
        { "enable-gpio",  no_argument,       NULL, 'g' },
        { "method",       required_argument, NULL, 'm' },
        { "list-methods", no_argument,       NULL, 'L' },
        { "help",         no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'v':
            if (parse_id(optarg, &cfg.vid) != 0) {
                fprintf(stderr, "%s: invalid --vid value '%s'\n", APP_NAME, optarg);
                return 2;
            }
            break;
        case 'p':
            if (parse_id(optarg, &cfg.pid) != 0) {
                fprintf(stderr, "%s: invalid --pid value '%s'\n", APP_NAME, optarg);
                return 2;
            }
            break;
        case 'w':
            cfg.wait_seconds = atoi(optarg);
            if (cfg.wait_seconds <= 0) {
                fprintf(stderr, "%s: --wait-seconds must be positive\n", APP_NAME);
                return 2;
            }
            break;
        case 'n':
            cfg.dmesg_lines = atoi(optarg);
            if (cfg.dmesg_lines <= 0) {
                fprintf(stderr, "%s: --dmesg-lines must be positive\n", APP_NAME);
                return 2;
            }
            break;
        case 'l':
            strncpy(cfg.log_dir, optarg, sizeof(cfg.log_dir) - 1);
            break;
        case 'g':
            cfg.enable_gpio = 1;
            break;
        case 'm':
            method_name = optarg;
            break;
        case 'L':
            list_methods = 1;
            break;
        case 'h':
            print_usage(argv[0]);
            return 0;
        default:
            print_usage(argv[0]);
            return 2;
        }
    }

    if (list_methods) {
        print_method_list();
        return 0;
    }

    struct stat st;
    if (stat(cfg.log_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "%s: --log-dir '%s' does not exist or is not a directory\n",
                APP_NAME, cfg.log_dir);
        return 2;
    }

    check_root_and_warn();

    char log_path[SYSFS_MAXLEN * 2];
    if (log_init(cfg.log_dir, log_path, sizeof(log_path)) != 0)
        return 1;

    log_line("%s starting (vid=0x%04x pid=0x%04x wait_seconds=%d dmesg_lines=%d "
              "log_dir=%s enable_gpio=%d)",
              APP_NAME, cfg.vid, cfg.pid, cfg.wait_seconds, cfg.dmesg_lines,
              cfg.log_dir, cfg.enable_gpio);
    log_line("Logging to %s", log_path);

    int rc = 0;

    if (method_name) {
        const struct recovery_method *method = recovery_method_find(method_name);
        if (!method) {
            log_line("ERROR: unknown method '%s'. Use --list-methods to see valid names.",
                      method_name);
            log_close();
            return 2;
        }

        struct device_snapshot snap;
        memset(&snap, 0, sizeof(snap));

        if (!usb_monitor_find_current(cfg.vid, cfg.pid, &snap)) {
            log_line("ERROR: target device %04x:%04x is not currently connected; "
                      "manual single-method test requires the device to be present "
                      "so its topology (busid/devnode/parent hub) can be resolved.",
                      cfg.vid, cfg.pid);
            log_close();
            return 3;
        }

        log_line("Manual method test: found device busid=%s devnode=%s parent_hub=%s port=%d",
                  snap.busid, snap.devnode,
                  snap.has_parent_hub ? snap.hub_busid : "(none)", snap.port_number);

        char detail[320];
        double elapsed = 0;
        method_result_t r = method->fn(&cfg, &snap, detail, sizeof(detail), &elapsed);
        const char *rstr = (r == METHOD_RESULT_SUCCESS) ? "SUCCESS"
                           : (r == METHOD_RESULT_FAIL)   ? "FAIL"
                                                          : "SKIPPED";
        log_line("METHOD=%s RESULT=%s DETAIL=%s", method->name, rstr, detail);

        rc = (r == METHOD_RESULT_SUCCESS) ? 0 : (r == METHOD_RESULT_FAIL) ? 1 : 4;
    } else {
        rc = usb_monitor_run(&cfg);
    }

    log_close();
    return rc;
}
