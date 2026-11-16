/*
 * wallinfo.c — desktop wallpaper info overlay for Mac OS X 10.5 (PowerPC)
 *
 * Background job:
 *   1. waits for the network connection to come up
 *   2. copies the current desktop wallpaper (never touching the original)
 *   3. draws a white box into the copy with: hostname, all network
 *      interfaces + their IPs, startup time, logged-in user and the
 *      boot volume
 *   4. activates the copy as the desktop wallpaper
 *
 * The box is offset from the top by the menu bar height (22 px) plus a
 * small inset, so it stays clear of the menu bar.
 *
 * Wallpaper handling:
 *   - The original wallpaper file is read-only to us. We render into
 *     ~/Library/Application Support/wallinfo/wallpaper.<ext> and point the
 *     Dock at that copy. The source file is recorded in the state file so
 *     repeated runs never stack boxes on top of each other.
 *   - Leopard-style prefs ("Desktop" dict with ImageFilePath) are read
 *     directly. If only the Tiger-legacy "Background" dict exists
 *     (ChangePath pointing at a folder of randomly changing pictures), the
 *     first adoption picks a random image from that folder as base.
 *   - When activating the copy, the Leopard-style "Desktop" dict is written
 *     with the random-change options cleared so the Dock keeps showing the
 *     overlay instead of switching pictures again.
 *
 * Build (cross, on dev-machine):  make cross32   -> bin/wallinfo-ppc
 *                                 make cross64   -> bin/wallinfo-ppc64
 *
 * Usage:
 *   wallinfo            daemonize, wait for network, update, then refresh
 *                       every -i seconds while info/wallpaper changed
 *   wallinfo -f         stay in foreground (for launchd)
 *   wallinfo -o         one update and exit
 *   wallinfo -r         restore the recorded original wallpaper and exit
 *   wallinfo -t SEC     network wait timeout (0 = forever, default 300)
 *   wallinfo -i SEC     refresh interval (default 300)
 *   wallinfo -V         print version (arch probe), exit
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <syslog.h>
#include <fcntl.h>
#include <dirent.h>
#include <pwd.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/sysctl.h>
#include <sys/socket.h>
#include <sys/mount.h>
#include <sys/attr.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <mach-o/dyld.h>

#include <CoreFoundation/CoreFoundation.h>
#include <ApplicationServices/ApplicationServices.h>
#include <CoreServices/CoreServices.h>
#include <ImageIO.h> /* sub-umbrella of ApplicationServices, see IMGIO_INC in Makefile */

#define VERSION "1.0.0"

/* --- layout ------------------------------------------------------------ */
/* The box sits in the very top-left corner. The menu bar covers the top
 * 22 px of the screen; TOP_OFFSET = menu bar height + a small inset. */
#define MENUBAR_HEIGHT   22
#define BOX_INSET        8
#define TOP_OFFSET       (MENUBAR_HEIGHT + BOX_INSET)
#define BOX_LEFT_INSET   10   /* left edge starts 10 px into the image */

#define FONT_NAME        "Monaco"
#define FONT_SIZE        12.0f
#define LINE_HEIGHT      16.0f
#define BOX_PAD          8.0f
#define BOX_ALPHA        0.85f

#define MAX_LINES        48
#define BAR_LEN          20     /* ascii fill bar width */
#define LINE_LEN         192
#define MAX_IFACES       16

/* --- behaviour ---------------------------------------------------------- */
#define NET_POLL_SECS    2
#define DEFAULT_NET_WAIT 300     /* seconds, 0 = wait forever */
#define DEFAULT_INTERVAL 300     /* refresh interval */

#define FALLBACK_WALLPAPER "/Library/Desktop Pictures/Aqua Blue.jpg"
#define WALLINFO_APP    CFSTR("com.apple.desktop")

static const char *const IMAGE_EXTS[] = {
    "jpg", "jpeg", "png", "gif", "tiff", "bmp", NULL
};

/* ------------------------------------------------------------------------ */

struct info {
    int  n;
    char lines[MAX_LINES][LINE_LEN];
};

static volatile sig_atomic_t g_run     = 1;
static volatile sig_atomic_t g_refresh = 0;
static int  g_daemonized  = 0;
static int  g_foreground  = 0;
static int  g_oneshot     = 0;
static int  g_restore     = 0;
static int  g_net_wait    = DEFAULT_NET_WAIT;
static int  g_interval    = DEFAULT_INTERVAL;

static char g_statedir[PATH_MAX];
static char g_last_digest[4096];
static char g_last_out[PATH_MAX];
static char g_out_path[PATH_MAX];   /* wallpaper path for set_wallpaper_dict */

/* ------------------------------------------------------------------------ */

static void lg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (g_daemonized) {
        /* LOG_INFO would be dropped by the stock 10.5 syslog.conf
         * ("*.notice" rule); LOG_NOTICE lands in /var/log/system.log */
        vsyslog(LOG_NOTICE, fmt, ap);
    } else {
        vfprintf(stderr, fmt, ap);
        fputc('\n', stderr);
    }
    va_end(ap);
}

static void on_term(int sig) { (void)sig; g_run = 0; }
static void on_hup(int sig)  { (void)sig; g_refresh = 1; }

/* ATS/CoreGraphics is not fork-safe: a forked child that touches fonts
 * SIGBUSes in ATS (see crash 2026-11-16). So daemonize() never continues
 * in the forked child — it re-execs itself with -D, which runs the same
 * loop with syslog logging in a fresh, cleanly initialized process. */
static void daemonize(void)
{
    char path[PATH_MAX];
    uint32_t size = sizeof path;
    pid_t pid = fork();

    if (pid < 0) return;
    if (pid > 0) _exit(0);
    setsid();
    chdir("/");
    umask(022);
    {
        int fd = open("/dev/null", O_RDWR);
        if (fd >= 0) {
            dup2(fd, 0); dup2(fd, 1); dup2(fd, 2);
            if (fd > 2) close(fd);
        }
    }
    if (_NSGetExecutablePath(path, &size) == 0) {
        char *nargv[3];
        nargv[0] = path;
        nargv[1] = (char *)"-D";
        nargv[2] = NULL;
        execv(path, nargv);
        /* exec failed: fall through, syslog from here (CG use may crash) */
    }
    openlog("wallinfo", LOG_PID, LOG_DAEMON);
    g_daemonized = 1;
}

/* ------------------------------------------------------------------------ */
/* info collection                                                           */

static void add_line(struct info *in, const char *fmt, ...)
{
    va_list ap;
    if (in->n >= MAX_LINES) return;
    va_start(ap, fmt);
    vsnprintf(in->lines[in->n], LINE_LEN, fmt, ap);
    va_end(ap);
    in->n++;
}

static int net_up_once(void)
{
    struct ifaddrs *ifa0 = NULL, *ifa;
    int ok = 0;

    if (getifaddrs(&ifa0) != 0) return 0;
    for (ifa = ifa0; ifa && !ok; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) continue;
        if (ifa->ifa_flags & IFF_LOOPBACK) continue;
        if (!(ifa->ifa_flags & IFF_UP)) continue;
        if (ifa->ifa_addr->sa_family == AF_INET) {
            struct sockaddr_in *s = (struct sockaddr_in *)ifa->ifa_addr;
            if (ntohl(s->sin_addr.s_addr) != INADDR_LOOPBACK) ok = 1;
        } else if (ifa->ifa_addr->sa_family == AF_INET6) {
            struct sockaddr_in6 *s = (struct sockaddr_in6 *)ifa->ifa_addr;
            if (!IN6_IS_ADDR_LOOPBACK(&s->sin6_addr) &&
                !IN6_IS_ADDR_LINKLOCAL(&s->sin6_addr)) ok = 1;
        }
    }
    freeifaddrs(ifa0);
    return ok;
}

static void wait_for_network(int timeout)
{
    time_t start = time(NULL);

    if (net_up_once()) { lg("network already up"); return; }
    lg("waiting for network connection...");
    while (!net_up_once()) {
        if (timeout && time(NULL) - start >= timeout) {
            lg("network wait timed out after %ds, continuing anyway", timeout);
            return;
        }
        sleep(NET_POLL_SECS);
    }
    lg("network is up");
}

/* Volume name via getattrlist. The buffer starts with a u_int32_t total
 * length, then the attrreference_t records; attr_dataoffset is relative to
 * the attrreference itself, attr_length includes the NUL. */
static void vol_name(const char *path, char *out, size_t n)
{
    struct attrlist al;
    struct {
        u_int32_t       total;
        attrreference_t name;
        char            namebuf[256];
    } abuf;

    out[0] = '\0';
    memset(&al, 0, sizeof al);
    al.bitmapcount = ATTR_BIT_MAP_COUNT;
    al.volattr = ATTR_VOL_INFO | ATTR_VOL_NAME;
    memset(&abuf, 0, sizeof abuf);
    if (getattrlist(path, &al, &abuf, sizeof abuf, 0) == 0 &&
        abuf.name.attr_length > 0) {
        const char *nm = (const char *)&abuf.name + abuf.name.attr_dataoffset;
        size_t l = abuf.name.attr_length;
        if (l > sizeof abuf.namebuf - 1) l = sizeof abuf.namebuf - 1;
        if (l > n - 1) l = n - 1;
        memcpy(out, nm, l);
        out[l] = '\0';
    }
}

static void fmt_size(unsigned long long b, char *out, size_t n)
{
    if (b >= 1024ULL * 1024 * 1024)
        snprintf(out, n, "%.1fG", (double)b / (1024.0 * 1024 * 1024));
    else
        snprintf(out, n, "%.0fM", (double)b / (1024.0 * 1024));
}

/* "vol  name (device)" + indented ascii fill bar line:
 * "     [#####.....] 42% 10.4G/25.0G" */
static void add_volume(struct info *in, const char *mnt, const char *dev)
{
    struct statfs sfs;
    char volname[256], bar[BAR_LEN + 1], u[32], t[32];
    unsigned long long tot, usd;
    int pct, fill, i;

    if (statfs(mnt, &sfs) != 0) return;
    vol_name(mnt, volname, sizeof volname);
    if (volname[0])
        add_line(in, "vol  %s (%s)", volname, dev);
    else
        add_line(in, "vol  %s on %s", dev, mnt);

    tot = (unsigned long long)sfs.f_blocks * (unsigned long long)sfs.f_bsize;
    usd = (unsigned long long)(sfs.f_blocks - sfs.f_bfree) *
          (unsigned long long)sfs.f_bsize;
    pct = tot ? (int)(usd * 100 / tot) : 0;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    fill = BAR_LEN * pct / 100;
    for (i = 0; i < BAR_LEN; i++) bar[i] = i < fill ? '#' : '.';
    bar[BAR_LEN] = '\0';
    fmt_size(usd, u, sizeof u);
    fmt_size(tot, t, sizeof t);
    add_line(in, "     [%s] %d%% %s/%s", bar, pct, u, t);
}

static void collect_info(struct info *in)
{
    char host[256];
    struct ifaddrs *ifa0 = NULL, *ifa;

    in->n = 0;
    if (gethostname(host, sizeof host) != 0) strcpy(host, "?");
    add_line(in, "%s", host);

    /* startup time (before the volumes, per spec) */
    {
        struct timeval tv;
        size_t len = sizeof tv;
        if (sysctlbyname("kern.boottime", &tv, &len, NULL, 0) == 0 && len == sizeof tv) {
            char b[64];
            strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", localtime(&tv.tv_sec));
            add_line(in, "boot %s", b);
        } else {
            add_line(in, "boot ?");
        }
    }

    /* mounted volumes: boot volume first, then every other local disk,
     * each with an ascii fill bar + used/total (indented like "vol  ") */
    {
        struct statfs root;
        struct statfs *mntbuf;
        int nmnt, i, nvols = 0;

        if (statfs("/", &root) == 0) {
            add_volume(in, "/", root.f_mntfromname);
            nmnt = getmntinfo(&mntbuf, MNT_WAIT);
            for (i = 0; i < nmnt && nvols < 10; i++) {
                if (!(mntbuf[i].f_flags & MNT_LOCAL)) continue;
                if (strncmp(mntbuf[i].f_mntfromname, "/dev/disk", 9) != 0) continue;
                if (strcmp(mntbuf[i].f_mntfromname, root.f_mntfromname) == 0) continue;
                add_volume(in, mntbuf[i].f_mntonname, mntbuf[i].f_mntfromname);
                nvols++;
            }
        }
    }

    /* logged-in user (console owner) */
    {
        struct stat st;
        const char *u = NULL;
        if (stat("/dev/console", &st) == 0) {
            struct passwd *pw = getpwuid(st.st_uid);
            if (pw) u = pw->pw_name;
        }
        if (!u) u = getlogin();
        add_line(in, "user %s", u ? u : "?");
    }

    /* network devices and their IPs (loopback skipped) */
    {
        char names[MAX_IFACES][IFNAMSIZ];
        char v4[MAX_IFACES][64], v6[MAX_IFACES][64];
        int nif = 0, i;

        memset(names, 0, sizeof names);
        memset(v4, 0, sizeof v4);
        memset(v6, 0, sizeof v6);

        if (getifaddrs(&ifa0) == 0) {
            for (ifa = ifa0; ifa; ifa = ifa->ifa_next) {
                int idx = -1;
                if (!ifa->ifa_addr) continue;
                if (ifa->ifa_flags & IFF_LOOPBACK) continue;
                for (i = 0; i < nif; i++)
                    if (strncmp(names[i], ifa->ifa_name, IFNAMSIZ) == 0) { idx = i; break; }
                if (idx < 0 && nif < MAX_IFACES && (ifa->ifa_flags & IFF_UP)) {
                    idx = nif++;
                    strncpy(names[idx], ifa->ifa_name, IFNAMSIZ - 1);
                }
                if (idx < 0) continue;
                if (ifa->ifa_addr->sa_family == AF_INET) {
                    struct sockaddr_in *s = (struct sockaddr_in *)ifa->ifa_addr;
                    if (ntohl(s->sin_addr.s_addr) == INADDR_LOOPBACK) continue;
                    inet_ntop(AF_INET, &s->sin_addr, v4[idx], sizeof v4[idx]);
                } else if (ifa->ifa_addr->sa_family == AF_INET6) {
                    struct sockaddr_in6 *s = (struct sockaddr_in6 *)ifa->ifa_addr;
                    if (IN6_IS_ADDR_LOOPBACK(&s->sin6_addr)) continue;
                    if (IN6_IS_ADDR_LINKLOCAL(&s->sin6_addr)) continue;
                    inet_ntop(AF_INET6, &s->sin6_addr, v6[idx], sizeof v6[idx]);
                }
            }
            freeifaddrs(ifa0);
        }
        for (i = 0; i < nif; i++) {
            if (v4[i][0]) add_line(in, "%s  %s", names[i], v4[i]);
            if (v6[i][0]) add_line(in, "%s  %s", names[i], v6[i]);
        }
        if (nif == 0) add_line(in, "net  (offline)");
    }
}

static void digest_info(const struct info *in, char *out, size_t n)
{
    size_t used = 0;
    int i;
    out[0] = '\0';
    for (i = 0; i < in->n; i++) {
        size_t l = strlen(in->lines[i]);
        if (used + l + 2 >= n) break;
        memcpy(out + used, in->lines[i], l);
        used += l;
        out[used++] = '\n';
    }
    out[used] = '\0';
}

/* ------------------------------------------------------------------------ */
/* wallpaper sources                                                         */

static int has_image_ext(const char *name)
{
    const char *dot = strrchr(name, '.');
    int i;
    if (!dot) return 0;
    for (i = 0; IMAGE_EXTS[i]; i++)
        if (strcasecmp(dot + 1, IMAGE_EXTS[i]) == 0) return 1;
    return 0;
}

static int pick_random_image(const char *dir, char *out, size_t n)
{
    DIR *d = opendir(dir);
    struct dirent *de;
    char pool[MAX_LINES][NAME_MAX + 1];
    int count = 0;

    if (!d) return 0;
    while ((de = readdir(d)) != NULL && count < MAX_LINES) {
        if (de->d_name[0] == '.') continue;
        if (!has_image_ext(de->d_name)) continue;
        strncpy(pool[count], de->d_name, NAME_MAX);
        pool[count][NAME_MAX] = '\0';
        count++;
    }
    closedir(d);
    if (count == 0) return 0;
    snprintf(out, n, "%s/%s", dir, pool[rand() % count]);
    return 1;
}

/* Leopard-style: first ImageFilePath found in the "Desktop" dict
 * (real display keys preferred over "default"). */
static int desktop_image_path(CFPropertyListRef dd, char *out, size_t n)
{
    CFIndex c, i, pass;

    if (!dd || CFGetTypeID(dd) != CFDictionaryGetTypeID()) return 0;
    c = CFDictionaryGetCount((CFDictionaryRef)dd);
    if (c <= 0) return 0;
    {
        const void **keys = malloc(c * sizeof(void *));
        const void **vals = malloc(c * sizeof(void *));
        int found = 0;
        if (!keys || !vals) { free(keys); free(vals); return 0; }
        CFDictionaryGetKeysAndValues((CFDictionaryRef)dd, keys, vals);
        for (pass = 0; pass < 2 && !found; pass++) {
            for (i = 0; i < c && !found; i++) {
                if ((pass == 0) ==
                    (CFStringCompare((CFStringRef)keys[i], CFSTR("default"), 0) == 0))
                    continue; /* pass 0 skips "default", pass 1 only "default" */
                if (!vals[i] || CFGetTypeID(vals[i]) != CFDictionaryGetTypeID())
                    continue;
                {
                    CFStringRef p = CFDictionaryGetValue((CFDictionaryRef)vals[i],
                                                         CFSTR("ImageFilePath"));
                    if (p && CFGetTypeID(p) == CFStringGetTypeID() &&
                        CFStringGetCString(p, out, n, kCFStringEncodingUTF8) &&
                        out[0])
                        found = 1;
                }
            }
        }
        free(keys);
        free(vals);
        return found;
    }
}

/* Tiger-legacy: "Background" dict entries have ChangePath — a folder with
 * randomly changing pictures (or a plain file path). */
static int legacy_background_path(CFPropertyListRef bd, char *out, size_t n)
{
    CFIndex c, i, pass;
    struct stat st;

    if (!bd || CFGetTypeID(bd) != CFDictionaryGetTypeID()) return 0;
    c = CFDictionaryGetCount((CFDictionaryRef)bd);
    if (c <= 0) return 0;
    {
        const void **keys = malloc(c * sizeof(void *));
        const void **vals = malloc(c * sizeof(void *));
        int found = 0;
        if (!keys || !vals) { free(keys); free(vals); return 0; }
        CFDictionaryGetKeysAndValues((CFDictionaryRef)bd, keys, vals);
        for (pass = 0; pass < 2 && !found; pass++) {
            for (i = 0; i < c && !found; i++) {
                if ((pass == 0) ==
                    (CFStringCompare((CFStringRef)keys[i], CFSTR("default"), 0) == 0))
                    continue;
                if (!vals[i] || CFGetTypeID(vals[i]) != CFDictionaryGetTypeID())
                    continue;
                {
                    CFStringRef p = CFDictionaryGetValue((CFDictionaryRef)vals[i],
                                                         CFSTR("ChangePath"));
                    if (p && CFGetTypeID(p) == CFStringGetTypeID() &&
                        CFStringGetCString(p, out, n, kCFStringEncodingUTF8) &&
                        out[0] && stat(out, &st) == 0) {
                        if (S_ISDIR(st.st_mode))
                            found = pick_random_image(out, out, n);
                        else
                            found = 1;
                    }
                }
            }
        }
        free(keys);
        free(vals);
        return found;
    }
}

static int current_wallpaper(char *out, size_t n)
{
    CFPropertyListRef v;

    out[0] = '\0';
    v = CFPreferencesCopyAppValue(CFSTR("Desktop"), WALLINFO_APP);
    if (v) {
        int ok = desktop_image_path(v, out, n);
        CFRelease(v);
        if (ok) return 1;
    }
    v = CFPreferencesCopyAppValue(CFSTR("Background"), WALLINFO_APP);
    if (v) {
        int ok = legacy_background_path(v, out, n);
        CFRelease(v);
        if (ok) return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* state (original wallpaper + our output path)                              */

static void state_paths(char *statefile, size_t sfn, char *outbase, size_t obn)
{
    const char *home = getenv("HOME");
    if (!home || !*home) home = "/var/root";
    snprintf(g_statedir, sizeof g_statedir,
             "%s/Library/Application Support/wallinfo", home);
    snprintf(statefile, sfn, "%s/state.txt", g_statedir);
    snprintf(outbase, obn, "%s/wallpaper.", g_statedir);
}

static void ensure_statedir(void)
{
    const char *home = getenv("HOME");
    char p[PATH_MAX];
    if (!home || !*home) home = "/var/root";
    snprintf(p, sizeof p, "%s/Library", home);            mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/Library/Application Support", home); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/Library/Application Support/wallinfo", home);
    mkdir(p, 0755);
}

static void read_state(char *orig, size_t on, char *out, size_t un)
{
    char statefile[PATH_MAX], outbase[PATH_MAX];
    FILE *f;

    orig[0] = out[0] = '\0';
    state_paths(statefile, sizeof statefile, outbase, sizeof outbase);
    f = fopen(statefile, "r");
    if (!f) return;
    {
        char line[PATH_MAX];
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\n")] = '\0';
            if (strncmp(line, "orig=", 5) == 0)
                snprintf(orig, on, "%s", line + 5);
            else if (strncmp(line, "out=", 4) == 0)
                snprintf(out, un, "%s", line + 4);
        }
    }
    fclose(f);
}

static void write_state(const char *orig, const char *out)
{
    char statefile[PATH_MAX], outbase[PATH_MAX];
    FILE *f;
    state_paths(statefile, sizeof statefile, outbase, sizeof outbase);
    f = fopen(statefile, "w");
    if (!f) { lg("cannot write state file %s", statefile); return; }
    fprintf(f, "orig=%s\nout=%s\n", orig, out);
    fclose(f);
}

/* Remove rendered wallpapers except the one currently active. Unique
 * filenames per render force the Dock to load the new image — reusing one
 * path let the Dock show a stale cached picture (observed 2026-11-16). */
static void cleanup_old_outputs(const char *keep)
{
    DIR *d = opendir(g_statedir);
    struct dirent *de;
    const char *base = strrchr(keep, '/');
    if (base) base++;
    if (!d) return;
    while ((de = readdir(d)) != NULL) {
        if (strncmp(de->d_name, "wallpaper", 9) == 0 &&
            (!base || strcmp(de->d_name, base) != 0)) {
            char p[PATH_MAX];
            snprintf(p, sizeof p, "%s/%s", g_statedir, de->d_name);
            unlink(p);
        }
    }
    closedir(d);
}

/* ------------------------------------------------------------------------ */
/* rendering                                                                 */

static CFURLRef url_from_path(const char *path)
{
    return CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path,
                                                   strlen(path), false);
}

static const char *out_ext(const char *src)
{
    const char *dot = strrchr(src, '.');
    if (dot && strcasecmp(dot + 1, "png") == 0) return "png";
    return "jpg";
}

static float measure_width(const char *s)
{
    /* scratch context: text is drawn invisible-irrelevant, we only need the
     * pen advance; the context is thrown away. */
    static unsigned char buf[16 * 16 * 4];
    CGContextRef c = CGBitmapContextCreate(buf, 16, 16, 8, 16 * 4,
                                           CGColorSpaceCreateDeviceRGB(),
                                           kCGImageAlphaPremultipliedFirst);
    CGPoint p;
    if (!c) return 0;
    CGContextSelectFont(c, FONT_NAME, FONT_SIZE, kCGEncodingMacRoman);
    CGContextSetTextDrawingMode(c, kCGTextFill);
    CGContextShowTextAtPoint(c, 0, 0, s, strlen(s));
    p = CGContextGetTextPosition(c);
    CFRelease(c);
    return p.x;
}

static int render_wallpaper(const char *src, const char *dst,
                            const struct info *in)
{
    CFURLRef surl = url_from_path(src);
    CGImageSourceRef isrc;
    CGImageRef img;
    CGContextRef ctx;
    CGColorSpaceRef cs;
    void *buf = NULL;
    size_t w, h, i;
    float maxw = 0, boxw, boxh, top_cg, box_left;
    CGImageRef out;
    CFURLRef durl;
    CGImageDestinationRef dest;
    CFStringRef type;
    int ok = 0;
    int img_screen = 0;

    if (!surl) return -1;
    isrc = CGImageSourceCreateWithURL(surl, NULL);
    CFRelease(surl);
    if (!isrc) { lg("cannot open image source %s", src); return -1; }
    img = CGImageSourceCreateImageAtIndex(isrc, 0, NULL);
    CFRelease(isrc);
    if (!img) { lg("cannot decode %s", src); return -1; }

    w = CGImageGetWidth(img);
    h = CGImageGetHeight(img);
    if (w == 0 || h == 0 || w > 16384 || h > 16384) {
        lg("bad image size %lux%lu", (unsigned long)w, (unsigned long)h);
        CGImageRelease(img);
        return -1;
    }

    /* Render at SCREEN size: scale-to-cover + center-crop, like the Dock's
     * "fill screen" placement. Otherwise the Dock crops the image and the
     * box (top-left!) can fall outside the visible area — observed with a
     * 2560x1600 wallpaper on a 1024x768 screen: only the box's bottom-right
     * corner survived. A screen-sized wallpaper is always shown 1:1. */
    {
        size_t sw = CGDisplayPixelsWide(CGMainDisplayID());
        size_t sh = CGDisplayPixelsHigh(CGMainDisplayID());
        if (sw && sh && (sw != w || sh != h)) {
            float s = (float)sw / w > (float)sh / h ? (float)sw / w
                                                    : (float)sh / h;
            lg("scaling %lux%lu -> screen %lux%lu (cover %.3f)",
               (unsigned long)w, (unsigned long)h,
               (unsigned long)sw, (unsigned long)sh, s);
            w = sw;
            h = sh;
            img_screen = 1;
        }
    }

    buf = malloc(w * h * 4);
    if (!buf) { CGImageRelease(img); return -1; }
    cs = CGColorSpaceCreateDeviceRGB();
    ctx = CGBitmapContextCreate(buf, w, h, 8, w * 4, cs,
                                kCGImageAlphaPremultipliedFirst);
    CGColorSpaceRelease(cs);
    if (!ctx) { free(buf); CGImageRelease(img); return -1; }

    if (img_screen) {
        /* draw scaled to cover the screen, centered */
        float s = (float)w / CGImageGetWidth(img) >
                  (float)h / CGImageGetHeight(img)
                      ? (float)w / CGImageGetWidth(img)
                      : (float)h / CGImageGetHeight(img);
        float dw = CGImageGetWidth(img) * s;
        float dh = CGImageGetHeight(img) * s;
        CGContextDrawImage(ctx, CGRectMake((w - dw) / 2, (h - dh) / 2,
                                           dw, dh), img);
    } else {
        CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), img);
    }
    CGImageRelease(img);

    /* box geometry: widest line wins; menu bar clearance via TOP_OFFSET */
    for (i = 0; i < (size_t)in->n; i++) {
        float lw = measure_width(in->lines[i]);
        if (lw > maxw) maxw = lw;
    }
    boxw = maxw + 2 * BOX_PAD;
    boxh = in->n * LINE_HEIGHT + 2 * BOX_PAD;
    if (boxw > w) boxw = w;
    if (boxh > h) boxh = h;

    /* position: top-left, below the menu bar, 10 px from the left edge
     * (CG origin is bottom-left) */
    top_cg = (float)h - TOP_OFFSET;
    if (top_cg < boxh) top_cg = boxh;
    box_left = BOX_LEFT_INSET;

    CGContextSetRGBFillColor(ctx, 1, 1, 1, BOX_ALPHA);
    CGContextFillRect(ctx, CGRectMake(box_left, top_cg - boxh, boxw, boxh));

    CGContextSelectFont(ctx, FONT_NAME, FONT_SIZE, kCGEncodingMacRoman);
    CGContextSetRGBFillColor(ctx, 0, 0, 0, 1);
    CGContextSetTextDrawingMode(ctx, kCGTextFill);
    for (i = 0; i < (size_t)in->n; i++) {
        float baseline = top_cg - BOX_PAD - FONT_SIZE - i * LINE_HEIGHT;
        CGContextShowTextAtPoint(ctx, box_left + BOX_PAD, baseline,
                                 in->lines[i], strlen(in->lines[i]));
    }

    out = CGBitmapContextCreateImage(ctx);
    if (out) {
        type = (strcmp(out_ext(src), "png") == 0) ? CFSTR("public.png")
                                                  : CFSTR("public.jpeg");
        durl = url_from_path(dst);
        dest = durl ? CGImageDestinationCreateWithURL(durl, type, 1, NULL) : NULL;
        if (dest) {
            CFMutableDictionaryRef opts = CFDictionaryCreateMutable(
                NULL, 0, &kCFTypeDictionaryKeyCallBacks,
                &kCFTypeDictionaryValueCallBacks);
            {
                float q = 0.92f;
                CFNumberRef qn = CFNumberCreate(NULL, kCFNumberFloat32Type, &q);
                CFDictionarySetValue(opts,
                    kCGImageDestinationLossyCompressionQuality, qn);
                CFRelease(qn);
            }
            CGImageDestinationAddImage(dest, out, opts);
            ok = CGImageDestinationFinalize(dest);
            CFRelease(opts);
            CFRelease(dest);
        } else {
            lg("cannot create image destination %s", dst);
        }
        if (durl) CFRelease(durl);
        CFRelease(out);
    }
    CFRelease(ctx);
    free(buf);
    return ok ? 0 : -1;
}

/* ------------------------------------------------------------------------ */
/* wallpaper activation                                                      */

/* Alias record (ImageFileAlias) for the given path. The Dock may resolve
 * the alias instead of the plain path, so ours must point at the overlay. */
static CFDataRef create_alias_data(const char *path)
{
    FSRef ref;
    AliasHandle alias = NULL;
    CFDataRef d = NULL;
    int len;

    if (FSPathMakeRef((const UInt8 *)path, &ref, NULL) != noErr) return NULL;
    if (FSNewAlias(NULL, &ref, &alias) != noErr || !alias) return NULL;
    len = GetHandleSize((Handle)alias);
    HLock((Handle)alias);
    d = CFDataCreate(NULL, (const UInt8 *)*alias, len);
    HUnlock((Handle)alias);
    DisposeHandle((Handle)alias);
    return d;
}

/* Rewrite one wallpaper prefs dict so every display entry and "default"
 * is a clean STATIC entry: Change=false, ImageFilePath, and for the
 * Tiger-legacy "Background" dict (what the G4 Dock honours) also
 * NewImageFilePath + ImageFileAlias - the key set System Events writes.
 * Old keys (ChangePath, NewChangePath, TimerPopUpTag) are dropped; as long
 * as ChangePath survives, the Dock keeps rotating and overwrites the entry
 * on every tick (observed 2026-11-16). */
static void set_wallpaper_dict(CFStringRef dictKey, int legacy)
{
    const char *path = g_out_path;

    CFPropertyListRef d = CFPreferencesCopyAppValue(dictKey, WALLINFO_APP);
    CFMutableDictionaryRef m = CFDictionaryCreateMutable(
        NULL, 0, &kCFCopyStringDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    CFStringRef outcf = CFStringCreateWithFileSystemRepresentation(NULL, path);
    CFDataRef alias = NULL;
    CFIndex c, i;

    if (!outcf) { CFRelease(m); return; }
    if (legacy) alias = create_alias_data(path);

    if (d && CFGetTypeID(d) == CFDictionaryGetTypeID()) {
        c = CFDictionaryGetCount((CFDictionaryRef)d);
        if (c > 0) {
            const void **keys = malloc(c * sizeof(void *));
            if (keys) {
                CFDictionaryGetKeysAndValues((CFDictionaryRef)d, keys, NULL);
                for (i = 0; i < c; i++) {
                    CFMutableDictionaryRef mv = CFDictionaryCreateMutable(
                        NULL, 0, &kCFCopyStringDictionaryKeyCallBacks,
                        &kCFTypeDictionaryValueCallBacks);
                    CFDictionarySetValue(mv, CFSTR("Change"), kCFBooleanFalse);
                    CFDictionarySetValue(mv, CFSTR("ImageFilePath"), outcf);
                    if (legacy) {
                        CFDictionarySetValue(mv, CFSTR("NewImageFilePath"), outcf);
                        if (alias) CFDictionarySetValue(mv, CFSTR("ImageFileAlias"), alias);
                    }
                    CFDictionarySetValue(m, (CFStringRef)keys[i], mv);
                    CFRelease(mv);
                }
                free(keys);
            }
        }
        CFRelease(d);
    }
    /* always ensure "default" exists */
    {
        CFMutableDictionaryRef mv = CFDictionaryCreateMutable(
            NULL, 0, &kCFCopyStringDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(mv, CFSTR("Change"), kCFBooleanFalse);
        CFDictionarySetValue(mv, CFSTR("ImageFilePath"), outcf);
        if (legacy) {
            CFDictionarySetValue(mv, CFSTR("NewImageFilePath"), outcf);
            if (alias) CFDictionarySetValue(mv, CFSTR("ImageFileAlias"), alias);
        }
        CFDictionarySetValue(m, CFSTR("default"), mv);
        CFRelease(mv);
    }

    CFPreferencesSetValue(dictKey, m, WALLINFO_APP,
                          kCFPreferencesCurrentUser, kCFPreferencesAnyHost);
    CFPreferencesAppSynchronize(WALLINFO_APP);
    CFRelease(m);
    CFRelease(outcf);
    if (alias) CFRelease(alias);
}

/* Activate the rendered copy:
 * 1. rewrite the prefs dicts to a clean static state (no rotation config)
 * 2. poke the running Dock via System Events (osascript) so it applies the
 *    picture immediately - it writes ImageFilePath + alias itself, which is
 *    exactly the state we just persisted. Direct CFPreferences writes alone
 *    lose against the Dock's rotation ticks. */
static void set_wallpaper(const char *path)
{
    char cmd[PATH_MAX + 160];

    strlcpy(g_out_path, path, sizeof g_out_path);
    set_wallpaper_dict(CFSTR("Desktop"), 0);
    set_wallpaper_dict(CFSTR("Background"), 1);

    snprintf(cmd, sizeof cmd,
        "/usr/bin/osascript -e 'tell application \"System Events\" to "
        "set picture of every desktop to POSIX file \"%s\"'"
        " >/dev/null 2>&1",
        path);
    system(cmd);
}

/* ------------------------------------------------------------------------ */
/* update logic                                                              */

static int update(void)
{
    struct info in;
    char digest[4096];
    char cur[PATH_MAX], orig[PATH_MAX], out[PATH_MAX], outbase[PATH_MAX];
    char statefile[PATH_MAX];
    const char *src;
    char chosen[PATH_MAX];

    collect_info(&in);
    digest_info(&in, digest, sizeof digest);

    read_state(orig, sizeof orig, out, sizeof out);
    state_paths(statefile, sizeof statefile, outbase, sizeof outbase);

    if (!current_wallpaper(cur, sizeof cur)) {
        /* nothing configured: fall back to recorded original, then default */
        if (orig[0] && access(orig, R_OK) == 0)
            strlcpy(cur, orig, sizeof cur);
        else
            strlcpy(cur, FALLBACK_WALLPAPER, sizeof cur);
    }

    /* if the Dock currently shows our own copy, render from the recorded
     * original (never from the copy — that would stack boxes) */
    if (out[0] && strcmp(cur, out) == 0) {
        if (orig[0] && access(orig, R_OK) == 0)
            src = orig;
        else {
            src = FALLBACK_WALLPAPER;
            strlcpy(orig, src, sizeof orig);
        }
    } else {
        src = cur;                 /* user changed the wallpaper: re-adopt */
        strlcpy(orig, cur, sizeof orig);
    }
    if (access(src, R_OK) != 0) {
        src = FALLBACK_WALLPAPER;
        strlcpy(orig, src, sizeof orig);
    }

    /* unique filename per render: the Dock caches by path, so reusing
     * wallpaper.jpg left it showing a stale picture */
    snprintf(out, sizeof out, "%s/wallpaper-%ld-%d.%s", g_statedir,
             (long)time(NULL), (int)getpid(), out_ext(src));
    strlcpy(chosen, src, sizeof chosen);

    if (render_wallpaper(chosen, out, &in) != 0) {
        lg("render failed for %s", chosen);
        return -1;
    }
    set_wallpaper(out);
    write_state(orig, out);
    cleanup_old_outputs(out);

    strlcpy(g_last_digest, digest, sizeof g_last_digest);
    strlcpy(g_last_out, out, sizeof g_last_out);
    lg("wallpaper updated: %s -> %s", chosen, out);
    return 0;
}

static int update_if_needed(void)
{
    struct info in;
    char digest[4096];
    char cur[PATH_MAX];

    if (g_refresh) { g_refresh = 0; return update(); }

    collect_info(&in);
    digest_info(&in, digest, sizeof digest);
    if (!current_wallpaper(cur, sizeof cur)) cur[0] = '\0';

    if (strcmp(digest, g_last_digest) == 0 &&
        strcmp(cur, g_last_out) == 0)
        return 0;                              /* nothing changed */
    return update();
}

static void restore_original(void)
{
    char orig[PATH_MAX], out[PATH_MAX];

    read_state(orig, sizeof orig, out, sizeof out);
    if (!orig[0] || access(orig, R_OK) != 0) {
        lg("no original wallpaper recorded, nothing to restore");
        return;
    }
    set_wallpaper(orig);
    lg("wallpaper restored to %s", orig);
}

/* ------------------------------------------------------------------------ */

static void usage(void)
{
    fprintf(stderr,
        "wallinfo %s — wallpaper info overlay (Mac OS X 10.5, PowerPC)\n"
        "usage: wallinfo [-f] [-o] [-r] [-i SEC] [-t SEC] [-V]\n"
        "  -f        stay in foreground (for launchd)\n"
        "  -o        one update, then exit\n"
        "  -r        restore recorded original wallpaper, then exit\n"
        "  -i SEC    refresh interval (default %d)\n"
        "  -t SEC    network wait timeout, 0 = forever (default %d)\n"
        "  -V        print version and exit\n",
        VERSION, DEFAULT_INTERVAL, DEFAULT_NET_WAIT);
}

int main(int argc, char **argv)
{
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-f") == 0) g_foreground = 1;
        else if (strcmp(argv[i], "-D") == 0) { g_foreground = 1; g_daemonized = 1; }
        else if (strcmp(argv[i], "-o") == 0) g_oneshot = 1;
        else if (strcmp(argv[i], "-r") == 0) g_restore = 1;
        else if (strcmp(argv[i], "-V") == 0) { printf("wallinfo %s\n", VERSION); return 0; }
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) { usage(); return 0; }
        else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) g_interval = atoi(argv[++i]);
        else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) g_net_wait = atoi(argv[++i]);
        else { usage(); return 2; }
    }
    if (g_interval < 5) g_interval = 5;

    srand((unsigned)(time(NULL) ^ getpid()));

    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);
    signal(SIGHUP, on_hup);
    signal(SIGPIPE, SIG_IGN);

    if (g_daemonized) openlog("wallinfo", LOG_PID, LOG_DAEMON);

    if (g_restore) { restore_original(); return 0; }

    if (!g_foreground && !g_oneshot) daemonize();

    lg("wallinfo %s starting (pid %d)", VERSION, (int)getpid());
    ensure_statedir();
    wait_for_network(g_net_wait);

    if (update() != 0) lg("initial update failed");
    if (g_oneshot) return 0;

    while (g_run) {
        int s;
        for (s = 0; s < g_interval && g_run && !g_refresh; s++)
            sleep(1);
        if (!g_run) break;
        update_if_needed();
    }
    lg("wallinfo exiting");
    return 0;
}
