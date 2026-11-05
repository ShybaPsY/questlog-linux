/*
 * wpcap.dll bridge for running the Questlog desktop app under Wine/Proton.
 *
 * Questlog loads %SystemRoot%\System32\Npcap\wpcap.dll with koffi and calls a
 * small part of the libpcap API. Npcap does not exist under Wine, and a Wine
 * process inside Steam's container cannot open raw sockets, so this DLL
 * forwards those calls over 127.0.0.1 to questlog-capture, a small Linux
 * service that does the capture with the host's libpcap.
 *
 * Only the functions Questlog uses are implemented. The service only accepts
 * capture filters for the TCP ports owned by the game process, so this bridge
 * cannot be used to sniff unrelated traffic.
 *
 * The DLL also runs a small window-order guard (see the section below), since
 * it is the one piece of code that runs inside Questlog's own processes.
 *
 * Environment (set by questlog-linux):
 *   QL_CAPTURE_PORT   service port (default 29741)
 *   QL_BRIDGE_LOG     file for the guard's messages (none if unset)
 *   QL_WINDOW_GUARD   0 turns the window-order guard off
 *
 * Build: x86_64-w64-mingw32-gcc -shared -O2 -o wpcap.dll wpcap.c -lws2_32
 */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PCAP_ERRBUF_SIZE 256
#define DEFAULT_PORT 29741
#define BRIDGE_VERSION "questlog-linux wpcap bridge 1.1 (host libpcap via questlog-capture)"

/* Frame types sent by questlog-capture: [type u8][length u32 LE][payload]. */
#define F_DEVICE 'D'
#define F_END 'Z'
#define F_OK 'K'
#define F_ERROR 'E'
#define F_PACKET 'P'

/* Layouts as Windows callers expect them (struct timeval uses 32-bit longs). */
struct pcap_pkthdr {
    int32_t tv_sec;
    int32_t tv_usec;
    uint32_t caplen;
    uint32_t len;
};

struct bpf_program {
    unsigned int bf_len;
    void *bf_insns; /* here: a heap copy of the filter text */
};

typedef struct pcap_addr {
    struct pcap_addr *next;
    struct sockaddr *addr;
    struct sockaddr *netmask;
    struct sockaddr *broadaddr;
    struct sockaddr *dstaddr;
} pcap_addr_t;

typedef struct pcap_if {
    struct pcap_if *next;
    char *name;
    char *description;
    pcap_addr_t *addresses;
    uint32_t flags;
} pcap_if_t;

typedef struct pcap {
    SOCKET sock;
    int nonblock;
    int timeout_ms;
    int linktype;
    char errbuf[PCAP_ERRBUF_SIZE];
    unsigned char *rbuf; /* bytes received and not consumed yet */
    size_t rlen, rcap;
    unsigned char *pkt; /* data of the packet last returned by pcap_next_ex */
    size_t pktcap;
    struct pcap_pkthdr hdr;
} pcap_t;

static INIT_ONCE wsa_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK wsa_init(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    WSADATA wsa;
    (void)once; (void)param; (void)ctx;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
}

static void seterr(char *errbuf, const char *fmt, ...)
{
    va_list ap;
    if (!errbuf) return;
    va_start(ap, fmt);
    _vsnprintf(errbuf, PCAP_ERRBUF_SIZE - 1, fmt, ap);
    errbuf[PCAP_ERRBUF_SIZE - 1] = 0;
    va_end(ap);
}

static int bridge_port(void)
{
    char buf[16];
    DWORD n = GetEnvironmentVariableA("QL_CAPTURE_PORT", buf, sizeof buf);
    if (n > 0 && n < sizeof buf) {
        int p = atoi(buf);
        if (p > 0 && p < 65536) return p;
    }
    return DEFAULT_PORT;
}

static SOCKET bridge_connect(char *errbuf)
{
    struct sockaddr_in sa;
    SOCKET s;
    int one = 1;

    InitOnceExecuteOnce(&wsa_once, wsa_init, NULL, NULL);
    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        seterr(errbuf, "socket() failed: %d", WSAGetLastError());
        return INVALID_SOCKET;
    }
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((u_short)bridge_port());
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (struct sockaddr *)&sa, sizeof sa) != 0) {
        seterr(errbuf, "The questlog-capture service is not reachable on 127.0.0.1:%d (error %d). "
               "Is questlog-capture.socket enabled on the Linux host?", bridge_port(), WSAGetLastError());
        closesocket(s);
        return INVALID_SOCKET;
    }
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    return s;
}

static int send_all(SOCKET s, const char *buf, size_t len)
{
    while (len > 0) {
        int n = send(s, buf, (int)(len > 65536 ? 65536 : len), 0);
        if (n <= 0) return -1;
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

static int send_line(SOCKET s, const char *fmt, ...)
{
    char line[2048];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = _vsnprintf(line, sizeof line - 2, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof line - 2) return -1;
    if (strchr(line, '\n')) return -1;
    line[n++] = '\n';
    return send_all(s, line, (size_t)n);
}

/* Reads up to `want` more bytes into p->rbuf. wait_ms: 0 = poll, -1 = forever. */
static int fill(pcap_t *p, size_t want, int wait_ms)
{
    fd_set rd;
    struct timeval tv;
    int n;

    if (p->rcap - p->rlen < want) {
        size_t cap = p->rcap ? p->rcap : 65536;
        unsigned char *nb;
        while (cap - p->rlen < want) cap *= 2;
        nb = realloc(p->rbuf, cap);
        if (!nb) { seterr(p->errbuf, "out of memory"); return -1; }
        p->rbuf = nb;
        p->rcap = cap;
    }
    if (wait_ms >= 0) {
        FD_ZERO(&rd);
        FD_SET(p->sock, &rd);
        tv.tv_sec = wait_ms / 1000;
        tv.tv_usec = (wait_ms % 1000) * 1000;
        n = select(0, &rd, NULL, NULL, &tv);
        if (n == 0) return 0;
        if (n < 0) { seterr(p->errbuf, "select() failed: %d", WSAGetLastError()); return -1; }
    }
    n = recv(p->sock, (char *)p->rbuf + p->rlen, (int)(p->rcap - p->rlen), 0);
    if (n == 0) { seterr(p->errbuf, "questlog-capture closed the connection"); return -1; }
    if (n < 0) { seterr(p->errbuf, "recv() failed: %d", WSAGetLastError()); return -1; }
    p->rlen += (size_t)n;
    return n;
}

/* If a whole frame is buffered, returns its total size and fills type/payload/len. */
static size_t frame_ready(pcap_t *p, unsigned char *type, unsigned char **payload, uint32_t *len)
{
    uint32_t l;
    if (p->rlen < 5) return 0;
    memcpy(&l, p->rbuf + 1, 4);
    if (p->rlen < 5 + (size_t)l) return 0;
    *type = p->rbuf[0];
    *payload = p->rbuf + 5;
    *len = l;
    return 5 + (size_t)l;
}

static void consume(pcap_t *p, size_t n)
{
    memmove(p->rbuf, p->rbuf + n, p->rlen - n);
    p->rlen -= n;
}

/* Blocks until a control frame (not a packet) arrives; packets before it are dropped. */
static int wait_control(pcap_t *p, unsigned char *type, char *text, size_t textcap, uint32_t *value)
{
    for (;;) {
        unsigned char t, *pl;
        uint32_t l;
        size_t total = frame_ready(p, &t, &pl, &l);
        if (!total) {
            if (fill(p, 65536, -1) < 0) return -1;
            continue;
        }
        if (t != F_PACKET) {
            *type = t;
            if (text && textcap) {
                size_t c = l < textcap - 1 ? l : textcap - 1;
                memcpy(text, pl, c);
                text[c] = 0;
            }
            if (value) {
                *value = 0;
                if (l >= 4) memcpy(value, pl, 4);
            }
            consume(p, total);
            return 0;
        }
        consume(p, total);
    }
}

/* ---- window-order guard ---------------------------------------------------------------------
 *
 * While the game is in front it confines the cursor (ClipCursor). winex11 does that with a
 * pointer grab on a screen-sized InputOnly window, and a click that lands on it is routed by
 * wineserver to the window under the cursor in Wine's own z-order. That order is not updated
 * when the window manager raises the game, so Questlog's main window, if it was the last one
 * active, still counts as above the game: an Alt+click in the game goes to Questlog, and
 * Chromium brings the window to the front in the middle of a fight.
 *
 * The guard keeps Wine's order in line with the screen: every 50 ms, if the foreground window
 * belongs to another program, every ordinary Questlog window above it in Wine's order is put
 * just below it. Topmost and non-activatable windows (Questlog's overlays) are left alone,
 * nothing is activated, and the X stacking is not touched (winex11 does not restack managed
 * windows downwards). Questlog's windows are recognised by program file: this DLL runs in
 * Questlog's Node service process, its windows belong to the browser process.
 */
#define GUARD_INTERVAL_MS 50
#define GUARD_LOG_MAX 40

static INIT_ONCE guard_once_flag = INIT_ONCE_STATIC_INIT;
static wchar_t own_image[MAX_PATH];
static FILE *guard_log;
static int guard_logged;

static void guard_msg(const char *fmt, ...)
{
    va_list ap;
    SYSTEMTIME t;
    if (!guard_log || guard_logged >= GUARD_LOG_MAX) return;
    if (++guard_logged == GUARD_LOG_MAX) fmt = "(further messages suppressed)";
    GetLocalTime(&t);
    fprintf(guard_log, "%04u-%02u-%02u %02u:%02u:%02u [pid %lu] ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
            t.wSecond, (unsigned long)GetCurrentProcessId());
    va_start(ap, fmt);
    vfprintf(guard_log, fmt, ap);
    va_end(ap);
    fputc('\n', guard_log);
    fflush(guard_log);
}

/* Small pid -> "runs our program file" cache, dropped every few seconds (pids are reused). */
static struct { DWORD pid; int ours; } pid_cache[64];
static int pid_cache_n;
static DWORD pid_cache_time;

static int pid_is_ours(DWORD pid)
{
    wchar_t img[MAX_PATH];
    DWORD len = MAX_PATH;
    HANDLE h;
    int i, ours = 0;

    if (!pid) return 0;
    if (pid == GetCurrentProcessId()) return 1;
    if (GetTickCount() - pid_cache_time > 5000) { pid_cache_n = 0; pid_cache_time = GetTickCount(); }
    for (i = 0; i < pid_cache_n; i++)
        if (pid_cache[i].pid == pid) return pid_cache[i].ours;
    h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h) {
        if (QueryFullProcessImageNameW(h, 0, img, &len)) ours = lstrcmpiW(img, own_image) == 0;
        CloseHandle(h);
    }
    if (pid_cache_n < (int)(sizeof pid_cache / sizeof pid_cache[0])) {
        pid_cache[pid_cache_n].pid = pid;
        pid_cache[pid_cache_n].ours = ours;
        pid_cache_n++;
    }
    return ours;
}

static int window_is_ours(HWND w)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    return pid_is_ours(pid);
}

static void guard_step(void)
{
    HWND fg = GetForegroundWindow(), w, above;

    if (!fg || window_is_ours(fg)) return;
    for (w = GetWindow(fg, GW_HWNDPREV); w; w = above) {
        LONG_PTR ex;
        above = GetWindow(w, GW_HWNDPREV);
        if (!IsWindowVisible(w)) continue;
        ex = GetWindowLongPtrW(w, GWL_EXSTYLE);
        if (ex & (WS_EX_TOPMOST | WS_EX_NOACTIVATE)) continue;
        if (!window_is_ours(w)) continue;
        if (SetWindowPos(w, fg, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_NOOWNERZORDER)) {
            wchar_t title[64] = L"";
            GetWindowTextW(w, title, 64);
            guard_msg("window %p '%ls' was above the foreground window %p of another program in Wine's order; "
                      "moved it below", (void *)w, title, (void *)fg);
        } else {
            guard_msg("could not move window %p below %p (error %lu)", (void *)w, (void *)fg, GetLastError());
        }
    }
}

static DWORD WINAPI guard_thread(LPVOID unused)
{
    (void)unused;
    for (;;) {
        Sleep(GUARD_INTERVAL_MS);
        guard_step();
    }
    return 0;
}

static BOOL CALLBACK guard_start(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    char buf[MAX_PATH];
    DWORD n;
    HANDLE t;
    (void)once; (void)param; (void)ctx;

    n = GetEnvironmentVariableA("QL_BRIDGE_LOG", buf, sizeof buf);
    if (n > 0 && n < sizeof buf) guard_log = fopen(buf, "a");
    n = GetEnvironmentVariableA("QL_WINDOW_GUARD", buf, sizeof buf);
    if (n > 0 && n < sizeof buf && strcmp(buf, "0") == 0) {
        guard_msg("window-order guard off (QL_WINDOW_GUARD=0)");
        return TRUE;
    }
    if (!GetModuleFileNameW(NULL, own_image, MAX_PATH)) {
        guard_msg("window-order guard not started: GetModuleFileName failed (%lu)", GetLastError());
        return TRUE;
    }
    t = CreateThread(NULL, 0, guard_thread, NULL, 0, NULL);
    if (!t) {
        guard_msg("window-order guard not started: CreateThread failed (%lu)", GetLastError());
        return TRUE;
    }
    CloseHandle(t);
    guard_msg("window-order guard running for %ls", own_image);
    return TRUE;
}

static void ensure_guard(void)
{
    InitOnceExecuteOnce(&guard_once_flag, guard_start, NULL, NULL);
}

static pcap_t *handle_new(SOCKET s)
{
    pcap_t *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->sock = s;
    p->timeout_ms = 1000;
    return p;
}

static void handle_free(pcap_t *p)
{
    if (!p) return;
    if (p->sock != INVALID_SOCKET) closesocket(p->sock);
    free(p->rbuf);
    free(p->pkt);
    free(p);
}

__declspec(dllexport) const char *pcap_lib_version(void)
{
    return BRIDGE_VERSION;
}

__declspec(dllexport) void pcap_freealldevs(pcap_if_t *devs)
{
    while (devs) {
        pcap_if_t *next = devs->next;
        pcap_addr_t *a = devs->addresses;
        while (a) {
            pcap_addr_t *an = a->next;
            free(a->addr);
            free(a->netmask);
            free(a);
            a = an;
        }
        free(devs->name);
        free(devs->description);
        free(devs);
        devs = next;
    }
}

/* Device frame payload: name\0 description\0 flags(u32) count(u32) count*(ipv4 u32, mask u32), network order. */
static pcap_if_t *parse_device(const unsigned char *pl, uint32_t len)
{
    const unsigned char *end = pl + len, *q;
    size_t nlen, dlen;
    uint32_t flags, count, i;
    pcap_if_t *d;
    pcap_addr_t **tail;

    q = memchr(pl, 0, len);
    if (!q) return NULL;
    nlen = (size_t)(q - pl);
    q = memchr(q + 1, 0, (size_t)(end - (q + 1)));
    if (!q) return NULL;
    dlen = (size_t)(q - (pl + nlen + 1));
    q++;
    if (end - q < 8) return NULL;
    memcpy(&flags, q, 4);
    memcpy(&count, q + 4, 4);
    q += 8;
    if ((size_t)(end - q) < (size_t)count * 8) return NULL;

    d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->name = malloc(nlen + 1);
    d->description = malloc(dlen + 1);
    if (!d->name || !d->description) { pcap_freealldevs(d); return NULL; }
    memcpy(d->name, pl, nlen);
    d->name[nlen] = 0;
    memcpy(d->description, pl + nlen + 1, dlen);
    d->description[dlen] = 0;
    d->flags = flags;

    tail = &d->addresses;
    for (i = 0; i < count; i++, q += 8) {
        pcap_addr_t *a = calloc(1, sizeof *a);
        struct sockaddr_in *ad = calloc(1, sizeof *ad), *mk = calloc(1, sizeof *mk);
        if (!a || !ad || !mk) { free(a); free(ad); free(mk); break; }
        ad->sin_family = AF_INET;
        memcpy(&ad->sin_addr, q, 4);
        mk->sin_family = AF_INET;
        memcpy(&mk->sin_addr, q + 4, 4);
        a->addr = (struct sockaddr *)ad;
        a->netmask = (struct sockaddr *)mk;
        *tail = a;
        tail = &a->next;
    }
    return d;
}

__declspec(dllexport) int pcap_findalldevs(pcap_if_t **alldevs, char *errbuf)
{
    pcap_t *p;
    pcap_if_t *head = NULL, **tail = &head;
    SOCKET s;

    *alldevs = NULL;
    ensure_guard();
    s = bridge_connect(errbuf);
    if (s == INVALID_SOCKET) return -1;
    p = handle_new(s);
    if (!p) { closesocket(s); seterr(errbuf, "out of memory"); return -1; }
    if (send_line(s, "LIST") != 0) {
        seterr(errbuf, "could not talk to questlog-capture");
        handle_free(p);
        return -1;
    }
    for (;;) {
        unsigned char t, *pl;
        uint32_t l;
        size_t total = frame_ready(p, &t, &pl, &l);
        if (!total) {
            if (fill(p, 65536, 5000) <= 0) {
                seterr(errbuf, "questlog-capture did not answer: %s", p->errbuf[0] ? p->errbuf : "timeout");
                pcap_freealldevs(head);
                handle_free(p);
                return -1;
            }
            continue;
        }
        if (t == F_DEVICE) {
            pcap_if_t *d = parse_device(pl, l);
            if (d) { *tail = d; tail = &d->next; }
        } else if (t == F_ERROR) {
            char msg[PCAP_ERRBUF_SIZE];
            size_t c = l < sizeof msg - 1 ? l : sizeof msg - 1;
            memcpy(msg, pl, c);
            msg[c] = 0;
            seterr(errbuf, "%s", msg);
            pcap_freealldevs(head);
            handle_free(p);
            return -1;
        } else if (t == F_END) {
            consume(p, total);
            break;
        }
        consume(p, total);
    }
    handle_free(p);
    *alldevs = head;
    return 0;
}

__declspec(dllexport) pcap_t *pcap_open_live(const char *device, int snaplen, int promisc, int to_ms, char *errbuf)
{
    unsigned char t;
    char msg[PCAP_ERRBUF_SIZE];
    uint32_t linktype;
    pcap_t *p;
    SOCKET s;

    ensure_guard();
    if (!device || !*device || strpbrk(device, " \r\n")) {
        seterr(errbuf, "invalid device name");
        return NULL;
    }
    s = bridge_connect(errbuf);
    if (s == INVALID_SOCKET) return NULL;
    p = handle_new(s);
    if (!p) { closesocket(s); seterr(errbuf, "out of memory"); return NULL; }
    p->timeout_ms = to_ms > 0 ? to_ms : 1000;
    if (send_line(s, "OPEN %d %d %d %s", snaplen, promisc ? 1 : 0, to_ms, device) != 0
        || wait_control(p, &t, msg, sizeof msg, &linktype) != 0) {
        seterr(errbuf, "could not talk to questlog-capture: %s", p->errbuf);
        handle_free(p);
        return NULL;
    }
    if (t != F_OK) {
        seterr(errbuf, "%s", msg);
        handle_free(p);
        return NULL;
    }
    p->linktype = (int)linktype;
    return p;
}

__declspec(dllexport) int pcap_setnonblock(pcap_t *p, int nonblock, char *errbuf)
{
    (void)errbuf;
    p->nonblock = nonblock ? 1 : 0;
    return 0;
}

__declspec(dllexport) int pcap_getnonblock(pcap_t *p, char *errbuf)
{
    (void)errbuf;
    return p->nonblock;
}

__declspec(dllexport) int pcap_datalink(pcap_t *p)
{
    return p->linktype;
}

__declspec(dllexport) int pcap_compile(pcap_t *p, struct bpf_program *fp, const char *str, int optimize, uint32_t netmask)
{
    size_t n;
    char *copy;
    (void)optimize; (void)netmask;
    if (!str) str = "";
    n = strlen(str);
    if (n > 1800 || strpbrk(str, "\r\n")) {
        seterr(p->errbuf, "filter expression is too long or has line breaks");
        return -1;
    }
    copy = malloc(n + 1);
    if (!copy) { seterr(p->errbuf, "out of memory"); return -1; }
    memcpy(copy, str, n + 1);
    fp->bf_len = (unsigned int)n;
    fp->bf_insns = copy;
    return 0;
}

__declspec(dllexport) void pcap_freecode(struct bpf_program *fp)
{
    if (!fp) return;
    free(fp->bf_insns);
    fp->bf_insns = NULL;
    fp->bf_len = 0;
}

__declspec(dllexport) int pcap_setfilter(pcap_t *p, struct bpf_program *fp)
{
    unsigned char t;
    char msg[PCAP_ERRBUF_SIZE];
    if (!fp || !fp->bf_insns) {
        seterr(p->errbuf, "no filter program");
        return -1;
    }
    if (send_line(p->sock, "FILTER %s", (const char *)fp->bf_insns) != 0
        || wait_control(p, &t, msg, sizeof msg, NULL) != 0)
        return -1;
    if (t != F_OK) {
        seterr(p->errbuf, "%s", msg);
        return -1;
    }
    return 0;
}

/* Returns 1 with a packet, 0 when none is ready (or the timeout passed), -1 on error. */
__declspec(dllexport) int pcap_next_ex(pcap_t *p, struct pcap_pkthdr **header, const unsigned char **data)
{
    int waited = 0;
    for (;;) {
        unsigned char t, *pl;
        uint32_t l;
        size_t total = frame_ready(p, &t, &pl, &l);
        if (total) {
            if (t == F_PACKET && l >= 16) {
                size_t caplen = l - 16;
                if (p->pktcap < caplen) {
                    unsigned char *nb = realloc(p->pkt, caplen ? caplen : 1);
                    if (!nb) { seterr(p->errbuf, "out of memory"); return -1; }
                    p->pkt = nb;
                    p->pktcap = caplen;
                }
                memcpy(&p->hdr.tv_sec, pl, 4);
                memcpy(&p->hdr.tv_usec, pl + 4, 4);
                memcpy(&p->hdr.len, pl + 12, 4);
                p->hdr.caplen = (uint32_t)caplen;
                memcpy(p->pkt, pl + 16, caplen);
                consume(p, total);
                *header = &p->hdr;
                *data = p->pkt;
                return 1;
            }
            if (t == F_ERROR) {
                size_t c = l < PCAP_ERRBUF_SIZE - 1 ? l : PCAP_ERRBUF_SIZE - 1;
                memcpy(p->errbuf, pl, c);
                p->errbuf[c] = 0;
                consume(p, total);
                return -1;
            }
            consume(p, total);
            continue;
        }
        if (p->nonblock || waited) {
            int n = fill(p, 65536, 0);
            if (n < 0) return -1;
            if (n == 0) return 0;
            continue;
        }
        {
            int n = fill(p, 65536, p->timeout_ms);
            if (n < 0) return -1;
            if (n == 0) return 0;
            waited = 1;
        }
    }
}

__declspec(dllexport) char *pcap_geterr(pcap_t *p)
{
    return p->errbuf;
}

__declspec(dllexport) void pcap_close(pcap_t *p)
{
    handle_free(p);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)inst; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(inst);
    return TRUE;
}
