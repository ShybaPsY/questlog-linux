/*
 * questlog-capture: packet capture service for the Questlog desktop app running under Proton.
 *
 * Started by systemd once per connection (questlog-capture.socket, Accept=yes), with the
 * connection on stdin/stdout and CAP_NET_RAW as an ambient capability. The wpcap.dll bridge
 * inside the Wine prefix talks to it with this protocol:
 *
 *   requests (text lines):  LIST
 *                           OPEN <snaplen> <promisc> <timeout_ms> <device>
 *                           FILTER <expression>
 *   replies (binary frames): [type u8][length u32 LE][payload]
 *     'D' device  name\0 description\0 flags u32, count u32, count * (ipv4, netmask)
 *     'Z' end of the device list
 *     'K' ok (after OPEN the payload is the link type, u32)
 *     'E' error text
 *     'P' packet  sec u32, usec u32, caplen u32, len u32, data
 *
 * Limits, so the capability cannot be used to sniff other traffic:
 *   - only connections from the same Linux user are served;
 *   - only filters of the form "tcp and (port A or port B ...)" are accepted, and every port must
 *     be the local port of a TCP socket owned by an allowed game process
 *     (QL_CAPTURE_PROCESSES, default AION2.exe);
 *   - the filter is rebuilt from the validated ports, never passed through;
 *   - promiscuous mode is never enabled.
 */
/* Not _GNU_SOURCE: with it, glibc 2.38+ headers turn strtol/sscanf into symbols that older systems
   lack. POSIX 2008 plus the default BSD/SVID set is all this file needs. */
#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <netinet/in.h>
#include <pcap/pcap.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#define MAX_PORTS 64
#define LOOPBACK_NAME "\\Device\\NPF_Loopback"
#define REVALIDATE_SECONDS 5

#ifndef DLT_LINUX_SLL2
#define DLT_LINUX_SLL2 276
#endif

static pcap_t *cap;
static int cap_linktype;    /* link type of the pcap handle */
static int report_linktype; /* link type reported to Windows (cooked captures become raw IP) */
static int capturing;       /* a validated filter is active */
static int paused;          /* the game no longer owns any of the filter ports */
static uint16_t filter_ports[MAX_PORTS];
static int filter_nports;

static void logmsg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static void write_all(const void *buf, size_t len)
{
    const char *b = buf;
    while (len > 0) {
        ssize_t n = write(1, b, len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) exit(0); /* the app went away */
        b += n;
        len -= (size_t)n;
    }
}

static void frame(char type, const void *payload, uint32_t len)
{
    char hdr[5];
    hdr[0] = type;
    memcpy(hdr + 1, &len, 4);
    write_all(hdr, 5);
    if (len) write_all(payload, len);
}

static void reply_error(const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    logmsg("refused: %s", msg);
    frame('E', msg, (uint32_t)strlen(msg));
}

/* ---- who is connecting ---------------------------------------------------------------- */

/* The peer must be a TCP socket on 127.0.0.1 created by our own uid (checked in /proc/net/tcp). */
static int peer_is_same_user(void)
{
    struct sockaddr_in peer, self;
    socklen_t pl = sizeof peer, sl = sizeof self;
    char line[512];
    FILE *f;
    int ok = 0;

    if (getpeername(0, (struct sockaddr *)&peer, &pl) != 0 || getsockname(0, (struct sockaddr *)&self, &sl) != 0)
        return 0;
    if (peer.sin_family != AF_INET || peer.sin_addr.s_addr != htonl(INADDR_LOOPBACK))
        return 0;
    f = fopen("/proc/net/tcp", "r");
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        unsigned int la, lp, ra, rp, uid;
        if (sscanf(line, " %*d: %8X:%4X %8X:%4X %*X %*X:%*X %*X:%*X %*X %u", &la, &lp, &ra, &rp, &uid) != 5)
            continue;
        /* /proc/net/tcp prints addresses as host-order hex of the network-order value */
        if (la == peer.sin_addr.s_addr && lp == ntohs(peer.sin_port)
            && ra == self.sin_addr.s_addr && rp == ntohs(self.sin_port)) {
            ok = uid == getuid();
            break;
        }
    }
    fclose(f);
    return ok;
}

/* ---- which ports belong to the game ----------------------------------------------------- */

static int allowed_process(pid_t pid)
{
    char path[64], argv0[4096], *base, *s;
    const char *list = getenv("QL_CAPTURE_PROCESSES");
    char names[512];
    ssize_t n;
    FILE *f;

    snprintf(path, sizeof path, "/proc/%d/cmdline", (int)pid);
    f = fopen(path, "r");
    if (!f) return 0;
    n = (ssize_t)fread(argv0, 1, sizeof argv0 - 1, f);
    fclose(f);
    if (n <= 0) return 0;
    argv0[n] = 0;
    base = argv0;
    for (s = argv0; *s; s++)
        if (*s == '/' || *s == '\\') base = s + 1;

    snprintf(names, sizeof names, "%s", list && *list ? list : "AION2.exe");
    for (s = strtok(names, ","); s; s = strtok(NULL, ",")) {
        while (isspace((unsigned char)*s)) s++;
        if (*s && strcasecmp(base, s) == 0) return 1;
    }
    return 0;
}

struct inodes { unsigned long *v; size_t n, cap; };

static void inodes_add(struct inodes *in, unsigned long ino)
{
    if (in->n == in->cap) {
        size_t cap = in->cap ? in->cap * 2 : 256;
        unsigned long *nv = realloc(in->v, cap * sizeof *nv);
        if (!nv) return;
        in->v = nv;
        in->cap = cap;
    }
    in->v[in->n++] = ino;
}

static int inodes_has(const struct inodes *in, unsigned long ino)
{
    for (size_t i = 0; i < in->n; i++)
        if (in->v[i] == ino) return 1;
    return 0;
}

/* Fills ports[] with the local TCP ports of sockets owned by allowed processes. */
static int game_ports(uint16_t *ports, int max)
{
    struct inodes in = {0};
    DIR *proc = opendir("/proc");
    struct dirent *de;
    int count = 0;

    if (!proc) return 0;
    while ((de = readdir(proc))) {
        char fdpath[64];
        DIR *fds;
        struct dirent *fe;
        pid_t pid = (pid_t)atoi(de->d_name);
        if (pid <= 0 || !allowed_process(pid)) continue;
        snprintf(fdpath, sizeof fdpath, "/proc/%d/fd", (int)pid);
        fds = opendir(fdpath);
        if (!fds) continue;
        while ((fe = readdir(fds))) {
            char link[512], target[64];
            ssize_t n;
            unsigned long ino;
            if (fe->d_name[0] == '.') continue;
            snprintf(link, sizeof link, "%s/%s", fdpath, fe->d_name);
            n = readlink(link, target, sizeof target - 1);
            if (n <= 0) continue;
            target[n] = 0;
            if (sscanf(target, "socket:[%lu]", &ino) == 1) inodes_add(&in, ino);
        }
        closedir(fds);
    }
    closedir(proc);

    for (int v6 = 0; v6 < 2 && in.n; v6++) {
        FILE *f = fopen(v6 ? "/proc/net/tcp6" : "/proc/net/tcp", "r");
        char line[512];
        if (!f) continue;
        while (fgets(line, sizeof line, f)) {
            unsigned int lp;
            unsigned long ino;
            int dup = 0;
            if (sscanf(line, " %*d: %*[0-9A-Fa-f]:%4X %*[0-9A-Fa-f]:%*X %*X %*X:%*X %*X:%*X %*X %*u %*d %lu", &lp, &ino) != 2)
                continue;
            if (!inodes_has(&in, ino)) continue;
            for (int i = 0; i < count; i++) dup |= ports[i] == lp;
            if (!dup && count < max) ports[count++] = (uint16_t)lp;
        }
        fclose(f);
    }
    free(in.v);
    return count;
}

/* Accepts exactly "tcp and (port A or port B ...)" or "tcp and port A". */
static int parse_filter(const char *expr, uint16_t *ports, int *nports)
{
    const char *s = expr;
    int paren = 0, n = 0;

    if (strncmp(s, "tcp and ", 8) != 0) return 0;
    s += 8;
    if (*s == '(') { paren = 1; s++; }
    for (;;) {
        char *end;
        long v;
        if (strncmp(s, "port ", 5) != 0) return 0;
        s += 5;
        if (!isdigit((unsigned char)*s)) return 0;
        v = strtol(s, &end, 10);
        if (v <= 0 || v > 65535 || n >= MAX_PORTS) return 0;
        ports[n++] = (uint16_t)v;
        s = end;
        if (strncmp(s, " or ", 4) == 0) { s += 4; continue; }
        break;
    }
    if (paren) {
        if (*s != ')') return 0;
        s++;
    }
    if (*s) return 0;
    *nports = n;
    return 1;
}

/* Keeps only the ports owned by the game; returns how many are left. Wine can still list
   connections that already closed (TIME_WAIT, no owner on Linux), so those are dropped, not fatal. */
static int keep_owned_ports(uint16_t *ports, int n, char *dropped, size_t droppedcap)
{
    uint16_t owned[1024];
    int nowned = game_ports(owned, 1024), kept = 0;
    size_t off = 0;
    dropped[0] = 0;
    for (int i = 0; i < n; i++) {
        int found = 0;
        for (int j = 0; j < nowned; j++) found |= owned[j] == ports[i];
        if (found) ports[kept++] = ports[i];
        else if (off < droppedcap) off += (size_t)snprintf(dropped + off, droppedcap - off, " %u", ports[i]);
    }
    return kept;
}

static int ports_owned(const uint16_t *want, int nwant, int need_all)
{
    uint16_t owned[1024];
    int nowned = game_ports(owned, 1024), hits = 0;
    for (int i = 0; i < nwant; i++) {
        int found = 0;
        for (int j = 0; j < nowned; j++) found |= owned[j] == want[i];
        hits += found;
    }
    return need_all ? hits == nwant && nwant > 0 : hits > 0;
}

/* ---- devices ---------------------------------------------------------------------------- */

static const char *windows_name(const char *linux_name)
{
    return strcmp(linux_name, "lo") == 0 ? LOOPBACK_NAME : linux_name;
}

static const char *linux_name(const char *windows_name)
{
    return strcmp(windows_name, LOOPBACK_NAME) == 0 ? "lo" : windows_name;
}

static void cmd_list(void)
{
    char err[PCAP_ERRBUF_SIZE];
    pcap_if_t *devs, *d;

    if (pcap_findalldevs(&devs, err) != 0) {
        reply_error("pcap_findalldevs: %s", err);
        return;
    }
    for (d = devs; d; d = d->next) {
        unsigned char buf[4096];
        size_t off = 0;
        uint32_t count = 0, flags = d->flags;
        const char *name = windows_name(d->name);
        const char *desc = d->description ? d->description : d->name;
        pcap_addr_t *a;

        if (strcmp(d->name, "any") == 0) continue;
        if (strlen(name) + strlen(desc) + 16 > sizeof buf) continue;
        memcpy(buf, name, strlen(name) + 1);
        off = strlen(name) + 1;
        memcpy(buf + off, desc, strlen(desc) + 1);
        off += strlen(desc) + 1;
        memcpy(buf + off, &flags, 4);
        off += 8; /* count is written below */
        for (a = d->addresses; a; a = a->next) {
            if (!a->addr || a->addr->sa_family != AF_INET || off + 8 > sizeof buf) continue;
            memcpy(buf + off, &((struct sockaddr_in *)a->addr)->sin_addr, 4);
            if (a->netmask && a->netmask->sa_family == AF_INET)
                memcpy(buf + off + 4, &((struct sockaddr_in *)a->netmask)->sin_addr, 4);
            else
                memset(buf + off + 4, 0, 4);
            off += 8;
            count++;
        }
        if (count == 0) continue; /* Questlog picks adapters by IPv4 address; skip usbmon, bluetooth, ... */
        memcpy(buf + strlen(name) + 1 + strlen(desc) + 1 + 4, &count, 4);
        frame('D', buf, (uint32_t)off);
    }
    pcap_freealldevs(devs);
    frame('Z', NULL, 0);
}

static int device_exists(const char *name)
{
    char err[PCAP_ERRBUF_SIZE];
    pcap_if_t *devs, *d;
    int found = 0;
    if (strcmp(name, "any") == 0 || pcap_findalldevs(&devs, err) != 0) return 0;
    for (d = devs; d; d = d->next) found |= strcmp(d->name, name) == 0;
    pcap_freealldevs(devs);
    return found;
}

static void cmd_open(char *args)
{
    char err[PCAP_ERRBUF_SIZE], device[256];
    int snaplen, promisc, to_ms;
    uint32_t lt;
    const char *dev;

    if (cap) { reply_error("this connection already has an open adapter"); return; }
    if (sscanf(args, "%d %d %d %255s", &snaplen, &promisc, &to_ms, device) != 4) {
        reply_error("bad OPEN request");
        return;
    }
    dev = linux_name(device);
    if (!device_exists(dev)) { reply_error("no such adapter: %s", device); return; }
    if (snaplen <= 0 || snaplen > 65535) snaplen = 65535;
    if (to_ms <= 0 || to_ms > 1000) to_ms = 1000;

    cap = pcap_create(dev, err);
    if (!cap) { reply_error("pcap_create %s: %s", dev, err); return; }
    pcap_set_snaplen(cap, snaplen);
    pcap_set_promisc(cap, 0);
    pcap_set_timeout(cap, to_ms);
    pcap_set_immediate_mode(cap, 1);
    pcap_set_buffer_size(cap, 8 << 20);
    if (pcap_activate(cap) < 0) {
        reply_error("pcap_activate %s: %s", dev, pcap_geterr(cap));
        pcap_close(cap);
        cap = NULL;
        return;
    }
    pcap_setnonblock(cap, 1, err);
    cap_linktype = pcap_datalink(cap);
    report_linktype = (cap_linktype == DLT_LINUX_SLL || cap_linktype == DLT_LINUX_SLL2) ? DLT_RAW : cap_linktype;
    lt = (uint32_t)report_linktype;
    logmsg("opened %s (link type %d)", dev, cap_linktype);
    frame('K', &lt, 4);
}

static void cmd_filter(const char *expr)
{
    uint16_t ports[MAX_PORTS];
    int nports;
    char rebuilt[1024];
    size_t off;
    struct bpf_program prog;

    if (!cap) { reply_error("FILTER before OPEN"); return; }
    if (!parse_filter(expr, ports, &nports)) {
        reply_error("only filters like \"tcp and (port N or port M)\" are allowed");
        return;
    }
    {
        char dropped[512];
        int asked = nports;
        nports = keep_owned_ports(ports, nports, dropped, sizeof dropped);
        if (nports == 0) {
            reply_error("none of the filter ports (%s) are connections of the game process", expr + 8);
            return;
        }
        if (nports < asked) logmsg("not capturing ports that are no longer the game's:%s", dropped);
    }
    off = (size_t)snprintf(rebuilt, sizeof rebuilt, "tcp and (");
    for (int i = 0; i < nports; i++)
        off += (size_t)snprintf(rebuilt + off, sizeof rebuilt - off, "%sport %u", i ? " or " : "", ports[i]);
    snprintf(rebuilt + off, sizeof rebuilt - off, ")");

    if (pcap_compile(cap, &prog, rebuilt, 1, PCAP_NETMASK_UNKNOWN) != 0) {
        reply_error("pcap_compile: %s", pcap_geterr(cap));
        return;
    }
    if (pcap_setfilter(cap, &prog) != 0) {
        reply_error("pcap_setfilter: %s", pcap_geterr(cap));
        pcap_freecode(&prog);
        return;
    }
    pcap_freecode(&prog);
    memcpy(filter_ports, ports, sizeof ports[0] * (size_t)nports);
    filter_nports = nports;
    capturing = 1;
    paused = 0;
    logmsg("capturing %s", rebuilt);
    frame('K', NULL, 0);
}

/* ---- packets ------------------------------------------------------------------------------ */

static void on_packet(u_char *user, const struct pcap_pkthdr *h, const u_char *bytes)
{
    uint32_t meta[4];
    char hdr[5];
    struct iovec iov[3];
    uint32_t caplen = h->caplen, len = h->len, total;
    (void)user;

    if (paused) return;
    /* cooked captures (any/ppp/tun on some setups): strip the header, keep IPv4 only */
    if (cap_linktype == DLT_LINUX_SLL || cap_linktype == DLT_LINUX_SLL2) {
        uint32_t hl = cap_linktype == DLT_LINUX_SLL ? 16 : 20;
        uint16_t proto;
        if (caplen < hl) return;
        memcpy(&proto, bytes + (cap_linktype == DLT_LINUX_SLL ? 14 : 0), 2);
        if (ntohs(proto) != 0x0800) return;
        bytes += hl;
        caplen -= hl;
        len = len > hl ? len - hl : caplen;
    }
    meta[0] = (uint32_t)h->ts.tv_sec;
    meta[1] = (uint32_t)h->ts.tv_usec;
    meta[2] = caplen;
    meta[3] = len;
    total = 16 + caplen;
    hdr[0] = 'P';
    memcpy(hdr + 1, &total, 4);
    iov[0].iov_base = hdr;
    iov[0].iov_len = 5;
    iov[1].iov_base = meta;
    iov[1].iov_len = 16;
    iov[2].iov_base = (void *)bytes;
    iov[2].iov_len = caplen;
    {
        size_t left = 5 + 16 + caplen;
        int idx = 0;
        while (left > 0) {
            ssize_t n = writev(1, iov + idx, 3 - idx);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) exit(0);
            left -= (size_t)n;
            while (idx < 3 && (size_t)n >= iov[idx].iov_len) { n -= (ssize_t)iov[idx].iov_len; idx++; }
            if (idx < 3) { iov[idx].iov_base = (char *)iov[idx].iov_base + n; iov[idx].iov_len -= (size_t)n; }
        }
    }
}

/* ---- main loop ---------------------------------------------------------------------------- */

int main(void)
{
    char line[2048];
    size_t used = 0;
    time_t last_check = time(NULL);

    signal(SIGPIPE, SIG_IGN);
    if (!peer_is_same_user()) {
        reply_error("connection refused: not from this user");
        return 1;
    }
    for (;;) {
        struct pollfd pf[2];
        int npf = 1, rc;
        pf[0].fd = 0;
        pf[0].events = POLLIN;
        if (cap && capturing) {
            pf[1].fd = pcap_get_selectable_fd(cap);
            pf[1].events = POLLIN;
            npf = pf[1].fd >= 0 ? 2 : 1;
        }
        rc = poll(pf, (nfds_t)npf, 1000);
        if (rc < 0 && errno != EINTR) return 1;

        if (cap && capturing && time(NULL) - last_check >= REVALIDATE_SECONDS) {
            int owned = ports_owned(filter_ports, filter_nports, 0);
            if (owned == paused) logmsg(owned ? "game ports are back, resuming" : "game ports are gone, pausing");
            paused = !owned;
            last_check = time(NULL);
        }
        if (npf == 2 && (pf[1].revents & POLLIN))
            pcap_dispatch(cap, -1, on_packet, NULL);
        if (rc > 0 && (pf[0].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t n = read(0, line + used, sizeof line - 1 - used);
            char *nl;
            if (n <= 0) break;
            used += (size_t)n;
            line[used] = 0;
            while ((nl = memchr(line, '\n', used))) {
                *nl = 0;
                if (nl > line && nl[-1] == '\r') nl[-1] = 0;
                if (strcmp(line, "LIST") == 0) cmd_list();
                else if (strncmp(line, "OPEN ", 5) == 0) cmd_open(line + 5);
                else if (strncmp(line, "FILTER ", 7) == 0) cmd_filter(line + 7);
                else reply_error("unknown request");
                used -= (size_t)(nl + 1 - line);
                memmove(line, nl + 1, used);
                line[used] = 0;
            }
            if (used == sizeof line - 1) { reply_error("request too long"); break; }
        }
    }
    if (cap) pcap_close(cap);
    return 0;
}
