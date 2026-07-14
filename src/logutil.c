/*
 * logutil.c - timestamped logging + dmesg ring-buffer snapshot capture.
 *
 * dmesg capture uses the klogctl(2) syscall directly (SYSLOG_ACTION_READ_ALL)
 * so the tool has no dependency on an external `dmesg` binary being present
 * in the Torizon container image. If that syscall is unavailable/denied
 * (e.g. missing CAP_SYSLOG), we fall back to popen("dmesg") when present.
 */
#define _GNU_SOURCE
#include "common.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <sys/klog.h>
#include <errno.h>
#include <unistd.h>

static FILE *g_log_fp = NULL;

void ts_now(char *buf, size_t len)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm_buf;
    localtime_r(&ts.tv_sec, &tm_buf);
    char base[64];
    strftime(base, sizeof(base), "%Y-%m-%d %H:%M:%S", &tm_buf);
    snprintf(buf, len, "%s.%03ld", base, ts.tv_nsec / 1000000);
}

static void ts_for_filename(char *buf, size_t len)
{
    time_t now = time(NULL);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    strftime(buf, len, "%Y%m%d_%H%M%S", &tm_buf);
}

int log_init(const char *log_dir, char *out_path, size_t out_path_len)
{
    char fname_ts[64];
    ts_for_filename(fname_ts, sizeof(fname_ts));

    char path[SYSFS_MAXLEN * 2];
    snprintf(path, sizeof(path), "%s/pm_recovery_%s.log", log_dir, fname_ts);

    g_log_fp = fopen(path, "a");
    if (!g_log_fp) {
        fprintf(stderr, "%s: failed to open log file %s: %s\n",
                APP_NAME, path, strerror(errno));
        return -1;
    }
    /* Line-buffered so `tail -f` on the customer site shows events live. */
    setvbuf(g_log_fp, NULL, _IOLBF, 0);

    if (out_path && out_path_len > 0) {
        strncpy(out_path, path, out_path_len - 1);
        out_path[out_path_len - 1] = '\0';
    }
    return 0;
}

void log_close(void)
{
    if (g_log_fp) {
        fclose(g_log_fp);
        g_log_fp = NULL;
    }
}

void log_line(const char *fmt, ...)
{
    char ts[64];
    ts_now(ts, sizeof(ts));

    va_list ap;

    /* Always echo to stdout for interactive/foreground debugging. */
    printf("[%s] ", ts);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);

    if (g_log_fp) {
        fprintf(g_log_fp, "[%s] ", ts);
        va_start(ap, fmt);
        vfprintf(g_log_fp, fmt, ap);
        va_end(ap);
        fprintf(g_log_fp, "\n");
        fflush(g_log_fp);
    }
}

/* Reads the kernel ring buffer via klogctl(SYSLOG_ACTION_READ_ALL). Returns
 * a malloc'd NUL-terminated buffer (caller frees), or NULL on failure. */
static char *read_kernel_ring_buffer(void)
{
    int len = klogctl(/*SYSLOG_ACTION_SIZE_BUFFER*/ 10, NULL, 0);
    if (len <= 0)
        len = 256 * 1024;

    char *buf = malloc((size_t)len + 1);
    if (!buf)
        return NULL;

    int n = klogctl(/*SYSLOG_ACTION_READ_ALL*/ 3, buf, len);
    if (n < 0) {
        free(buf);
        return NULL;
    }
    buf[n] = '\0';
    return buf;
}

/* Fallback: shell out to `dmesg` if present on the system/container. */
static char *read_dmesg_binary(void)
{
    FILE *p = popen("dmesg 2>/dev/null", "r");
    if (!p)
        return NULL;

    size_t cap = 256 * 1024, len = 0;
    char *buf = malloc(cap);
    if (!buf) {
        pclose(p);
        return NULL;
    }

    size_t n;
    while ((n = fread(buf + len, 1, cap - len, p)) > 0) {
        len += n;
        if (len == cap) {
            cap *= 2;
            char *nbuf = realloc(buf, cap);
            if (!nbuf) {
                free(buf);
                pclose(p);
                return NULL;
            }
            buf = nbuf;
        }
    }
    buf[len] = '\0';
    pclose(p);
    return buf;
}

/* Writes the last n_lines of `text` to fp. */
static void write_last_n_lines(FILE *fp, const char *text, int n_lines)
{
    size_t len = strlen(text);
    if (len == 0)
        return;

    /* Walk backwards counting newlines to find the start offset. */
    size_t i = len;
    int lines_seen = 0;
    /* Ignore a single trailing newline so we don't count a phantom blank
     * line at the end. */
    if (i > 0 && text[i - 1] == '\n')
        i--;

    size_t start = 0;
    while (i > 0) {
        if (text[i - 1] == '\n') {
            lines_seen++;
            if (lines_seen == n_lines) {
                start = i;
                break;
            }
        }
        i--;
    }

    fwrite(text + start, 1, len - start, fp);
    if (len > 0 && text[len - 1] != '\n')
        fputc('\n', fp);
}

int dmesg_snapshot_save(const char *log_dir, int n_lines, char *out_path, size_t out_path_len)
{
    char fname_ts[64];
    ts_for_filename(fname_ts, sizeof(fname_ts));

    char path[SYSFS_MAXLEN * 2];
    snprintf(path, sizeof(path), "%s/dmesg_snapshot_%s.log", log_dir, fname_ts);

    char *text = read_kernel_ring_buffer();
    const char *source = "klogctl(SYSLOG_ACTION_READ_ALL)";
    if (!text) {
        text = read_dmesg_binary();
        source = "dmesg(1) fallback";
    }

    if (!text) {
        log_line("ERROR: unable to capture dmesg snapshot (klogctl failed: %s; "
                  "dmesg binary also unavailable/failed). Need CAP_SYSLOG or "
                  "root, and/or kernel.dmesg_restrict=0.", strerror(errno));
        return -1;
    }

    FILE *fp = fopen(path, "w");
    if (!fp) {
        log_line("ERROR: failed to open dmesg snapshot file %s: %s", path, strerror(errno));
        free(text);
        return -1;
    }

    write_last_n_lines(fp, text, n_lines);
    fclose(fp);
    free(text);

    log_line("dmesg snapshot (last %d lines, via %s) saved to %s", n_lines, source, path);

    if (out_path && out_path_len > 0) {
        strncpy(out_path, path, out_path_len - 1);
        out_path[out_path_len - 1] = '\0';
    }
    return 0;
}
