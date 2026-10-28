#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <linux/perf_event.h>
#include <math.h>
#include <pwd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#define ROWS 64
#define COLS 512
#define SAMPLE_MS 250
#define KEY_W 6
#define SPACING 2
#define PADDING 1
#define MARGIN_X 2
#define MARGIN_Y 1
#define MAX_ITEMS 24
#define MAX_ENGINES 16

enum { C_DEF, C_ACC, C_SEP };

static const char *logo[] = {
    "⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀",
    "⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⢀⣦",
    "⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⣼⣿⣆",
    "⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⣸⣿⣿⣿⡄",
    "⠀⠀⠀⠀⠀⠀⠀⠀⠀⢰⣿⣿⣿⣿⣿⡄",
    "⠀⠀⠀⠀⠀⠀⠀⠀⢠⣦⣝⢿⣿⣿⣿⣿⡄",
    "⠀⠀⠀⠀⠀⠀⠀⢠⣿⣿⣿⣿⣿⣿⣿⣿⣿⡀",
    "⠀⠀⠀⠀⠀⠀⢠⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣷⡀",
    "⠀⠀⠀⠀⠀⢠⣿⣿⣿⣿⣿⠟⠛⢿⣿⣿⣿⣿⣷⡀",
    "⠀⠀⠀⠀⣠⣿⣿⣿⣿⣿⠃⠀⠀⠀⠹⣿⣿⣿⣿⣿⡄",
    "⠀⠀⠀⣰⣿⣿⣿⣿⣿⡇⠀⠀⠀⠀⠀⣿⣿⣿⣿⣟⠿⡄",
    "⠀⠀⣰⣿⣿⣿⣿⣿⡿⠇⠀⠀⠀⠀⠀⠿⣿⣿⣿⣿⣷⣦⡀",
    "⠀⣰⣿⡿⠟⠛⠉⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠈⠉⠛⠿⣿⣿⡄",
    "⠰⠛⠁⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠉⠛⠄",
    "⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀",
};

static const char *shells[] = {"sh", "su", "nu", "bash", "fish", "dash", "tcsh", "zsh", "ksh", "csh"};

typedef struct {
    char s[5];
    unsigned char col, used, cont;
} Cell;

typedef struct {
    const char *key;
    char val[512];
    size_t orig_w;
} Item;

typedef struct {
    int fd, n;
    uint64_t t, v[MAX_ENGINES];
} Pmu;

static Cell grid[ROWS][COLS];
static Item items[MAX_ITEMS];
static int nitems;

static ssize_t slurp(const char *path, char *buf, size_t cap)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    ssize_t n, t = 0;
    if (fd < 0)
        return -1;
    while ((size_t)t + 1 < cap && (n = read(fd, buf + t, cap - 1 - t)) > 0)
        t += n;
    close(fd);
    buf[t] = 0;
    return t;
}

static int read_line(const char *path, char *buf, size_t cap)
{
    if (slurp(path, buf, cap) < 0)
        return -1;
    buf[strcspn(buf, "\n")] = 0;
    return 0;
}

static char *slurp_all(const char *path, size_t *len)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    struct stat st;
    char *p;
    ssize_t n;
    size_t t = 0;
    if (fd < 0)
        return NULL;
    if (fstat(fd, &st) || !(p = malloc(st.st_size + 1))) {
        close(fd);
        return NULL;
    }
    while (t < (size_t)st.st_size && (n = read(fd, p + t, st.st_size - t)) > 0)
        t += n;
    close(fd);
    p[t] = 0;
    *len = t;
    return p;
}

static void trim(char *s)
{
    size_t n = strlen(s), i = 0;
    while (n && isspace((unsigned char)s[n - 1]))
        s[--n] = 0;
    while (isspace((unsigned char)s[i]))
        i++;
    memmove(s, s + i, n - i + 1);
}

static void replace_all(char *s, size_t cap, const char *from, const char *to)
{
    char tmp[1024], *p;
    size_t fl = strlen(from), tl = strlen(to);
    while ((p = strstr(s, from)) && strlen(s) - fl + tl < cap && strlen(s) < sizeof tmp) {
        strcpy(tmp, p + fl);
        memcpy(p, to, tl);
        strcpy(p + tl, tmp);
        s = p + tl;
    }
}

static unsigned utf8_dec(const char *s, int *len)
{
    const unsigned char *u = (const unsigned char *)s;
    if (u[0] < 0x80) {
        *len = 1;
        return u[0];
    }
    if ((u[0] & 0xE0) == 0xC0 && u[1]) {
        *len = 2;
        return ((u[0] & 0x1F) << 6) | (u[1] & 0x3F);
    }
    if ((u[0] & 0xF0) == 0xE0 && u[1] && u[2]) {
        *len = 3;
        return ((u[0] & 0x0F) << 12) | ((u[1] & 0x3F) << 6) | (u[2] & 0x3F);
    }
    if ((u[0] & 0xF8) == 0xF0 && u[1] && u[2] && u[3]) {
        *len = 4;
        return ((u[0] & 0x07) << 18) | ((u[1] & 0x3F) << 12) | ((u[2] & 0x3F) << 6) | (u[3] & 0x3F);
    }
    *len = 1;
    return 0xFFFD;
}

static int cp_width(unsigned c)
{
    if (c < 0x20 || (c >= 0x7F && c < 0xA0))
        return 0;
    if ((c >= 0x300 && c <= 0x36F) || (c >= 0x200B && c <= 0x200F) || (c >= 0xFE00 && c <= 0xFE0F))
        return 0;
    if ((c >= 0x1100 && c <= 0x115F) || (c >= 0x2E80 && c <= 0xA4CF && c != 0x303F) ||
        (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFE30 && c <= 0xFE4F) ||
        (c >= 0xFF00 && c <= 0xFF60) || (c >= 0xFFE0 && c <= 0xFFE6) || (c >= 0x1F300 && c <= 0x1FAFF) ||
        (c >= 0x20000 && c <= 0x3FFFD))
        return 2;
    return 1;
}

static size_t str_width(const char *s)
{
    size_t w = 0;
    int l;
    while (*s) {
        w += cp_width(utf8_dec(s, &l));
        s += l;
    }
    return w;
}

static int put(int y, int x, const char *s, int col)
{
    int l, w;
    unsigned c;
    while (*s && y < ROWS) {
        c = utf8_dec(s, &l);
        w = cp_width(c);
        if (w && x + w <= COLS) {
            memcpy(grid[y][x].s, s, l);
            grid[y][x].s[l] = 0;
            grid[y][x].col = col;
            grid[y][x].used = 1;
            grid[y][x].cont = 0;
            if (w == 2) {
                grid[y][x + 1].s[0] = 0;
                grid[y][x + 1].used = 1;
                grid[y][x + 1].cont = 1;
            }
        }
        x += w;
        s += l;
    }
    return x;
}

static Item *add(const char *key)
{
    Item *it = &items[nitems++];
    it->key = key;
    it->val[0] = 0;
    it->orig_w = 0;
    return it;
}

static int cpu_times(unsigned long long *idle, unsigned long long *total)
{
    char buf[512];
    unsigned long long f[8];
    if (slurp("/proc/stat", buf, sizeof buf) < 0)
        return -1;
    if (sscanf(buf, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &f[0], &f[1], &f[2], &f[3], &f[4], &f[5], &f[6],
               &f[7]) != 8)
        return -1;
    *idle = f[3] + f[4];
    *total = 0;
    for (int i = 0; i < 8; i++)
        *total += f[i];
    return 0;
}

static int cpu_temp(void)
{
    DIR *d = opendir("/sys/class/hwmon");
    struct dirent *e;
    char path[512], buf[64];
    int r = -1;
    if (!d)
        return -1;
    while (r < 0 && (e = readdir(d))) {
        if (strncmp(e->d_name, "hwmon", 5))
            continue;
        snprintf(path, sizeof path, "/sys/class/hwmon/%s/name", e->d_name);
        if (read_line(path, buf, sizeof buf) || strcmp(buf, "coretemp"))
            continue;
        snprintf(path, sizeof path, "/sys/class/hwmon/%s/temp1_input", e->d_name);
        if (!read_line(path, buf, sizeof buf) && isdigit((unsigned char)buf[0]))
            r = (int)((atol(buf) + 500) / 1000);
    }
    closedir(d);
    return r;
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static int pmu_open(Pmu *p, const char *name)
{
    char path[512], buf[128];
    DIR *d;
    struct dirent *e;
    int type, cpu = 0, fd;
    unsigned long long cfg;
    size_t l;
    p->fd = -1;
    p->n = 0;
    snprintf(path, sizeof path, "/sys/bus/event_source/devices/%s/type", name);
    if (read_line(path, buf, sizeof buf))
        return -1;
    type = atoi(buf);
    snprintf(path, sizeof path, "/sys/bus/event_source/devices/%s/cpumask", name);
    if (!read_line(path, buf, sizeof buf))
        cpu = atoi(buf);
    snprintf(path, sizeof path, "/sys/bus/event_source/devices/%s/events", name);
    if (!(d = opendir(path)))
        return -1;
    while ((e = readdir(d)) && p->n < MAX_ENGINES) {
        struct perf_event_attr a;
        l = strlen(e->d_name);
        if (l < 6 || strcmp(e->d_name + l - 5, "-busy"))
            continue;
        snprintf(path, sizeof path, "/sys/bus/event_source/devices/%s/events/%s", name, e->d_name);
        if (read_line(path, buf, sizeof buf) || sscanf(buf, "config=%llx", &cfg) != 1)
            continue;
        memset(&a, 0, sizeof a);
        a.type = type;
        a.size = sizeof a;
        a.config = cfg;
        a.read_format = PERF_FORMAT_GROUP | PERF_FORMAT_TOTAL_TIME_ENABLED;
        fd = syscall(SYS_perf_event_open, &a, -1, cpu, p->fd, PERF_FLAG_FD_CLOEXEC);
        if (fd < 0)
            continue;
        if (p->fd < 0)
            p->fd = fd;
        p->n++;
    }
    closedir(d);
    return p->fd < 0 ? -1 : 0;
}

static int pmu_read(Pmu *p)
{
    uint64_t buf[2 + MAX_ENGINES];
    ssize_t r = read(p->fd, buf, sizeof buf);
    if (r < (ssize_t)(2 * sizeof(uint64_t)) || (int)buf[0] != p->n)
        return -1;
    p->t = buf[1];
    for (int i = 0; i < p->n; i++)
        p->v[i] = buf[2 + i];
    return 0;
}

static int pmu_busy(Pmu *a, Pmu *b)
{
    double best = 0, u;
    if (b->t <= a->t)
        return -1;
    for (int i = 0; i < a->n; i++) {
        u = 100.0 * (double)(b->v[i] - a->v[i]) / (double)(b->t - a->t);
        if (u > best)
            best = u;
    }
    if (best > 100)
        best = 100;
    return (int)(best + 0.5);
}

static void bytes_fmt(char *out, size_t cap, uint64_t b)
{
    double s = (double)b, div = 1;
    size_t e;
    if (b < 1000) {
        snprintf(out, cap, "%llu B", (unsigned long long)b);
        return;
    }
    e = (size_t)(log(s) / 6.931471806);
    if (e == 0)
        e = 1;
    if (e > 6)
        e = 6;
    for (size_t i = 0; i < e; i++)
        div *= 1000;
    snprintf(out, cap, "%.1f %cB", s / div, "KMGTPE"[e - 1]);
}

static void get_distro(void)
{
    char buf[8192], name[256] = "", ver[256] = "", vid[256] = "", *line, *save, *v;
    size_t l;
    if (slurp("/etc/os-release", buf, sizeof buf) < 0)
        return;
    for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *dst = NULL;
        trim(line);
        if (!strncmp(line, "NAME=", 5))
            dst = name, v = line + 5;
        else if (!strncmp(line, "VERSION=", 8))
            dst = ver, v = line + 8;
        else if (!strncmp(line, "VERSION_ID=", 11))
            dst = vid, v = line + 11;
        if (!dst)
            continue;
        trim(v);
        l = strlen(v);
        if (l >= 2 && ((v[0] == '"' && v[l - 1] == '"') || (v[0] == '\'' && v[l - 1] == '\''))) {
            v[l - 1] = 0;
            v++;
        }
        snprintf(dst, 256, "%s", v);
    }
    Item *it = add("OS");
    if (*ver)
        snprintf(it->val, sizeof it->val, "%s %s", name, ver);
    else if (*vid)
        snprintf(it->val, sizeof it->val, "%s %s", name, vid);
    else
        snprintf(it->val, sizeof it->val, "%s", name);
}

static void get_uptime(void)
{
    struct sysinfo si;
    float u, d, h, m;
    char *p;
    Item *it;
    if (sysinfo(&si))
        return;
    it = add("Up");
    p = it->val;
    u = (float)si.uptime;
    if (u > 60.0f) {
        d = floorf(u / 60.0f / 60.0f / 24.0f);
        h = floorf(fmodf(u / 60.0f / 60.0f, 24.0f));
        m = floorf(fmodf(u / 60.0f, 60.0f));
        if (d != 0.0f)
            p += sprintf(p, "%.0fd ", d);
        if (h != 0.0f)
            p += sprintf(p, "%.0fh ", h);
        if (m != 0.0f)
            sprintf(p, "%.0fm", m);
    } else {
        d = floorf(fmodf(u, 60.0f));
        if (d != 0.0f)
            sprintf(p, "%.0fs", d);
    }
    trim(it->val);
}

static long count_dir(const char *path, const char *ext, const char *skip)
{
    DIR *d = opendir(path);
    struct dirent *e;
    long n = 0;
    const char *dot;
    if (!d)
        return -1;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        if (skip && !strcmp(e->d_name, skip))
            continue;
        if (ext) {
            dot = strrchr(e->d_name, '.');
            if (!dot || dot == e->d_name || strcmp(dot + 1, ext))
                continue;
        }
        n++;
    }
    closedir(d);
    return n;
}

static int is_dir(const char *path)
{
    struct stat st;
    return !stat(path, &st) && S_ISDIR(st.st_mode);
}

static long count_depth2(const char *path, int filter)
{
    DIR *d = opendir(path);
    struct dirent *e;
    char sub[4096];
    long n = 0, c;
    if (!d)
        return -1;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
        if (filter && (strstr(sub + 1, ".Locale") || strstr(sub + 1, ".Debug")))
            continue;
        if (filter) {
            DIR *d2 = opendir(sub);
            struct dirent *e2;
            char sub2[8192];
            if (!d2)
                continue;
            while ((e2 = readdir(d2))) {
                if (!strcmp(e2->d_name, ".") || !strcmp(e2->d_name, ".."))
                    continue;
                snprintf(sub2, sizeof sub2, "%s/%s", sub, e2->d_name);
                if ((c = count_dir(sub2, NULL, NULL)) > 0)
                    n += c;
            }
            closedir(d2);
        } else if ((c = count_dir(sub, NULL, NULL)) > 0) {
            n += c;
        }
    }
    closedir(d);
    return n;
}

static void pkg_add(char *out, size_t cap, long n, const char *pm)
{
    size_t l = strlen(out);
    if (n < 0)
        return;
    snprintf(out + l, cap - l, "%s%ld (%s)", l ? ", " : "", n, pm);
}

static long count_apk(void)
{
    size_t len, i;
    long n = 0;
    char *p = slurp_all("/lib/apk/db/installed", &len);
    if (!p)
        return -1;
    for (i = 0; i < len; i++)
        if (p[i] == '\n' && (i == 0 || p[i - 1] == '\n'))
            n++;
    free(p);
    return n;
}

static long count_flatpak(const char *home)
{
    const char *roots[2] = {"/var/lib/flatpak", NULL};
    const char *dirs[2] = {"app", "runtime"};
    char user[4096], path[4096];
    long total = 0, c;
    snprintf(user, sizeof user, "%s/.local/share/flatpak", home);
    roots[1] = user;
    for (int r = 0; r < 2; r++) {
        if (access(roots[r], F_OK))
            continue;
        for (int i = 0; i < 2; i++) {
            snprintf(path, sizeof path, "%s/%s", roots[r], dirs[i]);
            if ((c = count_depth2(path, 1)) > 0)
                total += c;
        }
    }
    return total > 0 ? total : -1;
}

static void get_packages(void)
{
    char out[512] = "", path[4096];
    const char *home = getenv("HOME"), *ch = getenv("CARGO_HOME");
    struct passwd *pw;
    long n;
    if (!home && (pw = getpwuid(geteuid())))
        home = pw->pw_dir;
    if (!home)
        home = "";
    if (is_dir("/var/lib/pacman/local") && (n = count_dir("/var/lib/pacman/local", NULL, NULL)) > 0)
        pkg_add(out, sizeof out, n - 1, "pacman");
    pkg_add(out, sizeof out, count_dir("/var/lib/dpkg/info", "list", NULL), "dpkg");
    if (!access("/var/db/pkg", F_OK))
        pkg_add(out, sizeof out, count_depth2("/var/db/pkg", 0), "portage");
    if (ch && *ch)
        snprintf(path, sizeof path, "%s/bin", ch);
    else
        snprintf(path, sizeof path, "%s/.cargo/bin", home);
    if ((n = count_dir(path, NULL, NULL)) > 0)
        pkg_add(out, sizeof out, n, "cargo");
    if (is_dir("/var/lib/eopkg/package"))
        pkg_add(out, sizeof out, count_dir("/var/lib/eopkg/package", NULL, NULL), "eopkg");
    pkg_add(out, sizeof out, count_apk(), "apk");
    pkg_add(out, sizeof out, count_flatpak(home), "flatpak");
    pkg_add(out, sizeof out, count_dir("/var/lib/snapd/snaps", "snap", NULL), "snap");
    snprintf(path, sizeof path, "%s/.linuxbrew", home);
    if (!is_dir(path))
        snprintf(path, sizeof path, "/home/linuxbrew/.linuxbrew");
    if (is_dir(path)) {
        strncat(path, "/Cellar", sizeof path - strlen(path) - 1);
        pkg_add(out, sizeof out, count_dir(path, NULL, ".keepme"), "Homebrew");
    }
    if (*out)
        snprintf(add("Pkgs")->val, sizeof items[0].val, "%s", out);
}

static void get_shell(void)
{
    struct passwd *pw = getpwuid(geteuid());
    const char *s;
    if (!pw || !pw->pw_shell)
        return;
    s = strrchr(pw->pw_shell, '/');
    s = s ? s + 1 : pw->pw_shell;
    if (*s)
        snprintf(add("Shell")->val, sizeof items[0].val, "%s", s);
}

static void ucfirst(char *s)
{
    if (*s >= 'a' && *s <= 'z')
        *s -= 32;
}

static int wayland_wm(char *out, size_t cap)
{
    const char *disp = getenv("WAYLAND_DISPLAY"), *rt = getenv("XDG_RUNTIME_DIR"), *sockenv = getenv("WAYLAND_SOCKET");
    struct sockaddr_un sa = {.sun_family = AF_UNIX};
    struct ucred cr;
    socklen_t cl = sizeof cr;
    char path[64];
    int fd = -1, own = 1, r;
    if (sockenv && *sockenv) {
        fd = atoi(sockenv);
        own = 0;
    } else {
        if (!disp || !*disp)
            disp = "wayland-0";
        if (disp[0] == '/')
            r = snprintf(sa.sun_path, sizeof sa.sun_path, "%s", disp);
        else if (rt && *rt)
            r = snprintf(sa.sun_path, sizeof sa.sun_path, "%s/%s", rt, disp);
        else
            return -1;
        if (r < 0 || (size_t)r >= sizeof sa.sun_path)
            return -1;
        fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0)
            return -1;
        if (connect(fd, (struct sockaddr *)&sa, sizeof sa)) {
            close(fd);
            return -1;
        }
    }
    r = getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &cl);
    if (own)
        close(fd);
    if (r || cr.pid <= 0)
        return -1;
    snprintf(path, sizeof path, "/proc/%d/comm", cr.pid);
    return read_line(path, out, cap);
}

static void get_wm(void)
{
    const char *st = getenv("XDG_SESSION_TYPE");
    char sess[128], wm[256];
    if (!st)
        return;
    snprintf(sess, sizeof sess, "%s", st);
    ucfirst(sess);
    if (strcmp(sess, "Wayland") || wayland_wm(wm, sizeof wm))
        return;
    snprintf(add("WM")->val, sizeof items[0].val, "%s (%s)", wm, sess);
}

static int parent_of(int pid)
{
    char path[64], buf[4096], *p;
    snprintf(path, sizeof path, "/proc/%d/status", pid);
    if (slurp(path, buf, sizeof buf) < 0)
        return -1;
    for (p = buf; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL)
        if (!strncasecmp(p, "ppid", 4)) {
            long v = 0;
            int any = 0;
            for (; *p && *p != '\n'; p++)
                if (isdigit((unsigned char)*p)) {
                    v = v * 10 + (*p - '0');
                    any = 1;
                }
            return any ? (int)v : -1;
        }
    return -1;
}

static int is_shell(const char *s)
{
    for (size_t i = 0; i < sizeof shells / sizeof *shells; i++)
        if (!strcmp(s, shells[i]))
            return 1;
    return 0;
}

static void get_terminal(void)
{
    char path[64], name[256];
    int pid = getppid();
    snprintf(path, sizeof path, "/proc/%d/comm", pid);
    if (read_line(path, name, sizeof name))
        return;
    for (int guard = 0; is_shell(name) && guard < 64; guard++) {
        pid = parent_of(pid);
        if (pid <= 0)
            break;
        snprintf(path, sizeof path, "/proc/%d/comm", pid);
        read_line(path, name, sizeof name);
    }
    if (*name)
        snprintf(add("Term")->val, sizeof items[0].val, "%s", name);
}

static void get_cpu(int usage)
{
    char buf[1 << 16], model[512] = "", *line, *save;
    Item *it;
    if (slurp("/proc/cpuinfo", buf, sizeof buf) >= 0)
        for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
            if (!strncmp(line, "model name", 10)) {
                snprintf(model, sizeof model, "%s", line);
                replace_all(model, sizeof model, "model name", "");
                replace_all(model, sizeof model, ":", "");
                trim(model);
                break;
            }
    replace_all(model, sizeof model, "(TM)", "™");
    replace_all(model, sizeof model, "(R)", "®");
    it = add("CPU");
    snprintf(it->val, sizeof it->val, "%s (%ld)", model, sysconf(_SC_NPROCESSORS_CONF));
    it->orig_w = str_width(it->val);
    if (usage >= 0) {
        size_t l = strlen(it->val);
        snprintf(it->val + l, sizeof it->val - l, " (%d%%)", usage);
    }
}

static const char *find_line(const char *db, const char *end, const char *prefix)
{
    size_t pl = strlen(prefix);
    const char *p = db;
    while (p < end) {
        if ((size_t)(end - p) >= pl && !memcmp(p, prefix, pl))
            return p;
        p = memchr(p, '\n', end - p);
        if (!p)
            break;
        p++;
    }
    return NULL;
}

static void line_name(const char *p, size_t skip, char *out, size_t cap)
{
    const char *e = strchr(p, '\n');
    size_t n;
    p += skip;
    if (!e)
        e = p + strlen(p);
    n = e - p;
    if (n >= cap)
        n = cap - 1;
    memcpy(out, p, n);
    out[n] = 0;
    trim(out);
}

static int pci_hex(const char *dev, const char *f, char *out)
{
    char path[512], buf[32];
    snprintf(path, sizeof path, "/sys/bus/pci/devices/%.256s/%s", dev, f);
    if (read_line(path, buf, sizeof buf) || strlen(buf) < 3)
        return -1;
    snprintf(out, 32, "%s", buf + 2);
    return 0;
}

static int gpu_name(const char *db, size_t dblen, const char *dev, char *out, size_t cap)
{
    const char *end = db + dblen, *vp, *p, *next, *q;
    char cls[32], ven[32], did[32], sven[32], sdev[32], key[160], devname[512], sub[512];
    size_t l;
    if (pci_hex(dev, "class", cls) || pci_hex(dev, "vendor", ven) || pci_hex(dev, "device", did) ||
        pci_hex(dev, "subsystem_vendor", sven) || pci_hex(dev, "subsystem_device", sdev))
        return -1;
    cls[2] = 0;
    snprintf(key, sizeof key, "\nC %s  ", cls);
    if (!(p = strstr(db, key)))
        return -1;
    line_name(p + 1, strlen(key) - 1, sub, sizeof sub);
    if (strcmp(sub, "Display controller") && strcmp(sub, "VGA compatible controller"))
        return -1;
    snprintf(key, sizeof key, "%s  ", ven);
    if (!(vp = find_line(db, end, key)))
        return -1;
    next = memchr(vp, '\n', end - vp);
    if (!next)
        return -1;
    snprintf(key, sizeof key, "\t%s  ", did);
    for (p = next + 1; p < end && (*p == '\t' || *p == '#'); p = q + 1) {
        q = memchr(p, '\n', end - p);
        if (!q)
            q = end;
        if (!strncmp(p, key, strlen(key)))
            break;
    }
    if (p >= end || strncmp(p, key, strlen(key)))
        return -1;
    line_name(p, strlen(key), devname, sizeof devname);
    snprintf(key, sizeof key, "\t\t%s %s  ", sven, sdev);
    for (q = memchr(p, '\n', end - p), p = q ? q + 1 : end; p < end && (!strncmp(p, "\t\t", 2) || *p == '#');
         p = q + 1) {
        q = memchr(p, '\n', end - p);
        if (!q)
            q = end;
        if (!strncmp(p, key, strlen(key))) {
            const char *b;
            line_name(p, strlen(key), sub, sizeof sub);
            b = strchr(sub, '[');
            l = strlen(sub);
            if (!b) {
                snprintf(out, cap, "%s", devname);
                return 0;
            }
            if ((size_t)(b + 1 - sub) <= l - 1)
                snprintf(out, cap, "%.*s", (int)(l - 1 - (b + 1 - sub)), b + 1);
            else
                out[0] = 0;
            return 0;
        }
    }
    snprintf(out, cap, "%s", devname);
    return 0;
}

typedef struct {
    char dev[256];
    char name[512];
    Pmu a;
    int has_pmu;
} Gpu;

static Gpu gpus[8];
static int ngpus;

static void scan_gpus(void)
{
    static const char *paths[] = {"/usr/share/hwdata/pci.ids", "/usr/share/misc/pci.ids"};
    char *db = NULL, link[512], path[512], pmu[320];
    size_t len = 0;
    DIR *d;
    struct dirent *e;
    ssize_t r;
    for (size_t i = 0; i < 2 && !db; i++)
        db = slurp_all(paths[i], &len);
    if (!db)
        return;
    if ((d = opendir("/sys/bus/pci/devices"))) {
        while ((e = readdir(d)) && ngpus < 8) {
            Gpu *g = &gpus[ngpus];
            if (e->d_name[0] == '.')
                continue;
            if (gpu_name(db, len, e->d_name, g->name, sizeof g->name))
                continue;
            snprintf(g->dev, sizeof g->dev, "%s", e->d_name);
            g->has_pmu = 0;
            snprintf(path, sizeof path, "/sys/bus/pci/devices/%s/driver", e->d_name);
            r = readlink(path, link, sizeof link - 1);
            if (r > 0) {
                link[r] = 0;
                const char *drv = strrchr(link, '/');
                drv = drv ? drv + 1 : link;
                if (!strcmp(drv, "i915")) {
                    snprintf(pmu, sizeof pmu, "i915_%s", e->d_name);
                    for (char *c = pmu; *c; c++)
                        if (*c == ':')
                            *c = '_';
                    if (!pmu_open(&g->a, pmu) || (!strcmp(e->d_name, "0000:00:02.0") && !pmu_open(&g->a, "i915")))
                        g->has_pmu = !pmu_read(&g->a);
                }
            }
            ngpus++;
        }
        closedir(d);
    }
    free(db);
}

static void get_memory(void)
{
    struct sysinfo si;
    char buf[8192], *p, a[32], b[32];
    uint64_t total, avail = 0, used;
    Item *it;
    if (sysinfo(&si))
        return;
    total = (uint64_t)si.totalram * si.mem_unit / 1024;
    if (slurp("/proc/meminfo", buf, sizeof buf) >= 0)
        for (p = buf; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL)
            if (!strncmp(p, "MemAvailable", 12)) {
                for (; *p && *p != '\n'; p++)
                    if (isdigit((unsigned char)*p))
                        avail = avail * 10 + (*p - '0');
                break;
            }
    if (avail)
        used = total - avail;
    else
        used = total - (uint64_t)si.freeram * si.mem_unit / 1024 - (uint64_t)si.bufferram * si.mem_unit / 1024;
    bytes_fmt(a, sizeof a, used * 1000);
    bytes_fmt(b, sizeof b, total * 1000);
    it = add("RAM");
    snprintf(it->val, sizeof it->val, "%s / %s (%d%%)", a, b, (int)ceil((double)used / (double)total * 100.0));
}

static void get_disk(void)
{
    struct statfs s;
    uint64_t total, used;
    char a[32], b[32];
    if (statfs("/", &s))
        return;
    total = (uint64_t)s.f_blocks * s.f_bsize;
    used = total - (uint64_t)s.f_bavail * s.f_bsize;
    bytes_fmt(a, sizeof a, used);
    bytes_fmt(b, sizeof b, total);
    snprintf(add("Disk")->val, sizeof items[0].val, "%s / %s (/) (%d%%)", a, b,
             (int)ceil((double)used / (double)total * 100.0));
}

static int term_width(void)
{
    struct winsize ws;
    int fd = open("/dev/tty", O_RDONLY | O_CLOEXEC), ok;
    ok = !ioctl(fd >= 0 ? fd : STDOUT_FILENO, TIOCGWINSZ, &ws);
    if (fd >= 0)
        close(fd);
    return ok && ws.ws_col ? ws.ws_col : COLS;
}

static void out_color(FILE *o, int c)
{
    if (c == C_ACC)
        fputs("\x1b[38;2;125;174;163;49m", o);
    else if (c == C_SEP)
        fputs("\x1b[38;2;102;92;84;49m", o);
    else
        fputs("\x1b[39;49m", o);
}

int main(void)
{
    unsigned long long idle0 = 0, total0 = 0, idle1, total1;
    int have_cpu, cpu = -1, temp;
    double t0 = now_ms(), rem;
    struct utsname un;
    struct passwd *pw;
    char host[256] = "", title[512];
    size_t nlogo = sizeof logo / sizeof *logo, logo_w = 0, orig_max = 0, fin_max = 0, inner, inner_orig, w;
    int x0, by, bh, last_y, row_end_box, row_end_other, tw, y, x;
    FILE *o = stdout;

    have_cpu = !cpu_times(&idle0, &total0);
    scan_gpus();

    get_distro();
    uname(&un);
    temp = cpu_temp();
    if (temp >= 0) {
        Item *it = add("Temp  ");
        snprintf(it->val, sizeof it->val, "%d°C", temp);
        it->orig_w = str_width(un.release);
    } else {
        snprintf(add("Kernel")->val, sizeof items[0].val, "%s", un.release);
    }
    get_uptime();
    get_packages();
    get_shell();
    get_wm();
    get_terminal();

    rem = SAMPLE_MS - (now_ms() - t0);
    if (rem > 0) {
        struct timespec ts = {(time_t)(rem / 1000), (long)((rem - (long)(rem / 1000) * 1000) * 1e6)};
        nanosleep(&ts, NULL);
    }
    if (have_cpu && !cpu_times(&idle1, &total1) && total1 > total0)
        cpu = (int)(100.0 * (1.0 - (double)(idle1 - idle0) / (double)(total1 - total0)) + 0.5);
    get_cpu(cpu);
    for (int i = 0; i < ngpus && nitems < MAX_ITEMS - 2; i++) {
        Item *it = add("GPU");
        Pmu b = gpus[i].a;
        int busy = -1;
        snprintf(it->val, sizeof it->val, "%s", gpus[i].name);
        it->orig_w = str_width(it->val);
        if (gpus[i].has_pmu && !pmu_read(&b))
            busy = pmu_busy(&gpus[i].a, &b);
        if (busy >= 0) {
            size_t l = strlen(it->val);
            snprintf(it->val + l, sizeof it->val - l, " (%d%%)", busy);
        }
    }
    get_memory();
    get_disk();

    for (int i = 0; i < nitems; i++) {
        size_t fw = str_width(items[i].val);
        size_t ow = items[i].orig_w ? items[i].orig_w : fw;
        if (fw > fin_max)
            fin_max = fw;
        if (ow > orig_max)
            orig_max = ow;
    }

    for (size_t i = 0; i < nlogo; i++)
        if ((w = str_width(logo[i])) > logo_w)
            logo_w = w;
    for (size_t i = 0; i < nlogo; i++)
        put(1 + i, 1, logo[i], C_ACC);

    x0 = 1 + logo_w + 2;
    by = 1;
    inner_orig = MARGIN_X * 2 + PADDING + KEY_W + SPACING + 1 + SPACING + orig_max;
    inner = MARGIN_X * 2 + PADDING + KEY_W + SPACING + 1 + SPACING + fin_max;
    if (inner < inner_orig)
        inner = inner_orig;
    bh = nitems + 2 + MARGIN_Y * 2;
    if (x0 + inner + 2 > COLS)
        inner = COLS - x0 - 2;

    pw = getpwuid(geteuid());
    read_line("/proc/sys/kernel/hostname", host, sizeof host);
    if (!*host)
        snprintf(host, sizeof host, "%s", un.nodename);
    snprintf(title, sizeof title, " %s@%s ", pw ? pw->pw_name : "", host);

    for (y = by; y < by + bh; y++) {
        const char *l = y == by ? "╭" : y == by + bh - 1 ? "╰" : "│";
        const char *r = y == by ? "╮" : y == by + bh - 1 ? "╯" : "│";
        put(y, x0, l, C_DEF);
        put(y, x0 + inner + 1, r, C_DEF);
        if (y == by || y == by + bh - 1)
            for (x = x0 + 1; x <= x0 + (int)inner; x++)
                put(y, x, "─", C_DEF);
    }
    {
        char t[512];
        int l, xx = x0 + 1;
        const char *s = title;
        while (*s && xx <= x0 + (int)inner) {
            unsigned c = utf8_dec(s, &l);
            if (xx + cp_width(c) > x0 + (int)inner + 1)
                break;
            memcpy(t, s, l);
            t[l] = 0;
            xx = put(by, xx, t, C_DEF);
            s += l;
        }
    }
    for (int i = 0; i < nitems; i++) {
        int ry = by + 1 + MARGIN_Y + i, kx = x0 + 1 + MARGIN_X + PADDING;
        put(ry, kx, items[i].key, C_ACC);
        put(ry, kx + KEY_W + SPACING, "›", C_SEP);
        put(ry, kx + KEY_W + SPACING + 1 + SPACING, items[i].val, C_DEF);
    }

    last_y = by + bh - 1;
    if ((int)nlogo > last_y)
        last_y = nlogo;
    tw = term_width();
    row_end_box = x0 + inner + 2;
    row_end_other = x0 + inner_orig + 2;
    if (row_end_box > tw)
        row_end_box = tw;
    if (row_end_other > tw)
        row_end_other = tw;

    {
        int cur = C_DEF;
        for (y = 0; y <= last_y; y++) {
            int end = (y >= by && y < by + bh) ? row_end_box : row_end_other;
            if (y)
                fputc('\n', o);
            for (x = 0; x < end; x++) {
                Cell *c = &grid[y][x];
                if (c->cont)
                    continue;
                if ((c->used ? c->col : C_DEF) != cur) {
                    cur = c->used ? c->col : C_DEF;
                    out_color(o, cur);
                }
                fputs(c->used ? c->s : " ", o);
            }
        }
    }
    fputs("\x1b[39m\x1b[49m\x1b[0m\n\n", o);
    return 0;
}
