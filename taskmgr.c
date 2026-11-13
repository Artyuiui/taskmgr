/*
 * taskmgr - a tiny Windows-Task-Manager-like TUI for Linux terminals.
 *
 * Reads /proc directly, no threads, no heap churn between frames.
 * Typical footprint: < 2 MB RSS, ~0% CPU between refreshes.
 *
 * Build:  make            (needs ncurses dev headers)
 * Run:    ./taskmgr
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <langinfo.h>
#include <locale.h>
#include <ncurses.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <sys/socket.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#ifndef TASKMGR_VERSION
#define TASKMGR_VERSION "dev"
#endif

#define MAX_CORES 512
#define HIST 512
#define NAME_LEN 32
#define UID_CACHE 64

typedef struct {
    pid_t pid, ppid;
    uid_t uid;
    char name[NAME_LEN];
    char state;
    int threads;
    unsigned long long ticks;  /* utime + stime, USER_HZ */
    unsigned long rss_kb;
    double cpu;                /* % of whole machine, like Windows */
} proc_t;

typedef struct { unsigned long long busy, total; } cputime_t;

#define MAX_GPU 4
#define MAX_FS 12
typedef struct {
    char name[64];
    int kind;                      /* 1 = NVML (NVIDIA), 2 = sysfs (amdgpu / i915 / nouveau) */
    char path[320], temp_path[400];
    void *dev;                     /* NVML device handle */
    int has_util, has_mem, has_temp;
    double util;
    unsigned long long mem_used, mem_total;
    int temp;
    char pci[16], driver[24], drv_version[32];
} gpu_t;
typedef struct { char mnt[40]; char type[12]; unsigned long long used, total; } fs_t;

enum { TAB_PROC = 0, TAB_PERF = 1, TAB_SPEC = 2, TAB_COUNT };
enum { SORT_CPU, SORT_MEM, SORT_NAME, SORT_PID, SORT_USER };
enum { C_TITLE = 1, C_TABSEL, C_COLHDR, C_SEL, C_OK, C_WARN, C_BAD, C_DIM, C_ACCENT };

static struct {
    const char *proc_root, *sys_root;
    int ascii;

    proc_t *cur, *prev;
    int ncur, nprev, cap_cur, cap_prev;
    int *view; int nview, cap_view;   /* indices into cur after filter+sort */

    int ncpu;
    cputime_t cpu_prev[MAX_CORES + 1], cpu_now[MAX_CORES + 1];  /* [0] = all */
    double cpu_pct[MAX_CORES + 1];
    unsigned long mem_total, mem_avail, mem_cached, swap_total, swap_free;  /* kB */
    unsigned long long rx, tx, rx_prev, tx_prev, rd, wr, rd_prev, wr_prev;
    double rx_rate, tx_rate, rd_rate, wr_rate;   /* bytes/s */
    double uptime, load[3];
    int running, total_threads;
    double hist_cpu[HIST], hist_mem[HIST]; int hist_pos, hist_n;
    struct timespec last;
    long pagesize;

    gpu_t gpus[MAX_GPU]; int ngpu; void *nvml;
    fs_t fs[MAX_FS]; int nfs;

    /* UI state */
    int tab, sort, reverse;
    int sel_idx, scroll;
    int perf_scroll, perf_height, spec_scroll, spec_height;
    const char *etc_root;
    pid_t sel_pid;
    char filter[64]; int filter_mode;
    int interval_ms;
    char msg[160]; time_t msg_until;

    struct { uid_t uid; char name[16]; } uids[UID_CACHE]; int nuids;
} S;

/* ------------------------------------------------------------------ util */

static int read_file(const char *path, char *buf, size_t n) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t r = read(fd, buf, n - 1);
    close(fd);
    if (r < 0) return -1;
    buf[r] = 0;
    return (int)r;
}

static void set_msg(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(S.msg, sizeof S.msg, fmt, ap);
    va_end(ap);
    S.msg_until = time(NULL) + 3;
}

static const char *uid_name(uid_t uid) {
    for (int i = 0; i < S.nuids; i++) if (S.uids[i].uid == uid) return S.uids[i].name;
    struct passwd *pw = getpwuid(uid);
    char tmp[16];
    if (pw) snprintf(tmp, sizeof tmp, "%s", pw->pw_name);
    else snprintf(tmp, sizeof tmp, "%u", (unsigned)uid);
    if (S.nuids < UID_CACHE) {
        S.uids[S.nuids].uid = uid;
        snprintf(S.uids[S.nuids].name, 16, "%s", tmp);
        return S.uids[S.nuids++].name;
    }
    static char fallback[16];
    snprintf(fallback, sizeof fallback, "%s", tmp);
    return fallback;
}

static void fmt_bytes(double b, char *out, size_t n) {
    const char *u[] = {"B", "KB", "MB", "GB", "TB"};
    int i = 0;
    while (b >= 1000.0 && i < 4) { b /= 1024.0; i++; }
    if (i == 0) snprintf(out, n, "%.0f %s", b, u[i]);
    else snprintf(out, n, "%.1f %s", b, u[i]);
}

static void fmt_rate(double bps, char *out, size_t n) {
    char t[32]; fmt_bytes(bps, t, sizeof t);
    snprintf(out, n, "%s/s", t);
}

static void fmt_uptime(double s, char *out, size_t n) {
    long t = (long)s, d = t / 86400, h = (t / 3600) % 24, m = (t / 60) % 60;
    if (d > 0) snprintf(out, n, "%ldd %02ld:%02ld", d, h, m);
    else snprintf(out, n, "%02ld:%02ld:%02ld", h, m, t % 60);
}

static const char *state_name(char st) {
    switch (st) {
    case 'R': return "Running";
    case 'S': return "Sleeping";
    case 'D': return "Disk wait";
    case 'Z': return "Zombie";
    case 'T': case 't': return "Stopped";
    case 'I': return "Idle";
    case 'X': case 'x': return "Dead";
    default: return "?";
    }
}

static double now_sec(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ------------------------------------------------------------- sampling */

static int read_proc(pid_t pid, proc_t *p) {
    char path[320], buf[1024];
    snprintf(path, sizeof path, "%s/%d/stat", S.proc_root, pid);
    if (read_file(path, buf, sizeof buf) < 0) return -1;
    char *l = strchr(buf, '('), *r = strrchr(buf, ')');
    if (!l || !r || r < l) return -1;
    size_t n = (size_t)(r - l - 1);
    if (n >= NAME_LEN) n = NAME_LEN - 1;
    memcpy(p->name, l + 1, n); p->name[n] = 0;

    unsigned long long ut, st; long rss; int threads;
    /* after ")": state ppid pgrp session tty tpgid flags minflt cminflt majflt
       cmajflt utime stime cutime cstime priority nice num_threads itrealvalue
       starttime vsize rss */
    if (sscanf(r + 2,
               "%c %d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu "
               "%*d %*d %*d %*d %d %*d %*u %*u %ld",
               &p->state, &p->ppid, &ut, &st, &threads, &rss) != 6)
        return -1;
    p->pid = pid;
    p->ticks = ut + st;
    p->threads = threads;
    p->rss_kb = (unsigned long)(rss > 0 ? rss : 0) * (unsigned long)(S.pagesize / 1024);
    struct stat sb;
    snprintf(path, sizeof path, "%s/%d", S.proc_root, pid);
    p->uid = stat(path, &sb) == 0 ? sb.st_uid : 0;
    p->cpu = 0;
    return 0;
}

static int cmp_pid(const void *a, const void *b) {
    pid_t x = ((const proc_t *)a)->pid, y = ((const proc_t *)b)->pid;
    return (x > y) - (x < y);
}

static const proc_t *find_prev(pid_t pid) {
    int lo = 0, hi = S.nprev - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (S.prev[mid].pid == pid) return &S.prev[mid];
        if (S.prev[mid].pid < pid) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

static void read_cpu(void) {
    char path[320], line[1024];
    snprintf(path, sizeof path, "%s/stat", S.proc_root);
    FILE *f = fopen(path, "r");
    if (!f) return;
    int n = 0;
    S.running = 0;
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "cpu", 3)) {
            char tag[16]; unsigned long long v[8] = {0};
            int k = sscanf(line, "%15s %llu %llu %llu %llu %llu %llu %llu %llu",
                           tag, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
            if (k < 5) continue;
            unsigned long long total = 0;
            for (int i = 0; i < 8; i++) total += v[i];
            unsigned long long idle = v[3] + v[4];
            int idx = tag[3] == 0 ? 0 : atoi(tag + 3) + 1;
            if (idx > MAX_CORES) continue;
            S.cpu_now[idx].busy = total - idle;
            S.cpu_now[idx].total = total;
            if (idx > n) n = idx;
        } else if (!strncmp(line, "procs_running", 13)) {
            S.running = atoi(line + 13);
        }
    }
    fclose(f);
    S.ncpu = n;
    for (int i = 0; i <= n; i++) {
        if (S.cpu_now[i].total < S.cpu_prev[i].total || S.cpu_now[i].busy < S.cpu_prev[i].busy) continue; /* counter reset */
        unsigned long long dt = S.cpu_now[i].total - S.cpu_prev[i].total;
        unsigned long long db = S.cpu_now[i].busy - S.cpu_prev[i].busy;
        if (dt) S.cpu_pct[i] = 100.0 * (double)db / (double)dt;   /* keep last value if no ticks passed */
        if (S.cpu_pct[i] > 100) S.cpu_pct[i] = 100;
    }
}

static void read_mem(void) {
    char path[320], line[256];
    snprintf(path, sizeof path, "%s/meminfo", S.proc_root);
    FILE *f = fopen(path, "r");
    if (!f) return;
    unsigned long free_kb = 0, buffers = 0, cached = 0, sre = 0, avail = 0;
    while (fgets(line, sizeof line, f)) {
        char k[64]; unsigned long v;
        if (sscanf(line, "%63s %lu", k, &v) != 2) continue;
        if (!strcmp(k, "MemTotal:")) S.mem_total = v;
        else if (!strcmp(k, "MemFree:")) free_kb = v;
        else if (!strcmp(k, "MemAvailable:")) avail = v;
        else if (!strcmp(k, "Buffers:")) buffers = v;
        else if (!strcmp(k, "Cached:")) cached = v;
        else if (!strcmp(k, "SReclaimable:")) sre = v;
        else if (!strcmp(k, "SwapTotal:")) S.swap_total = v;
        else if (!strcmp(k, "SwapFree:")) S.swap_free = v;
    }
    fclose(f);
    S.mem_cached = buffers + cached + sre;
    S.mem_avail = avail ? avail : free_kb + S.mem_cached;
}

static void read_net(void) {
    char path[320], line[512];
    snprintf(path, sizeof path, "%s/net/dev", S.proc_root);
    FILE *f = fopen(path, "r");
    if (!f) return;
    unsigned long long rx = 0, tx = 0;
    int ln = 0;
    while (fgets(line, sizeof line, f)) {
        if (ln++ < 2) continue;
        char *c = strchr(line, ':');
        if (!c) continue;
        *c = 0;
        char *name = line; while (*name == ' ') name++;
        if (!strcmp(name, "lo")) continue;
        unsigned long long r, t;
        if (sscanf(c + 1, "%llu %*u %*u %*u %*u %*u %*u %*u %llu", &r, &t) == 2) { rx += r; tx += t; }
    }
    fclose(f);
    S.rx = rx; S.tx = tx;
}

static void read_disk(void) {
    char path[320], buf[256];
    snprintf(path, sizeof path, "%s/block", S.sys_root);
    DIR *d = opendir(path);
    if (!d) return;
    unsigned long long rd = 0, wr = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' || !strncmp(e->d_name, "loop", 4) || !strncmp(e->d_name, "ram", 3)) continue;
        snprintf(path, sizeof path, "%s/block/%s/stat", S.sys_root, e->d_name);
        if (read_file(path, buf, sizeof buf) <= 0) continue;
        unsigned long long sr, sw;
        if (sscanf(buf, "%*u %*u %llu %*u %*u %*u %llu", &sr, &sw) == 2) { rd += sr * 512; wr += sw * 512; }
    }
    closedir(d);
    S.rd = rd; S.wr = wr;
}

static void read_misc(void) {
    char path[320], buf[128];
    snprintf(path, sizeof path, "%s/uptime", S.proc_root);
    if (read_file(path, buf, sizeof buf) > 0) S.uptime = atof(buf);
    snprintf(path, sizeof path, "%s/loadavg", S.proc_root);
    if (read_file(path, buf, sizeof buf) > 0) sscanf(buf, "%lf %lf %lf", &S.load[0], &S.load[1], &S.load[2]);
}

static void read_procs(void) {
    DIR *d = opendir(S.proc_root);
    if (!d) return;
    S.ncur = 0;
    S.total_threads = 0;
    unsigned long long dcpu = S.cpu_now[0].total - S.cpu_prev[0].total;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!isdigit((unsigned char)e->d_name[0])) continue;
        if (S.ncur == S.cap_cur) {
            S.cap_cur = S.cap_cur ? S.cap_cur * 2 : 256;
            S.cur = realloc(S.cur, (size_t)S.cap_cur * sizeof *S.cur);
            if (!S.cur) { endwin(); perror("realloc"); exit(1); }
        }
        proc_t *p = &S.cur[S.ncur];
        if (read_proc((pid_t)atoi(e->d_name), p) < 0) continue;
        const proc_t *q = find_prev(p->pid);
        if (q && dcpu && p->ticks >= q->ticks)
            p->cpu = 100.0 * (double)(p->ticks - q->ticks) / (double)dcpu;
        S.total_threads += p->threads;
        S.ncur++;
    }
    closedir(d);
}

/* ---- GPU: NVIDIA via NVML (dlopen, optional), others via sysfs ---- */

typedef struct { unsigned int gpu, memory; } nvml_util_t;
typedef struct { unsigned long long total, free, used; } nvml_mem_t;
static int (*p_nvmlInit)(void);
static int (*p_nvmlCount)(unsigned *);
static int (*p_nvmlHandle)(unsigned, void **);
static int (*p_nvmlName)(void *, char *, unsigned);
static int (*p_nvmlUtil)(void *, nvml_util_t *);
static int (*p_nvmlMem)(void *, nvml_mem_t *);
static int (*p_nvmlTemp)(void *, int, unsigned *);
static int (*p_nvmlDrv)(char *, unsigned);

static int nvml_load(void) {
    if (getenv("TASKMGR_NO_NVML")) return 0;
    S.nvml = dlopen("libnvidia-ml.so.1", RTLD_NOW);
    if (!S.nvml) return 0;
    *(void **)&p_nvmlInit   = dlsym(S.nvml, "nvmlInit_v2");
    *(void **)&p_nvmlCount  = dlsym(S.nvml, "nvmlDeviceGetCount_v2");
    *(void **)&p_nvmlHandle = dlsym(S.nvml, "nvmlDeviceGetHandleByIndex_v2");
    *(void **)&p_nvmlName   = dlsym(S.nvml, "nvmlDeviceGetName");
    *(void **)&p_nvmlUtil   = dlsym(S.nvml, "nvmlDeviceGetUtilizationRates");
    *(void **)&p_nvmlMem    = dlsym(S.nvml, "nvmlDeviceGetMemoryInfo");
    *(void **)&p_nvmlTemp   = dlsym(S.nvml, "nvmlDeviceGetTemperature");
    *(void **)&p_nvmlDrv    = dlsym(S.nvml, "nvmlSystemGetDriverVersion");
    if (!p_nvmlInit || !p_nvmlCount || !p_nvmlHandle || !p_nvmlName || !p_nvmlUtil || !p_nvmlMem || !p_nvmlTemp)
        return 0;
    if (p_nvmlInit() != 0) return 0;
    unsigned n = 0;
    if (p_nvmlCount(&n) != 0) return 0;
    int added = 0;
    for (unsigned i = 0; i < n && S.ngpu < MAX_GPU; i++) {
        gpu_t *g = &S.gpus[S.ngpu];
        memset(g, 0, sizeof *g);
        if (p_nvmlHandle(i, &g->dev) != 0) continue;
        g->kind = 1;
        if (p_nvmlName(g->dev, g->name, sizeof g->name) != 0) snprintf(g->name, sizeof g->name, "NVIDIA GPU");
        snprintf(g->driver, sizeof g->driver, "nvidia");
        if (p_nvmlDrv) p_nvmlDrv(g->drv_version, sizeof g->drv_version);
        S.ngpu++; added++;
    }
    return added > 0;
}

/* look a PCI vendor:device pair up in pci.ids (one sequential scan, startup only) */
static int pci_lookup(unsigned vendor, unsigned device, char *out, size_t n) {
    const char *files[] = {getenv("TASKMGR_PCIIDS"), "/usr/share/hwdata/pci.ids", "/usr/share/misc/pci.ids",
                           "/usr/share/pci.ids", "/var/lib/pciutils/pci.ids", NULL};
    FILE *f = NULL;
    for (int i = 0; i < 5 && !f; i++) if (files[i]) f = fopen(files[i], "r");
    if (!f) return 0;
    char line[512]; int in_vendor = 0, found = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        if (line[0] == 'C' && line[1] == ' ') break;               /* device classes section: past all vendors */
        char *end;
        if (line[0] != '\t') {
            unsigned v = (unsigned)strtoul(line, &end, 16);
            in_vendor = (end == line + 4 && v == vendor);
            if (!in_vendor && end == line + 4 && v > vendor) break;  /* sorted: not present */
        } else if (in_vendor && line[1] != '\t') {
            unsigned d = (unsigned)strtoul(line + 1, &end, 16);
            if (end == line + 5 && d == device) {
                while (*end == ' ') end++;
                char *nl = strchr(end, '\n'); if (nl) *nl = 0;
                snprintf(out, n, "%s", end);
                found = 1; break;
            }
        }
    }
    fclose(f);
    return found;
}

/* device-tree GPUs (no PCI vendor id): name from the OF compatible string */
static void platform_gpu_name(const char *compat, char *out, size_t n) {
    if (!strncmp(compat, "apple,agx", 9)) snprintf(out, n, "Apple GPU (AGX, %s)", compat + 10);
    else if (!strncmp(compat, "qcom,adreno", 11)) snprintf(out, n, "Qualcomm Adreno %s", compat[11] == '-' ? compat + 12 : "");
    else if (!strncmp(compat, "arm,mali-", 9)) { snprintf(out, n, "ARM Mali-%s", compat + 9); if (out[9]) out[9] = (char)toupper((unsigned char)out[9]); }
    else if (strstr(compat, "v3d") || !strncmp(compat, "brcm,", 5)) snprintf(out, n, "Broadcom VideoCore (%s)", compat);
    else if (!strncmp(compat, "vivante,", 8)) snprintf(out, n, "Vivante %s", compat + 8);
    else if (!strncmp(compat, "nvidia,", 7)) snprintf(out, n, "NVIDIA Tegra GPU (%s)", compat + 7);
    else if (!strncmp(compat, "img,", 4)) snprintf(out, n, "Imagination PowerVR (%s)", compat + 4);
    else snprintf(out, n, "%s", compat);
}

static void gpu_init(void) {
    S.ngpu = 0;
    int nvml_ok = nvml_load();

    char path[400], buf[512];
    snprintf(path, sizeof path, "%s/class/drm", S.sys_root);
    DIR *d = opendir(path);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && S.ngpu < MAX_GPU) {
        if (strncmp(e->d_name, "card", 4) || !isdigit((unsigned char)e->d_name[4]) || strchr(e->d_name, '-')) continue;
        char dev[320];
        snprintf(dev, sizeof dev, "%s/class/drm/%s/device", S.sys_root, e->d_name);
        snprintf(path, sizeof path, "%s/vendor", dev);
        int is_pci = read_file(path, buf, sizeof buf) > 0;
        unsigned vendor = is_pci ? (unsigned)strtoul(buf, NULL, 16) : 0, device = 0;
        if (is_pci) {
            snprintf(path, sizeof path, "%s/device", dev);
            if (read_file(path, buf, sizeof buf) > 0) device = (unsigned)strtoul(buf, NULL, 16);
        }
        if (vendor == 0x10de && nvml_ok) continue;        /* already covered by NVML */

        gpu_t *g = &S.gpus[S.ngpu];
        memset(g, 0, sizeof *g);
        g->kind = 2;
        snprintf(g->path, sizeof g->path, "%s", dev);

        char driver[32] = "", compat[96] = "";
        snprintf(path, sizeof path, "%s/uevent", dev);
        if (read_file(path, buf, sizeof buf) > 0) {
            char *q = strstr(buf, "DRIVER=");
            if (q) sscanf(q + 7, "%31s", driver);
            q = strstr(buf, "OF_COMPATIBLE_0=");
            if (q) sscanf(q + 16, "%95s", compat);
        }
        snprintf(g->driver, sizeof g->driver, "%s", driver);
        if (is_pci) snprintf(g->pci, sizeof g->pci, "%04x:%04x", vendor, device);
        const char *vname = vendor == 0x1002 ? "AMD" : vendor == 0x8086 ? "Intel" : vendor == 0x10de ? "NVIDIA" : "GPU";

        if (!is_pci) {                                       /* SoC GPU: Apple, Adreno, Mali, VideoCore ... */
            if (compat[0]) platform_gpu_name(compat, g->name, sizeof g->name);
            else if (driver[0]) snprintf(g->name, sizeof g->name, "GPU (%s)", driver);
            else snprintf(g->name, sizeof g->name, "GPU (%s)", e->d_name);
        } else if (vendor != 0x10de) {
            char pname[96];
            if (pci_lookup(vendor, device, pname, sizeof pname)) snprintf(g->name, sizeof g->name, "%s %s", vname, pname);
        }

        /* NVIDIA proprietary driver without NVML: model name from /proc/driver/nvidia */
        if (vendor == 0x10de) {
            char link[256];
            ssize_t n = readlink(dev, link, sizeof link - 1);
            if (n > 0) {
                link[n] = 0;
                char *bus = strrchr(link, '/'); bus = bus ? bus + 1 : link;
                snprintf(path, sizeof path, "%s/driver/nvidia/gpus/%s/information", S.proc_root, bus);
                if (read_file(path, buf, sizeof buf) > 0) {
                    char *m = strstr(buf, "Model:");
                    if (m) {
                        m += 6; while (*m == ' ' || *m == '\t') m++;
                        char *nl = strchr(m, '\n'); if (nl) *nl = 0;
                        snprintf(g->name, sizeof g->name, "%s", m);
                    }
                }
            }
        }
        if (!g->name[0] && is_pci) {
            char pname[96];
            if (pci_lookup(vendor, device, pname, sizeof pname)) snprintf(g->name, sizeof g->name, "%s %s", vname, pname);
        }
        if (!g->name[0]) {
            if (driver[0]) snprintf(g->name, sizeof g->name, "%s GPU (%s)", vname, driver);
            else snprintf(g->name, sizeof g->name, "%s GPU", vname);
        }
        snprintf(path, sizeof path, "%s/gpu_busy_percent", dev);    g->has_util = access(path, R_OK) == 0;
        snprintf(path, sizeof path, "%s/mem_info_vram_total", dev); g->has_mem = access(path, R_OK) == 0;

        /* hwmon temperature */
        snprintf(path, sizeof path, "%s/hwmon", dev);
        DIR *hd = opendir(path);
        if (hd) {
            struct dirent *he;
            while ((he = readdir(hd))) {
                if (strncmp(he->d_name, "hwmon", 5)) continue;
                snprintf(g->temp_path, sizeof g->temp_path, "%s/hwmon/%s/temp1_input", dev, he->d_name);
                if (access(g->temp_path, R_OK) == 0) break;
                g->temp_path[0] = 0;
            }
            closedir(hd);
        }
        S.ngpu++;
    }
    closedir(d);
}

static void gpu_sample(void) {
    char path[400], buf[64];
    for (int i = 0; i < S.ngpu; i++) {
        gpu_t *g = &S.gpus[i];
        if (g->kind == 1) {
            nvml_util_t u; nvml_mem_t m; unsigned t;
            if (p_nvmlUtil(g->dev, &u) == 0) { g->util = u.gpu; g->has_util = 1; }
            if (p_nvmlMem(g->dev, &m) == 0) { g->mem_used = m.used; g->mem_total = m.total; g->has_mem = 1; }
            if (p_nvmlTemp(g->dev, 0, &t) == 0) { g->temp = (int)t; g->has_temp = 1; }
        } else {
            if (g->has_util) {
                snprintf(path, sizeof path, "%s/gpu_busy_percent", g->path);
                if (read_file(path, buf, sizeof buf) > 0) g->util = atof(buf);
            }
            if (g->has_mem) {
                snprintf(path, sizeof path, "%s/mem_info_vram_used", g->path);
                if (read_file(path, buf, sizeof buf) > 0) g->mem_used = strtoull(buf, NULL, 10);
                snprintf(path, sizeof path, "%s/mem_info_vram_total", g->path);
                if (read_file(path, buf, sizeof buf) > 0) g->mem_total = strtoull(buf, NULL, 10);
            }
            if (g->temp_path[0] && read_file(g->temp_path, buf, sizeof buf) > 0) { g->temp = atoi(buf) / 1000; g->has_temp = 1; }
        }
    }
}

/* ---- Storage: local filesystems from /proc/mounts + statvfs ---- */

static int fs_wanted(const char *type) {
    static const char *ok[] = {"ext4", "ext3", "ext2", "xfs", "btrfs", "f2fs", "vfat", "exfat", "ntfs", "ntfs3",
                               "zfs", "fuseblk", "jfs", "reiserfs", "bcachefs", "apfs", NULL};
    for (int i = 0; ok[i]; i++) if (!strcmp(type, ok[i])) return 1;
    return 0;
}

static void unescape_mount(char *s) {   /* "\040" -> ' ' etc. */
    char *w = s;
    for (char *r = s; *r;) {
        if (r[0] == '\\' && isdigit((unsigned char)r[1]) && isdigit((unsigned char)r[2]) && isdigit((unsigned char)r[3])) {
            *w++ = (char)((r[1] - '0') * 64 + (r[2] - '0') * 8 + (r[3] - '0'));
            r += 4;
        } else *w++ = *r++;
    }
    *w = 0;
}

static void read_storage(void) {
    char path[320], line[512];
    snprintf(path, sizeof path, "%s/mounts", S.proc_root);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char seen[MAX_FS][64]; int nseen = 0;
    S.nfs = 0;
    while (fgets(line, sizeof line, f) && S.nfs < MAX_FS) {
        char dev[128], mnt[256], type[32];
        if (sscanf(line, "%127s %255s %31s", dev, mnt, type) != 3) continue;
        if (!fs_wanted(type) || !strncmp(dev, "/dev/loop", 9)) continue;
        int dup = 0;
        for (int i = 0; i < nseen; i++) if (!strcmp(seen[i], dev)) { dup = 1; break; }
        if (dup) continue;     /* bind mounts / btrfs subvolumes share one device */
        unescape_mount(mnt);
        struct statvfs st;
        if (statvfs(mnt, &st) != 0 || st.f_blocks == 0) continue;
        snprintf(seen[nseen++], 64, "%s", dev);
        fs_t *x = &S.fs[S.nfs++];
        snprintf(x->mnt, sizeof x->mnt, "%s", mnt);
        snprintf(x->type, sizeof x->type, "%s", type);
        x->total = (unsigned long long)st.f_blocks * st.f_frsize;
        x->used = (unsigned long long)(st.f_blocks - st.f_bfree) * st.f_frsize;
    }
    fclose(f);
}

/* --------------------------------------------------- system info / sensors */

#define MAX_SENS 48
#define MAX_DISK 8
#define MAX_NET 8
#define MAX_CT 4

typedef struct { char chip[24], label[28], path[400]; double temp; int valid; } sensor_t;
typedef struct { char name[16], model[48], kind[12]; unsigned long long bytes; } disk_t;
typedef struct { char name[16], state[12], ip[48], kind[10]; int speed; } net_t;
typedef struct { char name[40]; int count; unsigned long max_khz; } coretype_t;

static struct {
    char machine[96], os[96], kernel[64], hostname[64], firmware[80];
    char arch[64], cpu_model[96], cpu_vendor[32], features[200], virt[64], cache[160], topology[160];
    int sockets, cores, threads;
    unsigned long max_khz, cur_khz;
    int has_cpu_temp; double cpu_temp; int cpu_temp_sensor; char cpu_temp_path[400];
    sensor_t sens[MAX_SENS]; int nsens;
    disk_t disks[MAX_DISK]; int ndisk;
    net_t nets[MAX_NET]; int nnet;
    int has_bat, bat_pct; char bat_status[16], bat_path[400];
    coretype_t ct[MAX_CT]; int nct;
} SI;

static int read_str(const char *path, char *out, size_t n) {
    int r = read_file(path, out, n);
    if (r <= 0) { out[0] = 0; return 0; }
    while (r > 0 && (out[r - 1] == '\n' || out[r - 1] == '\r' || out[r - 1] == ' ')) out[--r] = 0;
    return 1;
}
static unsigned long read_ul(const char *path) { char b[64]; return read_str(path, b, sizeof b) ? strtoul(b, NULL, 10) : 0; }
static void fmt_hz(unsigned long khz, char *out, size_t n) {
    if (khz >= 1000000) snprintf(out, n, "%.2f GHz", khz / 1e6); else snprintf(out, n, "%lu MHz", khz / 1000);
}
static void squeeze_spaces(char *s) {       /* "Intel(R) Core(TM)  i7   CPU" -> single spaces */
    char *w = s; int sp = 0;
    for (char *r = s; *r; r++) { if (*r == ' ' || *r == '\t') { if (!sp && w != s) *w++ = ' '; sp = 1; } else { *w++ = *r; sp = 0; } }
    while (w > s && w[-1] == ' ') w--;
    *w = 0;
}
static int has_flag(const char *flags, const char *f) {
    size_t n = strlen(f);
    for (const char *p = flags; (p = strstr(p, f)); p += n)
        if ((p == flags || p[-1] == ' ') && (p[n] == 0 || p[n] == ' ')) return 1;
    return 0;
}

static const char *arm_impl_name(unsigned impl) {
    switch (impl) {
    case 0x41: return "ARM"; case 0x61: return "Apple"; case 0x51: return "Qualcomm"; case 0x4e: return "NVIDIA";
    case 0x53: return "Samsung"; case 0x48: return "HiSilicon"; case 0x43: return "Marvell"; case 0xc0: return "Ampere";
    case 0x42: return "Broadcom"; case 0x46: return "Fujitsu"; case 0x69: return "Intel"; case 0x70: return "Phytium";
    default: return "ARM";
    }
}

static const char *arm_core_name(unsigned impl, unsigned part) {
    static const struct { unsigned impl, part; const char *name; } t[] = {
        {0x41,0xb76,"ARM1176"},{0x41,0xc07,"Cortex-A7"},{0x41,0xc08,"Cortex-A8"},{0x41,0xc09,"Cortex-A9"},{0x41,0xc0d,"Cortex-A17"},{0x41,0xc0f,"Cortex-A15"},
        {0x41,0xd03,"Cortex-A53"},{0x41,0xd04,"Cortex-A35"},{0x41,0xd05,"Cortex-A55"},{0x41,0xd07,"Cortex-A57"},{0x41,0xd08,"Cortex-A72"},
        {0x41,0xd09,"Cortex-A73"},{0x41,0xd0a,"Cortex-A75"},{0x41,0xd0b,"Cortex-A76"},{0x41,0xd0c,"Neoverse-N1"},{0x41,0xd0d,"Cortex-A77"},
        {0x41,0xd0e,"Cortex-A76AE"},{0x41,0xd40,"Neoverse-V1"},{0x41,0xd41,"Cortex-A78"},{0x41,0xd42,"Cortex-A78AE"},{0x41,0xd44,"Cortex-X1"},
        {0x41,0xd46,"Cortex-A510"},{0x41,0xd47,"Cortex-A710"},{0x41,0xd48,"Cortex-X2"},{0x41,0xd49,"Neoverse-N2"},{0x41,0xd4a,"Neoverse-E1"},
        {0x41,0xd4b,"Cortex-A78C"},{0x41,0xd4c,"Cortex-X1C"},{0x41,0xd4d,"Cortex-A715"},{0x41,0xd4e,"Cortex-X3"},{0x41,0xd4f,"Neoverse-V2"},
        {0x41,0xd80,"Cortex-A520"},{0x41,0xd81,"Cortex-A720"},{0x41,0xd82,"Cortex-X4"},{0x41,0xd84,"Neoverse-V3"},{0x41,0xd85,"Cortex-X925"},
        {0x41,0xd87,"Cortex-A725"},{0x41,0xd8e,"Neoverse-N3"},
        {0x61,0x022,"Icestorm"},{0x61,0x023,"Firestorm"},{0x61,0x024,"Icestorm"},{0x61,0x025,"Firestorm"},{0x61,0x028,"Icestorm"},{0x61,0x029,"Firestorm"},
        {0x61,0x032,"Blizzard"},{0x61,0x033,"Avalanche"},{0x61,0x034,"Blizzard"},{0x61,0x035,"Avalanche"},{0x61,0x038,"Blizzard"},{0x61,0x039,"Avalanche"},
        {0x61,0x048,"Sawtooth"},{0x61,0x049,"Everest"},
        {0x51,0x001,"Oryon"},{0x51,0x800,"Kryo 2xx Gold"},{0x51,0x801,"Kryo 2xx Silver"},{0x51,0x802,"Kryo 3xx Gold"},{0x51,0x803,"Kryo 3xx Silver"},
        {0x51,0x804,"Kryo 4xx Gold"},{0x51,0x805,"Kryo 4xx Silver"},{0x51,0xc00,"Falkor"},{0x51,0xc01,"Saphira"},
        {0x4e,0x003,"Denver"},{0x4e,0x004,"Carmel"},{0x53,0x001,"Exynos M1"},{0x53,0x002,"Exynos M3"},{0x53,0x003,"Exynos M4"},{0x53,0x004,"Exynos M5"},
        {0x48,0xd01,"TaiShan v110"},{0x43,0x0af,"ThunderX2"},{0x43,0x0b8,"ThunderX3"},{0xc0,0xac3,"Ampere-1"},{0xc0,0xac4,"Ampere-1a"},{0x70,0x663,"Phecda"},
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) if (t[i].impl == impl && t[i].part == part) return t[i].name;
    return NULL;
}

/* SoC marketing name from a device-tree compatible string such as "apple,t8103" */
static const char *soc_name(const char *c) {
    static const struct { const char *id, *name; } t[] = {
        {"apple,t8103","Apple M1"},{"apple,t6000","Apple M1 Pro"},{"apple,t6001","Apple M1 Max"},{"apple,t6002","Apple M1 Ultra"},
        {"apple,t8112","Apple M2"},{"apple,t6020","Apple M2 Pro"},{"apple,t6021","Apple M2 Max"},{"apple,t6022","Apple M2 Ultra"},
        {"apple,t8122","Apple M3"},{"apple,t6030","Apple M3 Pro"},{"apple,t6031","Apple M3 Max"},{"apple,t6034","Apple M3 Max"},
        {"apple,t8132","Apple M4"},{"apple,t6040","Apple M4 Pro"},{"apple,t6041","Apple M4 Max"},
        {"qcom,x1e80100","Snapdragon X Elite"},{"qcom,x1p42100","Snapdragon X Plus"},{"qcom,sc8280xp","Snapdragon 8cx Gen 3"},
        {"qcom,sc8180x","Snapdragon 8cx"},{"qcom,sc7180","Snapdragon 7c"},{"qcom,sc7280","Snapdragon 7c Gen 3"},
        {"qcom,sm8750","Snapdragon 8 Elite"},{"qcom,sm8650","Snapdragon 8 Gen 3"},{"qcom,sm8550","Snapdragon 8 Gen 2"},
        {"qcom,sm8450","Snapdragon 8 Gen 1"},{"qcom,sm8350","Snapdragon 888"},{"qcom,sm8250","Snapdragon 865"},{"qcom,sm8150","Snapdragon 855"},
        {"qcom,sdm845","Snapdragon 845"},{"qcom,sdm670","Snapdragon 670"},{"qcom,sm7325","Snapdragon 778G"},{"qcom,sm6350","Snapdragon 690"},
        {"qcom,msm8998","Snapdragon 835"},{"qcom,msm8996","Snapdragon 820"},{"qcom,msm8916","Snapdragon 410"},{"qcom,qcs6490","Qualcomm QCS6490"},
        {"brcm,bcm2712","Broadcom BCM2712 (Raspberry Pi 5)"},{"brcm,bcm2711","Broadcom BCM2711 (Raspberry Pi 4)"},
        {"brcm,bcm2837","Broadcom BCM2837 (Raspberry Pi 3)"},{"brcm,bcm2836","Broadcom BCM2836 (Raspberry Pi 2)"},{"brcm,bcm2835","Broadcom BCM2835 (Raspberry Pi)"},
        {"rockchip,rk3588","Rockchip RK3588"},{"rockchip,rk3399","Rockchip RK3399"},{"rockchip,rk3566","Rockchip RK3566"},{"rockchip,rk3568","Rockchip RK3568"},{"rockchip,rk3328","Rockchip RK3328"},
        {"mediatek,mt8183","MediaTek MT8183"},{"mediatek,mt8192","MediaTek MT8192"},{"mediatek,mt8195","MediaTek MT8195"},{"mediatek,mt8186","MediaTek MT8186"},
        {"nvidia,tegra234","NVIDIA Tegra Orin"},{"nvidia,tegra194","NVIDIA Tegra Xavier"},{"nvidia,tegra210","NVIDIA Tegra X1"},{"nvidia,tegra186","NVIDIA Tegra X2"},
        {"amlogic,s905","Amlogic S905"},{"amlogic,s922x","Amlogic S922X"},{"amlogic,a311d","Amlogic A311D"},
        {"allwinner,sun50i-h6","Allwinner H6"},{"allwinner,sun50i-h616","Allwinner H616"},{"allwinner,sun50i-a64","Allwinner A64"},
        {"ti,am625","TI AM625"},{"fsl,imx8mp","NXP i.MX 8M Plus"},{"fsl,imx8mq","NXP i.MX 8M Quad"},{"starfive,jh7110","StarFive JH7110"},
        {"sophgo,sg2042","Sophgo SG2042"},{"xlnx,zynqmp","Xilinx ZynqMP"},
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) if (!strncmp(c, t[i].id, strlen(t[i].id))) return t[i].name;
    return NULL;
}

static void soc_vendor(const char *c, char *out, size_t n) {
    static const struct { const char *id, *name; } t[] = {
        {"apple","Apple"},{"qcom","Qualcomm"},{"brcm","Broadcom"},{"rockchip","Rockchip"},{"mediatek","MediaTek"},{"nvidia","NVIDIA"},
        {"amlogic","Amlogic"},{"allwinner","Allwinner"},{"ti","Texas Instruments"},{"fsl","NXP"},{"nxp","NXP"},{"samsung","Samsung"},
        {"hisilicon","HiSilicon"},{"marvell","Marvell"},{"starfive","StarFive"},{"sifive","SiFive"},{"xlnx","Xilinx"},{"sophgo","Sophgo"},
        {"raspberrypi","Raspberry Pi"},{"intel","Intel"},{"amd","AMD"},
    };
    const char *comma = strchr(c, ',');
    size_t len = comma ? (size_t)(comma - c) : strlen(c);
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++)
        if (strlen(t[i].id) == len && !strncmp(c, t[i].id, len)) { snprintf(out, n, "%s", t[i].name); return; }
    snprintf(out, n, "%.*s", (int)len, c);
    if (out[0]) out[0] = (char)toupper((unsigned char)out[0]);
}

static void describe_arch(const char *m, char *out, size_t n) {
    int bits = (int)(sizeof(void *) * 8);
    if (!strcmp(m, "x86_64") || !strcmp(m, "amd64")) snprintf(out, n, "x86_64 (x64, 64-bit)");
    else if (m[0] == 'i' && strstr(m, "86")) snprintf(out, n, "x86 (%s, 32-bit)", m);
    else if (!strcmp(m, "aarch64") || !strcmp(m, "arm64")) snprintf(out, n, "ARM64 (aarch64, 64-bit)");
    else if (!strncmp(m, "arm", 3)) snprintf(out, n, "ARM (%s, 32-bit)", m);
    else if (!strncmp(m, "riscv", 5)) snprintf(out, n, "RISC-V (%s)", m);
    else if (!strncmp(m, "ppc64", 5)) snprintf(out, n, "PowerPC (%s, 64-bit)", m);
    else if (!strcmp(m, "loongarch64")) snprintf(out, n, "LoongArch (64-bit)");
    else if (!strcmp(m, "s390x")) snprintf(out, n, "IBM Z (s390x, 64-bit)");
    else if (!strcmp(m, "mips64")) snprintf(out, n, "MIPS (64-bit)");
    else snprintf(out, n, "%s", m);
    if (bits == 32 && strstr(out, "64-bit")) { size_t l = strlen(out); snprintf(out + l, n - l, " · 32-bit process"); }
}

static void spec_cpu(void) {
    char path[400], line[4096];
    char model[96] = "", vendor[32] = "", flags[4096] = "", hw[64] = "", cachesz[32] = "";
    static unsigned impl_of[MAX_CORES], part_of[MAX_CORES];
    int cpu = -1; unsigned impl = 0, part = 0;

    snprintf(path, sizeof path, "%s/cpuinfo", S.proc_root);
    FILE *f = fopen(path, "r");
    if (f) {
        while (fgets(line, sizeof line, f)) {
            char *colon = strchr(line, ':');
            if (!colon) { cpu = -1; continue; }
            *colon = 0;
            char *key = line, *val = colon + 1;
            for (char *e = key + strlen(key); e > key && (e[-1] == ' ' || e[-1] == '\t'); e--) e[-1] = 0;
            while (*val == ' ' || *val == '\t') val++;
            char *nl = strchr(val, '\n'); if (nl) *nl = 0;
            if (!strcmp(key, "processor") && isdigit((unsigned char)val[0])) { cpu = atoi(val); if (cpu >= 0 && cpu < MAX_CORES) { impl_of[cpu] = part_of[cpu] = 0; } }
            else if (!strcmp(key, "vendor_id") && !vendor[0]) snprintf(vendor, sizeof vendor, "%s", val);
            else if ((!strcmp(key, "model name") || !strcmp(key, "Processor") || !strcmp(key, "uarch")) && !model[0]) snprintf(model, sizeof model, "%s", val);
            else if (!strcmp(key, "cpu") && !model[0] && strchr(val, ' ')) snprintf(model, sizeof model, "%s", val);
            else if (!strcmp(key, "cache size") && !cachesz[0]) snprintf(cachesz, sizeof cachesz, "%s", val);
            else if ((!strcmp(key, "flags") || !strcmp(key, "Features")) && !flags[0]) snprintf(flags, sizeof flags, " %s ", val);
            else if (!strcmp(key, "CPU implementer")) { impl = (unsigned)strtoul(val, NULL, 16); if (cpu >= 0 && cpu < MAX_CORES) impl_of[cpu] = impl; }
            else if (!strcmp(key, "CPU part")) { part = (unsigned)strtoul(val, NULL, 16); if (cpu >= 0 && cpu < MAX_CORES) part_of[cpu] = part; }
            else if (!strcmp(key, "Hardware") && !hw[0]) snprintf(hw, sizeof hw, "%s", val);
        }
        fclose(f);
    }
    squeeze_spaces(model);

    /* topology from sysfs */
    static unsigned long pkg_of[MAX_CORES], core_of[MAX_CORES], maxk_of[MAX_CORES];
    static int present[MAX_CORES];
    int threads = 0, sockets = 0, cores = 0;
    for (int i = 0; i < MAX_CORES; i++) {
        snprintf(path, sizeof path, "%s/devices/system/cpu/cpu%d", S.sys_root, i);
        present[i] = access(path, F_OK) == 0;
        if (!present[i]) continue;
        threads++;
        snprintf(path, sizeof path, "%s/devices/system/cpu/cpu%d/topology/physical_package_id", S.sys_root, i); pkg_of[i] = read_ul(path);
        snprintf(path, sizeof path, "%s/devices/system/cpu/cpu%d/topology/core_id", S.sys_root, i);             core_of[i] = read_ul(path);
        snprintf(path, sizeof path, "%s/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", S.sys_root, i);     maxk_of[i] = read_ul(path);
        if (maxk_of[i] > SI.max_khz) SI.max_khz = maxk_of[i];
    }
    for (int i = 0; i < MAX_CORES; i++) {
        if (!present[i]) continue;
        int newpkg = 1, newcore = 1;
        for (int j = 0; j < i; j++) {
            if (!present[j]) continue;
            if (pkg_of[j] == pkg_of[i]) { newpkg = 0; if (core_of[j] == core_of[i]) newcore = 0; }
        }
        sockets += newpkg; cores += newcore;
    }
    if (!threads) { threads = cores = S.ncpu; sockets = 1; }
    SI.sockets = sockets; SI.cores = cores; SI.threads = threads;

    /* core types: group by (implementer, part, max freq) -> big.LITTLE / P+E summary */
    SI.nct = 0;
    for (int i = 0; i < MAX_CORES; i++) {
        if (!present[i]) continue;
        const char *nm = arm_core_name(impl_of[i], part_of[i]);
        char name[40];
        if (nm) snprintf(name, sizeof name, "%s", nm);
        else if (impl_of[i]) snprintf(name, sizeof name, "%s core 0x%03x", arm_impl_name(impl_of[i]), part_of[i]);
        else name[0] = 0;
        int k;
        for (k = 0; k < SI.nct; k++) if (!strcmp(SI.ct[k].name, name) && SI.ct[k].max_khz == maxk_of[i]) break;
        if (k == SI.nct) { if (SI.nct == MAX_CT) continue; snprintf(SI.ct[k].name, sizeof SI.ct[k].name, "%s", name); SI.ct[k].max_khz = maxk_of[i]; SI.ct[k].count = 0; SI.nct++; }
        SI.ct[k].count++;
    }
    const char *x = S.ascii ? "x" : "×", *dot = S.ascii ? "-" : "·";
    if (SI.nct > 1 || (SI.nct == 1 && SI.ct[0].name[0] && impl_of[0])) {
        int n = snprintf(SI.topology, sizeof SI.topology, "%d cores%s%d threads: ", cores, S.ascii ? " / " : " · ", threads);
        for (int k = 0; k < SI.nct && n < (int)sizeof SI.topology; k++) {
            char hz[24] = "";
            if (SI.ct[k].max_khz) { char t[20]; fmt_hz(SI.ct[k].max_khz, t, sizeof t); snprintf(hz, sizeof hz, " @ %s", t); }
            n += snprintf(SI.topology + n, sizeof SI.topology - (size_t)n, "%s%d%s %s%s", k ? " + " : "", SI.ct[k].count, x,
                          SI.ct[k].name[0] ? SI.ct[k].name : "cores", hz);
        }
    } else {
        snprintf(SI.topology, sizeof SI.topology, "%d socket%s %s %d cores %s %d threads", sockets, sockets > 1 ? "s" : "", dot, cores, dot, threads);
    }

    /* device tree: machine + SoC */
    char compat[512]; int clen;
    char soc[96] = "", socvendor[32] = "";
    snprintf(path, sizeof path, "%s/device-tree/compatible", S.proc_root);
    if ((clen = read_file(path, compat, sizeof compat)) > 0) {
        const char *second = NULL; int idx = 0;
        for (int off = 0; off < clen; off += (int)strlen(compat + off) + 1, idx++) {
            const char *tok = compat + off;
            if (!*tok) continue;
            const char *nm = soc_name(tok);
            if (nm && !soc[0]) { snprintf(soc, sizeof soc, "%s", nm); soc_vendor(tok, socvendor, sizeof socvendor); }
            if (idx == 1) second = tok;
        }
        if (!soc[0]) {
            const char *tok = second ? second : compat;
            soc_vendor(tok, socvendor, sizeof socvendor);
            const char *comma = strchr(tok, ',');
            snprintf(soc, sizeof soc, "%s %s", socvendor, comma ? comma + 1 : tok);
        }
    }

    /* vendor + model */
    if (!strcmp(vendor, "GenuineIntel")) snprintf(SI.cpu_vendor, sizeof SI.cpu_vendor, "Intel");
    else if (!strcmp(vendor, "AuthenticAMD")) snprintf(SI.cpu_vendor, sizeof SI.cpu_vendor, "AMD");
    else if (!strcmp(vendor, "HygonGenuine")) snprintf(SI.cpu_vendor, sizeof SI.cpu_vendor, "Hygon");
    else if (strstr(vendor, "Centaur") || strstr(vendor, "Shanghai")) snprintf(SI.cpu_vendor, sizeof SI.cpu_vendor, "Zhaoxin");
    else if (vendor[0]) snprintf(SI.cpu_vendor, sizeof SI.cpu_vendor, "%s", vendor);
    else if (socvendor[0]) snprintf(SI.cpu_vendor, sizeof SI.cpu_vendor, "%s", socvendor);
    else if (impl_of[0]) snprintf(SI.cpu_vendor, sizeof SI.cpu_vendor, "%s", arm_impl_name(impl_of[0]));
    else snprintf(SI.cpu_vendor, sizeof SI.cpu_vendor, "Unknown");

    if (model[0] && !strstr(model, "Processor rev") && !strstr(model, "ARMv")) snprintf(SI.cpu_model, sizeof SI.cpu_model, "%s", model);
    else if (soc[0]) snprintf(SI.cpu_model, sizeof SI.cpu_model, "%s", soc);
    else if (model[0]) snprintf(SI.cpu_model, sizeof SI.cpu_model, "%s", model);
    else if (hw[0]) snprintf(SI.cpu_model, sizeof SI.cpu_model, "%s", hw);
    else if (SI.nct) {
        int n = 0;
        for (int k = 0; k < SI.nct && n < (int)sizeof SI.cpu_model; k++)
            n += snprintf(SI.cpu_model + n, sizeof SI.cpu_model - (size_t)n, "%s%s", k ? " + " : "", SI.ct[k].name);
    } else snprintf(SI.cpu_model, sizeof SI.cpu_model, "Unknown");
    if (soc[0] && model[0] && !strstr(SI.cpu_model, soc)) {  /* "Cortex-A76 (Broadcom BCM2712)" */
        size_t l = strlen(SI.cpu_model); snprintf(SI.cpu_model + l, sizeof SI.cpu_model - l, " (%s)", soc);
    }

    /* cache: cpu0 cache indexes, counted across all cpus by distinct shared_cpu_list */
    int n = 0;
    for (int idx = 0; idx < 8 && n < (int)sizeof SI.cache - 20; idx++) {
        char lvl[8], type[16], size[16];
        snprintf(path, sizeof path, "%s/devices/system/cpu/cpu0/cache/index%d/level", S.sys_root, idx);
        if (!read_str(path, lvl, sizeof lvl)) break;
        snprintf(path, sizeof path, "%s/devices/system/cpu/cpu0/cache/index%d/type", S.sys_root, idx); read_str(path, type, sizeof type);
        snprintf(path, sizeof path, "%s/devices/system/cpu/cpu0/cache/index%d/size", S.sys_root, idx); read_str(path, size, sizeof size);
        static char seen[MAX_CORES][64]; int nseen = 0;
        for (int i = 0; i < MAX_CORES; i++) {
            if (!present[i]) continue;
            char sh[64];
            snprintf(path, sizeof path, "%s/devices/system/cpu/cpu%d/cache/index%d/shared_cpu_list", S.sys_root, i, idx);
            if (!read_str(path, sh, sizeof sh)) continue;
            int dup = 0; for (int j = 0; j < nseen; j++) if (!strcmp(seen[j], sh)) { dup = 1; break; }
            if (!dup && nseen < MAX_CORES) snprintf(seen[nseen++], 64, "%s", sh);
        }
        { char *endp; unsigned long kb = strtoul(size, &endp, 10);
          if (*endp == 'K' && kb >= 1024 && kb % 1024 == 0) snprintf(size, sizeof size, "%luM", kb / 1024); }
        const char *lab = !strcmp(type, "Data") ? "d" : !strcmp(type, "Instruction") ? "i" : "";
        n += snprintf(SI.cache + n, sizeof SI.cache - (size_t)n, "%sL%s%s %s", n ? (S.ascii ? "  " : " · ") : "", lvl, lab, size);
        if (nseen > 1) n += snprintf(SI.cache + n, sizeof SI.cache - (size_t)n, " %s%d", x, nseen);
    }
    if (!SI.cache[0] && cachesz[0]) snprintf(SI.cache, sizeof SI.cache, "%s", cachesz);

    /* instruction-set highlights + virtualization */
    static const struct { const char *flag, *name; } fl[] = {
        {"sse4_2","SSE4.2"},{"avx","AVX"},{"avx2","AVX2"},{"avx512f","AVX-512"},{"fma","FMA"},{"aes","AES"},{"sha_ni","SHA"},{"amx_tile","AMX"},
        {"asimd","NEON"},{"neon","NEON"},{"sha2","SHA2"},{"sha3","SHA3"},{"sve","SVE"},{"sve2","SVE2"},{"sme","SME"},{"i8mm","I8MM"},{"bf16","BF16"},
        {"asimddp","DotProd"},{"atomics","LSE"},{"crc32","CRC32"},{"lrcpc","RCpc"},{"pauth","PAuth"},{"bti","BTI"},{"mte","MTE"},
    };
    n = 0;
    for (size_t i = 0; i < sizeof fl / sizeof fl[0] && n < (int)sizeof SI.features - 12; i++)
        if (has_flag(flags, fl[i].flag) && !strstr(SI.features, fl[i].name))
            n += snprintf(SI.features + n, sizeof SI.features - (size_t)n, "%s%s", n ? ", " : "", fl[i].name);
    if (has_flag(flags, "vmx")) snprintf(SI.virt, sizeof SI.virt, "Intel VT-x");
    else if (has_flag(flags, "svm")) snprintf(SI.virt, sizeof SI.virt, "AMD-V");
    else if (access("/dev/kvm", F_OK) == 0) snprintf(SI.virt, sizeof SI.virt, "KVM available");
    if (has_flag(flags, "hypervisor")) { size_t l = strlen(SI.virt); snprintf(SI.virt + l, sizeof SI.virt - l, "%srunning inside a VM", l ? " · " : ""); }
}

static void spec_sensors(void) {
    char path[400], buf[64], dir[400];
    SI.nsens = 0; SI.cpu_temp_sensor = -1; SI.cpu_temp_path[0] = 0;
    snprintf(path, sizeof path, "%s/class/hwmon", S.sys_root);
    DIR *d = opendir(path);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && SI.nsens < MAX_SENS) {
            if (strncmp(e->d_name, "hwmon", 5)) continue;
            snprintf(dir, sizeof dir, "%s/class/hwmon/%s", S.sys_root, e->d_name);
            char chip[24];
            snprintf(path, sizeof path, "%s/name", dir);
            if (!read_str(path, chip, sizeof chip)) snprintf(chip, sizeof chip, "%s", e->d_name);
            for (int t = 1; t <= 32 && SI.nsens < MAX_SENS; t++) {
                snprintf(path, sizeof path, "%s/temp%d_input", dir, t);
                if (access(path, R_OK) != 0) continue;
                sensor_t *sn = &SI.sens[SI.nsens];
                snprintf(sn->path, sizeof sn->path, "%s", path);
                snprintf(sn->chip, sizeof sn->chip, "%s", chip);
                snprintf(path, sizeof path, "%s/temp%d_label", dir, t);
                if (!read_str(path, sn->label, sizeof sn->label)) snprintf(sn->label, sizeof sn->label, "temp%d", t);
                sn->valid = 0;
                SI.nsens++;
            }
        }
        closedir(d);
    }
    /* thermal zones: used as sensors when hwmon has none, and as a CPU-temp fallback */
    snprintf(path, sizeof path, "%s/class/thermal", S.sys_root);
    d = opendir(path);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (strncmp(e->d_name, "thermal_zone", 12)) continue;
            snprintf(path, sizeof path, "%s/class/thermal/%s/type", S.sys_root, e->d_name);
            if (!read_str(path, buf, sizeof buf)) continue;
            char lower[64]; for (int i = 0; buf[i] && i < 63; i++) { lower[i] = (char)tolower((unsigned char)buf[i]); lower[i + 1] = 0; }
            if (!SI.cpu_temp_path[0] && (strstr(lower, "cpu") || strstr(lower, "x86_pkg") || strstr(lower, "soc")))
                snprintf(SI.cpu_temp_path, sizeof SI.cpu_temp_path, "%s/class/thermal/%s/temp", S.sys_root, e->d_name);
        }
        closedir(d);
    }
    if (!SI.nsens) {
        snprintf(path, sizeof path, "%s/class/thermal", S.sys_root);
        d = opendir(path);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) && SI.nsens < MAX_SENS) {
                if (strncmp(e->d_name, "thermal_zone", 12)) continue;
                sensor_t *sn = &SI.sens[SI.nsens];
                snprintf(sn->path, sizeof sn->path, "%s/class/thermal/%s/temp", S.sys_root, e->d_name);
                snprintf(path, sizeof path, "%s/class/thermal/%s/type", S.sys_root, e->d_name);
                if (!read_str(path, sn->label, sizeof sn->label)) snprintf(sn->label, sizeof sn->label, "%s", e->d_name + 8);
                snprintf(sn->chip, sizeof sn->chip, "thermal");
                SI.nsens++;
            }
            closedir(d);
        }
    }
    /* pick the CPU temperature sensor */
    static const char *cpu_chips[] = {"coretemp", "k10temp", "zenpower", "cpu_thermal", "cpu-thermal", "soc_thermal", "cpu", NULL};
    for (int pass = 0; pass < 4 && SI.cpu_temp_sensor < 0; pass++)
        for (int i = 0; i < SI.nsens && SI.cpu_temp_sensor < 0; i++) {
            const sensor_t *sn = &SI.sens[i];
            int cpuchip = 0; for (int c = 0; cpu_chips[c]; c++) if (!strcasecmp(sn->chip, cpu_chips[c])) cpuchip = 1;
            if (pass == 0 && cpuchip && (!strncmp(sn->label, "Package", 7) || !strcmp(sn->label, "Tctl") || !strcmp(sn->label, "Tdie"))) SI.cpu_temp_sensor = i;
            if (pass == 1 && cpuchip) SI.cpu_temp_sensor = i;
            if (pass == 2 && (strcasestr(sn->label, "cpu") || strcasestr(sn->chip, "cpu"))) SI.cpu_temp_sensor = i;
            if (pass == 3 && (strcasestr(sn->label, "soc") || strcasestr(sn->chip, "soc"))) SI.cpu_temp_sensor = i;
        }
}

static int cmp_name16(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }  /* name[] is the first field */

static void spec_init(void) {
    char path[400], buf[512];
    S.etc_root = getenv("TASKMGR_ETC") ? getenv("TASKMGR_ETC") : "/etc";
    struct utsname u; memset(&u, 0, sizeof u); uname(&u);

    snprintf(path, sizeof path, "%s/sys/kernel/hostname", S.proc_root);
    if (!read_str(path, SI.hostname, sizeof SI.hostname)) snprintf(SI.hostname, sizeof SI.hostname, "%s", u.nodename);
    snprintf(path, sizeof path, "%s/sys/kernel/osrelease", S.proc_root);
    if (!read_str(path, SI.kernel, sizeof SI.kernel)) snprintf(SI.kernel, sizeof SI.kernel, "%s", u.release);
    snprintf(path, sizeof path, "%s/os-release", S.etc_root);
    if (read_file(path, buf, sizeof buf) > 0) {
        char *q = strstr(buf, "PRETTY_NAME=");
        if (q) {
            q += 12; if (*q == '"') q++;
            char *end = strpbrk(q, "\"\n"); if (end) *end = 0;
            snprintf(SI.os, sizeof SI.os, "%s", q);
        }
    }
    if (!SI.os[0]) snprintf(SI.os, sizeof SI.os, "%s %s", u.sysname, u.release);
    describe_arch(u.machine, SI.arch, sizeof SI.arch);
    spec_cpu();                       /* before the DMI block, which may append a VM note to SI.virt */

    /* machine: device tree model (ARM boards, Apple, phones) or DMI (PCs) */
    snprintf(path, sizeof path, "%s/device-tree/model", S.proc_root);
    read_str(path, SI.machine, sizeof SI.machine);
    if (!SI.machine[0]) {
        char vend[64] = "", prod[64] = "", ver[64] = "";
        snprintf(path, sizeof path, "%s/devices/virtual/dmi/id/sys_vendor", S.sys_root);   read_str(path, vend, sizeof vend);
        snprintf(path, sizeof path, "%s/devices/virtual/dmi/id/product_name", S.sys_root); read_str(path, prod, sizeof prod);
        snprintf(path, sizeof path, "%s/devices/virtual/dmi/id/product_version", S.sys_root); read_str(path, ver, sizeof ver);
        if (strstr(ver, "To be filled") || !strcmp(ver, "None") || !strcmp(ver, "Default string")) ver[0] = 0;
        if (vend[0] || prod[0]) snprintf(SI.machine, sizeof SI.machine, "%s%s%s%s%s", vend, vend[0] && prod[0] ? " " : "", prod, ver[0] ? " " : "", ver);
        char bv[32] = "", bver[32] = "", bd[16] = "";
        snprintf(path, sizeof path, "%s/devices/virtual/dmi/id/bios_vendor", S.sys_root);  read_str(path, bv, sizeof bv);
        snprintf(path, sizeof path, "%s/devices/virtual/dmi/id/bios_version", S.sys_root); read_str(path, bver, sizeof bver);
        snprintf(path, sizeof path, "%s/devices/virtual/dmi/id/bios_date", S.sys_root);    read_str(path, bd, sizeof bd);
        if (bver[0]) snprintf(SI.firmware, sizeof SI.firmware, "%s%s%s%s%s%s", bv, bv[0] ? " " : "", bver, bd[0] ? " (" : "", bd, bd[0] ? ")" : "");
        if (strstr(prod, "Virtual") || strstr(prod, "KVM") || strstr(prod, "QEMU") || strstr(vend, "QEMU") || strstr(prod, "VMware")) {
            size_t l = strlen(SI.virt); if (!strstr(SI.virt, "VM")) snprintf(SI.virt + l, sizeof SI.virt - l, "%svirtual machine", l ? " · " : "");
        }
    }

    spec_sensors();

    /* block devices */
    SI.ndisk = 0;
    snprintf(path, sizeof path, "%s/block", S.sys_root);
    DIR *d = opendir(path);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && SI.ndisk < MAX_DISK) {
            const char *nm = e->d_name;
            if (nm[0] == '.' || !strncmp(nm, "loop", 4) || !strncmp(nm, "ram", 3) || !strncmp(nm, "zram", 4) || !strncmp(nm, "dm-", 3) || !strncmp(nm, "sr", 2)) continue;
            disk_t *dk = &SI.disks[SI.ndisk];
            memset(dk, 0, sizeof *dk);
            snprintf(dk->name, sizeof dk->name, "%s", nm);
            snprintf(path, sizeof path, "%s/block/%s/size", S.sys_root, nm);
            dk->bytes = (unsigned long long)read_ul(path) * 512ULL;
            if (!dk->bytes) continue;
            snprintf(path, sizeof path, "%s/block/%s/device/model", S.sys_root, nm);
            if (!read_str(path, dk->model, sizeof dk->model)) { snprintf(path, sizeof path, "%s/block/%s/device/name", S.sys_root, nm); read_str(path, dk->model, sizeof dk->model); }
            squeeze_spaces(dk->model);
            snprintf(path, sizeof path, "%s/block/%s/queue/rotational", S.sys_root, nm);
            int rot = (int)read_ul(path);
            if (!strncmp(nm, "nvme", 4)) snprintf(dk->kind, sizeof dk->kind, "NVMe SSD");
            else if (!strncmp(nm, "mmcblk", 6)) snprintf(dk->kind, sizeof dk->kind, "SD/eMMC");
            else if (!strncmp(nm, "vd", 2) || !strncmp(nm, "xvd", 3)) snprintf(dk->kind, sizeof dk->kind, "virtual");
            else if (!strncmp(nm, "md", 2)) snprintf(dk->kind, sizeof dk->kind, "RAID");
            else snprintf(dk->kind, sizeof dk->kind, rot ? "HDD" : "SSD");
            SI.ndisk++;
        }
        closedir(d);
    }

    qsort(SI.disks, (size_t)SI.ndisk, sizeof SI.disks[0], cmp_name16);

    /* network interfaces (physical only) */
    SI.nnet = 0;
    snprintf(path, sizeof path, "%s/class/net", S.sys_root);
    d = opendir(path);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && SI.nnet < MAX_NET) {
            if (e->d_name[0] == '.' || !strcmp(e->d_name, "lo")) continue;
            char link[400];
            snprintf(path, sizeof path, "%s/class/net/%s", S.sys_root, e->d_name);
            ssize_t ln = readlink(path, link, sizeof link - 1);
            if (ln > 0) { link[ln] = 0; if (strstr(link, "/virtual/")) continue; }
            net_t *nt = &SI.nets[SI.nnet];
            memset(nt, 0, sizeof *nt);
            snprintf(nt->name, sizeof nt->name, "%s", e->d_name);
            snprintf(path, sizeof path, "%s/class/net/%s/operstate", S.sys_root, e->d_name); read_str(path, nt->state, sizeof nt->state);
            snprintf(path, sizeof path, "%s/class/net/%s/speed", S.sys_root, e->d_name);
            char sp[16]; nt->speed = read_str(path, sp, sizeof sp) ? atoi(sp) : 0;
            snprintf(path, sizeof path, "%s/class/net/%s/wireless", S.sys_root, e->d_name);
            snprintf(nt->kind, sizeof nt->kind, access(path, F_OK) == 0 ? "Wi-Fi" : "Ethernet");
            SI.nnet++;
        }
        closedir(d);
    }
    qsort(SI.nets, (size_t)SI.nnet, sizeof SI.nets[0], cmp_name16);
    struct ifaddrs *ifa, *p;
    if (getifaddrs(&ifa) == 0) {
        for (p = ifa; p; p = p->ifa_next) {
            if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
            for (int i = 0; i < SI.nnet; i++)
                if (!strcmp(SI.nets[i].name, p->ifa_name) && !SI.nets[i].ip[0])
                    inet_ntop(AF_INET, &((struct sockaddr_in *)p->ifa_addr)->sin_addr, SI.nets[i].ip, sizeof SI.nets[i].ip);
        }
        freeifaddrs(ifa);
    }

    /* battery */
    SI.has_bat = 0;
    snprintf(path, sizeof path, "%s/class/power_supply", S.sys_root);
    d = opendir(path);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && !SI.has_bat) {
            if (e->d_name[0] == '.') continue;
            snprintf(path, sizeof path, "%s/class/power_supply/%s/type", S.sys_root, e->d_name);
            if (read_str(path, buf, sizeof buf) && !strcmp(buf, "Battery")) {
                snprintf(SI.bat_path, sizeof SI.bat_path, "%s/class/power_supply/%s", S.sys_root, e->d_name);
                SI.has_bat = 1;
            }
        }
        closedir(d);
    }
}

/* per-tick dynamic bits: temperatures, battery, current frequency (frequency only on the System tab) */
static void spec_sample(void) {
    char path[400], buf[64];
    for (int i = 0; i < SI.nsens; i++) {
        sensor_t *sn = &SI.sens[i];
        if (read_str(sn->path, buf, sizeof buf)) { sn->temp = atof(buf) / 1000.0; sn->valid = 1; } else sn->valid = 0;
    }
    SI.has_cpu_temp = 0;
    if (SI.cpu_temp_sensor >= 0 && SI.sens[SI.cpu_temp_sensor].valid) { SI.cpu_temp = SI.sens[SI.cpu_temp_sensor].temp; SI.has_cpu_temp = 1; }
    else if (SI.cpu_temp_path[0] && read_str(SI.cpu_temp_path, buf, sizeof buf)) { SI.cpu_temp = atof(buf) / 1000.0; SI.has_cpu_temp = 1; }

    if (SI.has_bat) {
        snprintf(path, sizeof path, "%s/capacity", SI.bat_path); SI.bat_pct = (int)read_ul(path);
        snprintf(path, sizeof path, "%s/status", SI.bat_path);   read_str(path, SI.bat_status, sizeof SI.bat_status);
    }

    if (S.tab == TAB_SPEC) {
        unsigned long long sum = 0; int n = 0;
        for (int i = 0; i < MAX_CORES && n < S.ncpu; i++) {
            snprintf(path, sizeof path, "%s/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", S.sys_root, i);
            unsigned long k = read_ul(path);
            if (k) { sum += k; n++; }
            else if (i > 0 && !n) break;      /* no cpufreq at all */
        }
        if (n) SI.cur_khz = (unsigned long)(sum / (unsigned long long)n);
        else {                                /* fall back to /proc/cpuinfo "cpu MHz" */
            snprintf(path, sizeof path, "%s/cpuinfo", S.proc_root);
            FILE *f = fopen(path, "r");
            if (f) {
                char line[512]; double mhz = 0; int m = 0;
                while (fgets(line, sizeof line, f)) if (!strncmp(line, "cpu MHz", 7)) { char *c = strchr(line, ':'); if (c) { mhz += atof(c + 1); m++; } }
                fclose(f);
                if (m) SI.cur_khz = (unsigned long)(mhz / m * 1000);
            }
        }
    }
}

static void sample(void) {
    double t = now_sec(), dt = t - (S.last.tv_sec + S.last.tv_nsec / 1e9);
    if (dt <= 0) dt = 1;
    memcpy(S.cpu_prev, S.cpu_now, sizeof S.cpu_now);
    S.rx_prev = S.rx; S.tx_prev = S.tx; S.rd_prev = S.rd; S.wr_prev = S.wr;

    read_cpu();
    read_mem();
    read_net();
    read_disk();
    read_misc();
    gpu_sample();
    spec_sample();
    read_storage();
    read_procs();

    /* counters that went backwards (interface reset, wrap) count as 0 for this tick */
#define RATE(now, prev) ((now) >= (prev) ? (double)((now) - (prev)) / dt : 0.0)
    if (S.rx_prev || S.tx_prev) { S.rx_rate = RATE(S.rx, S.rx_prev); S.tx_rate = RATE(S.tx, S.tx_prev); }
    if (S.rd_prev || S.wr_prev) { S.rd_rate = RATE(S.rd, S.rd_prev); S.wr_rate = RATE(S.wr, S.wr_prev); }
#undef RATE
    S.hist_cpu[S.hist_pos] = S.cpu_pct[0];
    S.hist_mem[S.hist_pos] = S.mem_total ? 100.0 * (double)(S.mem_total - S.mem_avail) / (double)S.mem_total : 0;
    S.hist_pos = (S.hist_pos + 1) % HIST;
    if (S.hist_n < HIST) S.hist_n++;

    /* keep current snapshot as "prev" (sorted by pid) for next delta */
    if (S.cap_prev < S.ncur) {
        S.cap_prev = S.cap_cur;
        S.prev = realloc(S.prev, (size_t)S.cap_prev * sizeof *S.prev);
        if (!S.prev) { endwin(); perror("realloc"); exit(1); }
    }
    memcpy(S.prev, S.cur, (size_t)S.ncur * sizeof *S.cur);
    S.nprev = S.ncur;
    qsort(S.prev, (size_t)S.nprev, sizeof *S.prev, cmp_pid);
    clock_gettime(CLOCK_MONOTONIC, &S.last);
}

/* --------------------------------------------------------- filter + sort */

static int ci_contains(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    if (!n) return 1;
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < n && hay[i] && tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == n) return 1;
    }
    return 0;
}

static int cmp_view(const void *a, const void *b) {
    const proc_t *x = &S.cur[*(const int *)a], *y = &S.cur[*(const int *)b];
    int r = 0;
    switch (S.sort) {
    case SORT_CPU:  r = (x->cpu < y->cpu) - (x->cpu > y->cpu); break;          /* desc */
    case SORT_MEM:  r = (x->rss_kb < y->rss_kb) - (x->rss_kb > y->rss_kb); break; /* desc */
    case SORT_NAME: r = strcasecmp(x->name, y->name); break;
    case SORT_PID:  r = (x->pid > y->pid) - (x->pid < y->pid); break;
    case SORT_USER: r = strcmp(uid_name(x->uid), uid_name(y->uid)); break;
    }
    if (r == 0) r = (x->pid > y->pid) - (x->pid < y->pid);
    return S.reverse ? -r : r;
}

static void build_view(void) {
    if (S.cap_view < S.ncur) {
        S.cap_view = S.cap_cur;
        S.view = realloc(S.view, (size_t)S.cap_view * sizeof *S.view);
        if (!S.view) { endwin(); perror("realloc"); exit(1); }
    }
    S.nview = 0;
    char pidstr[16];
    for (int i = 0; i < S.ncur; i++) {
        if (S.filter[0]) {
            snprintf(pidstr, sizeof pidstr, "%d", S.cur[i].pid);
            if (!ci_contains(S.cur[i].name, S.filter) && !ci_contains(pidstr, S.filter) &&
                !ci_contains(uid_name(S.cur[i].uid), S.filter))
                continue;
        }
        S.view[S.nview++] = i;
    }
    qsort(S.view, (size_t)S.nview, sizeof *S.view, cmp_view);

    /* keep selection on the same pid if it still exists */
    int found = -1;
    if (S.sel_pid > 0)
        for (int i = 0; i < S.nview; i++) if (S.cur[S.view[i]].pid == S.sel_pid) { found = i; break; }
    if (found >= 0) S.sel_idx = found;
    if (S.sel_idx >= S.nview) S.sel_idx = S.nview - 1;
    if (S.sel_idx < 0) S.sel_idx = 0;
    S.sel_pid = S.nview ? S.cur[S.view[S.sel_idx]].pid : 0;
}

/* ---------------------------------------------------------------- drawing */

static void fill_line(int y, int pair) {
    attron(COLOR_PAIR(pair));
    mvhline(y, 0, ' ', COLS);
    attroff(COLOR_PAIR(pair));
}

static int pct_color(double p) { return p < 50 ? C_OK : p < 85 ? C_WARN : C_BAD; }
static int temp_color(double t) { return t < 60 ? C_OK : t < 80 ? C_WARN : C_BAD; }

static int row_ok(int y) { return y >= 2 && y < LINES - 1; }
static void put(int y, int x, const char *s) { if (row_ok(y)) mvaddstr(y, x, s); }

static void draw_bar(int y, int x, int w, double pct, int pair) {
    if (w < 1 || !row_ok(y)) return;
    int fill = (int)(pct / 100.0 * w + 0.5);
    if (fill > w) fill = w;
    attron(COLOR_PAIR(pair));
    for (int i = 0; i < fill; i++) mvaddstr(y, x + i, S.ascii ? "#" : "█");
    attroff(COLOR_PAIR(pair));
    attron(COLOR_PAIR(C_DIM));
    for (int i = fill; i < w; i++) mvaddstr(y, x + i, S.ascii ? "." : "░");
    attroff(COLOR_PAIR(C_DIM));
}

/* history graph: newest sample at the right edge */
static void draw_graph(int y, int x, int w, int h, const double *hist, int pair) {
    static const char *blocks[] = {" ", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
    attron(COLOR_PAIR(C_DIM));
    for (int r = 0; r < h; r++) for (int c = 0; c < w; c++) put(y + r, x + c, S.ascii ? "." : " ");
    attroff(COLOR_PAIR(C_DIM));
    attron(COLOR_PAIR(pair));
    for (int c = 0; c < w && c < S.hist_n; c++) {
        int idx = (S.hist_pos - 1 - c + HIST * 2) % HIST;
        double v = hist[idx] / 100.0 * h;      /* in rows */
        int col = x + w - 1 - c;
        int full = (int)v;
        double frac = v - full;
        for (int r = 0; r < full && r < h; r++) put(y + h - 1 - r, col, S.ascii ? "#" : "█");
        if (full < h) {
            int part = (int)(frac * 8 + 0.5);
            if (S.ascii) { if (part >= 4) put(y + h - 1 - full, col, "-"); }
            else if (part > 0) put(y + h - 1 - full, col, blocks[part]);
        }
    }
    attroff(COLOR_PAIR(pair));
}

static void draw_header(void) {
    fill_line(0, C_TITLE);
    attron(COLOR_PAIR(C_TITLE) | A_BOLD);
    mvaddstr(0, 1, "Task Manager");
    attroff(A_BOLD);
    const char *tabs[TAB_COUNT] = {" Processes ", " Performance ", " System "};
    int x = 16;
    for (int i = 0; i < TAB_COUNT; i++) {
        if (i == S.tab) { attron(COLOR_PAIR(C_TABSEL) | A_BOLD); mvaddstr(0, x, tabs[i]); attroff(COLOR_PAIR(C_TABSEL) | A_BOLD); attron(COLOR_PAIR(C_TITLE)); }
        else mvaddstr(0, x, tabs[i]);
        x += (int)strlen(tabs[i]) + 1;
    }
    char right[64];
    snprintf(right, sizeof right, "refresh %.2gs  q:quit", S.interval_ms / 1000.0);
    mvaddstr(0, COLS - (int)strlen(right) - 1, right);
    attroff(COLOR_PAIR(C_TITLE));

    /* summary line */
    char used[24], tot[24], up[24];
    double mem_pct = S.mem_total ? 100.0 * (double)(S.mem_total - S.mem_avail) / (double)S.mem_total : 0;
    fmt_bytes((double)(S.mem_total - S.mem_avail) * 1024, used, sizeof used);
    fmt_bytes((double)S.mem_total * 1024, tot, sizeof tot);
    fmt_uptime(S.uptime, up, sizeof up);
    move(1, 0); clrtoeol();
    mvaddstr(1, 1, "CPU ");
    attron(COLOR_PAIR(pct_color(S.cpu_pct[0])) | A_BOLD); printw("%3.0f%%", S.cpu_pct[0]); attroff(COLOR_PAIR(pct_color(S.cpu_pct[0])) | A_BOLD);
    if (SI.has_cpu_temp) { attron(COLOR_PAIR(temp_color(SI.cpu_temp))); printw(" %.0f%s", SI.cpu_temp, S.ascii ? "C" : "°C"); attroff(COLOR_PAIR(temp_color(SI.cpu_temp))); }
    printw("   Memory ");
    attron(COLOR_PAIR(pct_color(mem_pct)) | A_BOLD); printw("%3.0f%%", mem_pct); attroff(COLOR_PAIR(pct_color(mem_pct)) | A_BOLD);
    if (COLS >= 100) printw(" (%s / %s)   %d processes   %d threads   up %s", used, tot, S.ncur, S.total_threads, up);
    else printw(" (%s / %s)   %d procs   up %s", used, tot, S.ncur, up);
}

static void draw_footer(void) {
    int y = LINES - 1;
    fill_line(y, C_COLHDR);
    attron(COLOR_PAIR(C_COLHDR));
    if (S.filter_mode) {
        mvprintw(y, 1, "Search: %s_   (Enter: keep, Esc: clear)", S.filter);
    } else if (S.msg[0] && time(NULL) < S.msg_until) {
        mvaddstr(y, 1, S.msg);
    } else if (S.tab == TAB_PROC) {
        mvaddstr(y, 1, "Tab:switch  ↑↓:select  Del/e:End task  K:force kill  /:search  "
                       "c/m/n/p/u:sort  r:reverse  +/-:speed  q:quit");
    } else {
        mvaddstr(y, 1, "Tab:switch  \u2191\u2193/PgUp/PgDn:scroll  +/-:speed  q:quit");
    }
    attroff(COLOR_PAIR(C_COLHDR));
}

static void draw_processes(void) {
    const int W_PID = 7, W_ST = 10, W_CPU = 6, W_MEM = 11, W_THR = 5, W_USER = 10;
    int fixed = 1 + W_PID + 1 + W_ST + 1 + W_CPU + 1 + W_MEM + 1 + W_THR + 1 + W_USER + 1;
    int w_name = COLS - fixed - 1;
    if (w_name < 8) w_name = 8;

    fill_line(2, C_COLHDR);
    attron(COLOR_PAIR(C_COLHDR) | A_BOLD);
    const char *arrow = S.ascii ? (S.reverse ? "^" : "v") : (S.reverse ? "▴" : "▾");
    char h_name[64], h_pid[16], h_st[16], h_cpu[16], h_mem[16], h_thr[16], h_usr[16];
    snprintf(h_name, sizeof h_name, "Name%s", S.sort == SORT_NAME ? arrow : "");
    snprintf(h_pid, sizeof h_pid, "PID%s", S.sort == SORT_PID ? arrow : "");
    snprintf(h_st, sizeof h_st, "Status");
    snprintf(h_cpu, sizeof h_cpu, "CPU%%%s", S.sort == SORT_CPU ? arrow : "");
    snprintf(h_mem, sizeof h_mem, "Memory%s", S.sort == SORT_MEM ? arrow : "");
    snprintf(h_thr, sizeof h_thr, "Thr");
    snprintf(h_usr, sizeof h_usr, "User%s", S.sort == SORT_USER ? arrow : "");
    mvprintw(2, 1, "%-*s %*s %-*s %*s %*s %*s %-*s", w_name, h_name, W_PID, h_pid, W_ST, h_st,
             W_CPU, h_cpu, W_MEM, h_mem, W_THR, h_thr, W_USER, h_usr);
    attroff(COLOR_PAIR(C_COLHDR) | A_BOLD);

    int top = 3, rows = LINES - top - 1;
    if (rows < 1) return;
    if (S.sel_idx < S.scroll) S.scroll = S.sel_idx;
    if (S.sel_idx >= S.scroll + rows) S.scroll = S.sel_idx - rows + 1;
    if (S.scroll > S.nview - rows) S.scroll = S.nview - rows;
    if (S.scroll < 0) S.scroll = 0;

    for (int r = 0; r < rows; r++) {
        int y = top + r, i = S.scroll + r;
        move(y, 0); clrtoeol();
        if (i >= S.nview) continue;
        const proc_t *p = &S.cur[S.view[i]];
        char mem[24]; fmt_bytes((double)p->rss_kb * 1024, mem, sizeof mem);
        int selected = (i == S.sel_idx);
        if (selected) { attron(COLOR_PAIR(C_SEL)); mvhline(y, 0, ' ', COLS); }
        else if (p->state == 'R') attron(COLOR_PAIR(C_ACCENT));
        else if (p->state == 'Z' || p->state == 'D') attron(COLOR_PAIR(C_WARN));
        mvprintw(y, 1, "%-*.*s %*d %-*s %*.1f %*s %*d %-*.*s", w_name, w_name, p->name, W_PID, p->pid,
                 W_ST, state_name(p->state), W_CPU, p->cpu, W_MEM, mem, W_THR, p->threads,
                 W_USER, W_USER, uid_name(p->uid));
        attroff(COLOR_PAIR(C_SEL)); attroff(COLOR_PAIR(C_ACCENT)); attroff(COLOR_PAIR(C_WARN));
    }
}

/* one "Label  NN%   rest" line; pct hidden when show_pct == 0 */
static void section(int y, const char *label, double pct, int show_pct, const char *rest) {
    if (!row_ok(y)) return;
    attron(A_BOLD); mvprintw(y, 1, "%-8s", label); attroff(A_BOLD);
    if (show_pct) {
        attron(COLOR_PAIR(pct_color(pct)) | A_BOLD); printw("%3.0f%%", pct); attroff(COLOR_PAIR(pct_color(pct)) | A_BOLD);
    } else printw("    ");
    printw("   %s", rest);
}

static void dim_line(int y, const char *text) {
    if (!row_ok(y)) return;
    attron(COLOR_PAIR(C_DIM)); mvaddstr(y, 1, text); attroff(COLOR_PAIR(C_DIM));
}

static void draw_performance(void) {
    for (int y = 2; y < LINES - 1; y++) { move(y, 0); clrtoeol(); }
    int gw = COLS - 4, gh = 6;
    if (gw < 10) return;
    int y = 2 - S.perf_scroll;
    char a[32], b[32], c[32], d[32], rest[256];
    double mem_pct = S.mem_total ? 100.0 * (double)(S.mem_total - S.mem_avail) / (double)S.mem_total : 0;

    /* CPU */
    int rn = snprintf(rest, sizeof rest, "%d cores   load %.2f %.2f %.2f   %d running", S.ncpu, S.load[0], S.load[1], S.load[2], S.running);
    if (SI.has_cpu_temp) snprintf(rest + rn, sizeof rest - (size_t)rn, "   %.0f%s", SI.cpu_temp, S.ascii ? "C" : "°C");
    section(y++, "CPU", S.cpu_pct[0], 1, rest);
    draw_graph(y, 2, gw, gh, S.hist_cpu, C_ACCENT); y += gh + 1;
    int cell = 20, per_row = gw / cell; if (per_row < 1) per_row = 1;
    for (int i = 1; i <= S.ncpu; i++) {
        int col = (i - 1) % per_row, x = 2 + col * cell;
        if (row_ok(y)) {
            mvprintw(y, x, "%2d", i - 1);
            draw_bar(y, x + 3, 10, S.cpu_pct[i], pct_color(S.cpu_pct[i]));
            mvprintw(y, x + 14, "%3.0f%%", S.cpu_pct[i]);
        }
        if (col == per_row - 1 || i == S.ncpu) y++;
    }
    y++;

    /* Memory */
    fmt_bytes((double)(S.mem_total - S.mem_avail) * 1024, a, sizeof a);
    fmt_bytes((double)S.mem_total * 1024, b, sizeof b);
    fmt_bytes((double)S.mem_cached * 1024, c, sizeof c);
    snprintf(rest, sizeof rest, "%s / %s   cached %s", a, b, c);
    section(y++, "Memory", mem_pct, 1, rest);
    draw_graph(y, 2, gw, 3, S.hist_mem, C_OK); y += 4;

    /* Swap */
    double swap_pct = S.swap_total ? 100.0 * (double)(S.swap_total - S.swap_free) / (double)S.swap_total : 0;
    fmt_bytes((double)(S.swap_total - S.swap_free) * 1024, a, sizeof a);
    fmt_bytes((double)S.swap_total * 1024, b, sizeof b);
    snprintf(rest, sizeof rest, "%s / %s", a, b);
    section(y++, "Swap", swap_pct, 1, rest);
    draw_bar(y, 2, gw, swap_pct, pct_color(swap_pct)); y += 2;

    /* GPU */
    if (!S.ngpu) { dim_line(y, "GPU      none detected"); y += 2; }
    for (int i = 0; i < S.ngpu; i++) {
        const gpu_t *g = &S.gpus[i];
        char label[12];
        if (S.ngpu > 1) snprintf(label, sizeof label, "GPU %d", i); else snprintf(label, sizeof label, "GPU");
        int n = snprintf(rest, sizeof rest, "%s", g->name);
        if (g->has_mem) {
            fmt_bytes((double)g->mem_used, a, sizeof a); fmt_bytes((double)g->mem_total, b, sizeof b);
            n += snprintf(rest + n, sizeof rest - (size_t)n, "   mem %s / %s", a, b);
        }
        if (g->has_temp) snprintf(rest + n, sizeof rest - (size_t)n, "   %d%s", g->temp, S.ascii ? "C" : "°C");
        section(y++, label, g->util, g->has_util, rest);
        if (g->has_util) { draw_bar(y, 2, gw, g->util, pct_color(g->util)); y += 2; }
        else if (g->has_mem && g->mem_total) {
            double mp = 100.0 * (double)g->mem_used / (double)g->mem_total;
            draw_bar(y, 2, gw, mp, pct_color(mp)); y += 2;
        } else y++;
    }

    /* Sensors (hwmon) */
    if (SI.nsens) {
        if (row_ok(y)) { attron(A_BOLD); mvaddstr(y, 1, "Sensors"); attroff(A_BOLD); }
        y++;
        int i = 0;
        while (i < SI.nsens) {
            const char *chip = SI.sens[i].chip;
            if (row_ok(y)) { attron(COLOR_PAIR(C_DIM)); mvprintw(y, 2, "%-12.12s", chip); attroff(COLOR_PAIR(C_DIM)); }
            int x = 15;
            for (; i < SI.nsens && !strcmp(SI.sens[i].chip, chip); i++) {
                const sensor_t *sn = &SI.sens[i];
                char cellbuf[64];
                int len = snprintf(cellbuf, sizeof cellbuf, "%s %.0f%s", sn->label, sn->temp, S.ascii ? "C" : "°C");
                int vis = len - (S.ascii ? 0 : 1);                /* degree sign is 2 bytes, 1 column */
                if (x + vis > COLS - 2 && x > 15) { y++; x = 15; }
                if (row_ok(y) && sn->valid) {
                    mvprintw(y, x, "%s ", sn->label);
                    attron(COLOR_PAIR(temp_color(sn->temp)) | A_BOLD); printw("%.0f%s", sn->temp, S.ascii ? "C" : "°C"); attroff(COLOR_PAIR(temp_color(sn->temp)) | A_BOLD);
                }
                x += vis + 3;
            }
            y++;
        }
        y++;
    }

    /* Network + Disk I/O */
    fmt_rate(S.rx_rate, a, sizeof a); fmt_rate(S.tx_rate, b, sizeof b);
    fmt_bytes((double)S.rx, c, sizeof c); fmt_bytes((double)S.tx, d, sizeof d);
    snprintf(rest, sizeof rest, "%s %-12s %s %-12s  total %s %s / %s %s", S.ascii ? "down" : "↓", a, S.ascii ? "up" : "↑", b,
             S.ascii ? "down" : "↓", c, S.ascii ? "up" : "↑", d);
    section(y++, "Network", 0, 0, rest);
    fmt_rate(S.rd_rate, a, sizeof a); fmt_rate(S.wr_rate, b, sizeof b);
    fmt_bytes((double)S.rd, c, sizeof c); fmt_bytes((double)S.wr, d, sizeof d);
    snprintf(rest, sizeof rest, "read %-12s write %-12s  total read %s / write %s", a, b, c, d);
    section(y++, "Disk I/O", 0, 0, rest);
    y++;

    /* Storage capacity */
    if (row_ok(y)) { attron(A_BOLD); mvaddstr(y, 1, "Storage"); attroff(A_BOLD); }
    y++;
    if (!S.nfs) { dim_line(y, "  no local filesystems found"); y++; }
    int mw = 8;                                   /* mount-point column: fit the longest, max 30 */
    for (int i = 0; i < S.nfs; i++) { int l = (int)strlen(S.fs[i].mnt); if (l > mw) mw = l; }
    if (mw > 30) mw = 30;
    int bw = gw - mw - 42; if (bw < 8) bw = 8;
    for (int i = 0; i < S.nfs; i++) {
        const fs_t *x = &S.fs[i];
        double p = x->total ? 100.0 * (double)x->used / (double)x->total : 0;
        fmt_bytes((double)x->used, a, sizeof a); fmt_bytes((double)x->total, b, sizeof b);
        if (row_ok(y)) {
            mvprintw(y, 2, "%-*.*s", mw, mw, x->mnt);
            draw_bar(y, 3 + mw, bw, p, pct_color(p));
            attron(COLOR_PAIR(pct_color(p))); mvprintw(y, 4 + mw + bw, "%3.0f%%", p); attroff(COLOR_PAIR(pct_color(p)));
            printw("  %s / %s  %s", a, b, x->type);
        }
        y++;
    }
    y++;

    char up[32]; fmt_uptime(S.uptime, up, sizeof up);
    snprintf(rest, sizeof rest, "Processes %d   Threads %d   Uptime %s", S.ncur, S.total_threads, up);
    dim_line(y, rest); y++;

    S.perf_height = y + S.perf_scroll - 2;   /* total content rows, for scroll clamping */
    if (S.perf_scroll > 0 && row_ok(2)) { attron(COLOR_PAIR(C_DIM)); mvaddstr(2, COLS - 3, S.ascii ? "^" : "▲"); attroff(COLOR_PAIR(C_DIM)); }
    if (S.perf_height - S.perf_scroll > LINES - 3) { attron(COLOR_PAIR(C_DIM)); mvaddstr(LINES - 2, COLS - 3, S.ascii ? "v" : "▼"); attroff(COLOR_PAIR(C_DIM)); }
}

static void kv(int y, const char *key, const char *val) {
    if (!row_ok(y)) return;
    attron(COLOR_PAIR(C_DIM)); mvprintw(y, 3, "%-14s", key); attroff(COLOR_PAIR(C_DIM));
    if (COLS > 19) mvaddnstr(y, 18, val, COLS - 19);
}
static void title(int y, const char *t) {
    if (!row_ok(y)) return;
    attron(A_BOLD | COLOR_PAIR(C_ACCENT)); mvaddstr(y, 1, t); attroff(A_BOLD | COLOR_PAIR(C_ACCENT));
}

static void draw_specs(void) {
    for (int y = 2; y < LINES - 1; y++) { move(y, 0); clrtoeol(); }
    int y = 2 - S.spec_scroll;
    char a[64], b[64], buf[320];
    const char *deg = S.ascii ? "C" : "°C";

    title(y++, "System");
    if (SI.machine[0]) kv(y++, "Machine", SI.machine);
    kv(y++, "OS", SI.os);
    kv(y++, "Kernel", SI.kernel);
    kv(y++, "Hostname", SI.hostname);
    if (SI.firmware[0]) kv(y++, "Firmware", SI.firmware);
    fmt_uptime(S.uptime, a, sizeof a); kv(y++, "Uptime", a);
    y++;

    title(y++, "CPU");
    kv(y++, "Model", SI.cpu_model);
    kv(y++, "Vendor", SI.cpu_vendor);
    kv(y++, "Architecture", SI.arch);
    kv(y++, "Cores", SI.topology);
    if (SI.cur_khz || SI.max_khz) {
        int n = 0;
        if (SI.cur_khz) { fmt_hz(SI.cur_khz, a, sizeof a); n += snprintf(buf + n, sizeof buf - (size_t)n, "now %s", a); }
        if (SI.max_khz) { fmt_hz(SI.max_khz, b, sizeof b); n += snprintf(buf + n, sizeof buf - (size_t)n, "%smax %s", n ? "   " : "", b); }
        kv(y++, "Frequency", buf);
    }
    if (SI.cache[0]) kv(y++, "Cache", SI.cache);
    if (SI.features[0]) kv(y++, "Features", SI.features);
    if (SI.virt[0]) kv(y++, "Virtualization", SI.virt);
    if (SI.has_cpu_temp) { snprintf(buf, sizeof buf, "%.0f%s", SI.cpu_temp, deg); kv(y++, "Temperature", buf); }
    y++;

    title(y++, "GPU");
    if (!S.ngpu) kv(y++, "", "none detected");
    for (int i = 0; i < S.ngpu; i++) {
        const gpu_t *g = &S.gpus[i];
        char key[12]; snprintf(key, sizeof key, "GPU %d", i);
        int n = snprintf(buf, sizeof buf, "%s", g->name);
        if (g->mem_total) { fmt_bytes((double)g->mem_total, a, sizeof a); n += snprintf(buf + n, sizeof buf - (size_t)n, "   VRAM %s", a); }
        if (g->driver[0]) n += snprintf(buf + n, sizeof buf - (size_t)n, "   driver %s%s%s", g->driver, g->drv_version[0] ? " " : "", g->drv_version);
        if (g->pci[0]) n += snprintf(buf + n, sizeof buf - (size_t)n, "   [%s]", g->pci);
        if (g->has_temp) snprintf(buf + n, sizeof buf - (size_t)n, "   %d%s", g->temp, deg);
        kv(y++, key, buf);
    }
    y++;

    title(y++, "Memory");
    fmt_bytes((double)S.mem_total * 1024, a, sizeof a); fmt_bytes((double)S.mem_avail * 1024, b, sizeof b);
    snprintf(buf, sizeof buf, "%s   (available %s)", a, b); kv(y++, "RAM", buf);
    fmt_bytes((double)S.swap_total * 1024, a, sizeof a); kv(y++, "Swap", S.swap_total ? a : "none");
    y++;

    title(y++, "Storage");
    if (!SI.ndisk) kv(y++, "", "no block devices found");
    for (int i = 0; i < SI.ndisk; i++) {
        const disk_t *dk = &SI.disks[i];
        fmt_bytes((double)dk->bytes, a, sizeof a);
        snprintf(buf, sizeof buf, "%-10s %-24s %s", a, dk->kind, dk->model[0] ? dk->model : "");
        kv(y++, dk->name, buf);
    }
    y++;

    title(y++, "Network");
    if (!SI.nnet) kv(y++, "", "no physical interfaces found");
    for (int i = 0; i < SI.nnet; i++) {
        const net_t *nt = &SI.nets[i];
        int n = snprintf(buf, sizeof buf, "%-9s %-8s", nt->kind, nt->state);
        if (nt->speed > 0) n += snprintf(buf + n, sizeof buf - (size_t)n, " %5d Mb/s", nt->speed);
        if (nt->ip[0]) snprintf(buf + n, sizeof buf - (size_t)n, "   %s", nt->ip);
        kv(y++, nt->name, buf);
    }
    if (SI.has_bat) {
        y++;
        title(y++, "Battery");
        snprintf(buf, sizeof buf, "%d%%   %s", SI.bat_pct, SI.bat_status);
        kv(y++, "Charge", buf);
        if (row_ok(y)) draw_bar(y, 18, COLS - 20 > 10 ? COLS - 20 : 10, SI.bat_pct, SI.bat_pct > 50 ? C_OK : SI.bat_pct > 20 ? C_WARN : C_BAD);
        y++;
    }

    S.spec_height = y + S.spec_scroll - 2;
    if (S.spec_scroll > 0 && row_ok(2)) { attron(COLOR_PAIR(C_DIM)); mvaddstr(2, COLS - 3, S.ascii ? "^" : "▲"); attroff(COLOR_PAIR(C_DIM)); }
    if (S.spec_height - S.spec_scroll > LINES - 3) { attron(COLOR_PAIR(C_DIM)); mvaddstr(LINES - 2, COLS - 3, S.ascii ? "v" : "▼"); attroff(COLOR_PAIR(C_DIM)); }
}

static void draw(void) {
    erase();
    draw_header();
    if (S.tab == TAB_PROC) draw_processes(); else if (S.tab == TAB_PERF) draw_performance(); else draw_specs();
    draw_footer();
    refresh();
}

/* ------------------------------------------------------------------ input */

static int confirm(const char *prompt) {
    int y = LINES - 1;
    fill_line(y, C_BAD);
    attron(COLOR_PAIR(C_BAD) | A_BOLD);
    mvprintw(y, 1, "%s  [y/N]", prompt);
    attroff(COLOR_PAIR(C_BAD) | A_BOLD);
    refresh();
    timeout(-1);
    int ch = getch();
    timeout(S.interval_ms);
    return ch == 'y' || ch == 'Y';
}

static void end_task(int sig) {
    if (!S.nview) return;
    const proc_t *p = &S.cur[S.view[S.sel_idx]];
    char q[160];
    snprintf(q, sizeof q, "%s '%s' (PID %d)?", sig == SIGKILL ? "Force kill" : "End task", p->name, p->pid);
    if (!confirm(q)) { set_msg("Cancelled"); return; }
    if (kill(p->pid, sig) == 0) set_msg("Sent %s to %s (%d)", sig == SIGKILL ? "SIGKILL" : "SIGTERM", p->name, p->pid);
    else set_msg("Failed: %s", strerror(errno));
}

static void set_sort(int s) {
    if (S.sort == s) S.reverse = !S.reverse;
    else { S.sort = s; S.reverse = 0; }
}

static int handle_key(int ch) {
    if (S.filter_mode) {
        size_t n = strlen(S.filter);
        if (ch == 27) { S.filter[0] = 0; S.filter_mode = 0; }
        else if (ch == '\n' || ch == KEY_ENTER) S.filter_mode = 0;
        else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) { if (n) S.filter[n - 1] = 0; }
        else if (ch >= 32 && ch < 127 && n < sizeof S.filter - 1) { S.filter[n] = (char)ch; S.filter[n + 1] = 0; }
        S.sel_idx = 0; S.sel_pid = 0;
        return 1;
    }
    switch (ch) {
    case 'q': case 'Q': case KEY_F(10): return 0;
    case '\t': case KEY_RIGHT: S.tab = (S.tab + 1) % TAB_COUNT; break;
    case KEY_BTAB: case KEY_LEFT: S.tab = (S.tab + TAB_COUNT - 1) % TAB_COUNT; break;
    case '1': S.tab = TAB_PROC; break;
    case '2': S.tab = TAB_PERF; break;
    case '3': S.tab = TAB_SPEC; break;
    case '+': case '=': if (S.interval_ms > 250) S.interval_ms -= 250; timeout(S.interval_ms); break;
    case '-': case '_': if (S.interval_ms < 5000) S.interval_ms += 250; timeout(S.interval_ms); break;
    case 27: if (S.filter[0]) { S.filter[0] = 0; } break;
    default: break;
    }
    int rows = LINES - 4; if (rows < 1) rows = 1;
    if (S.tab == TAB_PERF || S.tab == TAB_SPEC) {
        int *sc = S.tab == TAB_PERF ? &S.perf_scroll : &S.spec_scroll;
        int height = S.tab == TAB_PERF ? S.perf_height : S.spec_height;
        switch (ch) {
        case KEY_UP: case 'k': (*sc)--; break;
        case KEY_DOWN: case 'j': (*sc)++; break;
        case KEY_PPAGE: *sc -= rows; break;
        case KEY_NPAGE: *sc += rows; break;
        case KEY_HOME: *sc = 0; break;
        case KEY_END: *sc = height; break;
        default: break;
        }
        int max_scroll = height - (LINES - 3);
        if (max_scroll < 0) max_scroll = 0;
        if (*sc > max_scroll) *sc = max_scroll;
        if (*sc < 0) *sc = 0;
        return 1;
    }
    switch (ch) {
    case KEY_UP: case 'k': S.sel_idx--; break;
    case KEY_DOWN: case 'j': S.sel_idx++; break;
    case KEY_PPAGE: S.sel_idx -= rows; break;
    case KEY_NPAGE: S.sel_idx += rows; break;
    case KEY_HOME: S.sel_idx = 0; break;
    case KEY_END: S.sel_idx = S.nview - 1; break;
    case 'c': set_sort(SORT_CPU); break;
    case 'm': set_sort(SORT_MEM); break;
    case 'n': set_sort(SORT_NAME); break;
    case 'p': set_sort(SORT_PID); break;
    case 'u': set_sort(SORT_USER); break;
    case 'r': S.reverse = !S.reverse; break;
    case '/': S.filter_mode = 1; break;
    case KEY_DC: case 'e': end_task(SIGTERM); break;
    case 'K': end_task(SIGKILL); break;
    default: break;
    }
    if (S.sel_idx >= S.nview) S.sel_idx = S.nview - 1;
    if (S.sel_idx < 0) S.sel_idx = 0;
    S.sel_pid = S.nview ? S.cur[S.view[S.sel_idx]].pid : 0;
    return 1;
}

/* ------------------------------------------------------------------- main */

static void init_colors(void) {
    if (!has_colors()) return;
    start_color();
    use_default_colors();
    init_pair(C_TITLE, COLOR_WHITE, COLOR_BLUE);
    init_pair(C_TABSEL, COLOR_BLUE, COLOR_WHITE);
    init_pair(C_COLHDR, COLOR_BLACK, COLOR_CYAN);
    init_pair(C_SEL, COLOR_BLACK, COLOR_WHITE);
    init_pair(C_OK, COLOR_GREEN, -1);
    init_pair(C_WARN, COLOR_YELLOW, -1);
    init_pair(C_BAD, COLOR_RED, -1);
    init_pair(C_DIM, COLOR_BLUE, -1);
    init_pair(C_ACCENT, COLOR_CYAN, -1);
}

static void usage(const char *argv0) {
    fprintf(stderr,
            "taskmgr %s - Windows-style task manager for the Linux terminal\n"
            "usage: %s [-a] [-d ms] [-v]\n"
            "  -a      ASCII only (no unicode bars)\n"
            "  -d ms   refresh interval in milliseconds (default 1000)\n"
            "  -v      print version and exit\n", TASKMGR_VERSION, argv0);
    exit(2);
}

int main(int argc, char **argv) {
    setlocale(LC_ALL, "");
    S.proc_root = getenv("TASKMGR_PROC") ? getenv("TASKMGR_PROC") : "/proc";
    S.sys_root = getenv("TASKMGR_SYS") ? getenv("TASKMGR_SYS") : "/sys";
    S.pagesize = sysconf(_SC_PAGESIZE);
    S.interval_ms = 1000;
    S.sort = SORT_CPU;
    const char *cs = nl_langinfo(CODESET);
    S.ascii = !(cs && strstr(cs, "UTF-8"));

    int opt;
    while ((opt = getopt(argc, argv, "ad:hv")) != -1) {
        if (opt == 'a') S.ascii = 1;
        else if (opt == 'v') { printf("taskmgr %s\n", TASKMGR_VERSION); return 0; }
        else if (opt == 'd') { S.interval_ms = atoi(optarg); if (S.interval_ms < 100) S.interval_ms = 100; }
        else usage(argv[0]);
    }

    gpu_init();
    read_cpu();           /* S.ncpu is needed by spec_cpu's fallbacks */
    spec_init();

    /* two samples ~200ms apart so the first frame already has CPU% */
    sample();
    struct timespec ts = {0, 200 * 1000000L};
    nanosleep(&ts, NULL);

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    init_colors();
    timeout(S.interval_ms);

    sample();
    build_view();
    draw();
    for (;;) {
        int ch = getch();
        if (ch == ERR) {              /* timer tick */
            sample();
            build_view();
        } else if (ch == KEY_RESIZE) {
            /* just redraw */
        } else {
            if (!handle_key(ch)) break;
            build_view();
        }
        draw();
    }
    endwin();
    if (S.nvml) dlclose(S.nvml);
    free(S.cur); free(S.prev); free(S.view);
    return 0;
}
