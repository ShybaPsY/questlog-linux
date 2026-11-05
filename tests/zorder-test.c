/* Checks the window-order guard in wpcap.dll.
 *
 *   zorder-test.exe <path to wpcap.dll> <path to fake-game.exe>
 *
 * This process plays Questlog: it loads the DLL (pcap_findalldevs starts the guard; the
 * capture service is pointed at a closed port, so nothing is captured). It opens an ordinary
 * window, a non-activatable one and a topmost one, starts fake-game.exe, which takes the
 * foreground, and then puts its windows above the game in Wine's order without activating them.
 * With the guard, the ordinary window must end up below the game and the other two must stay
 * where they are. Run it with QL_WINDOW_GUARD=0 as well: then the ordinary window stays above
 * and the test must say FAIL. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

typedef int (*findalldevs_fn)(void **, char *);

static LRESULT CALLBACK proc(HWND w, UINT m, WPARAM wp, LPARAM lp) { return DefWindowProcW(w, m, wp, lp); }

static void pump(DWORD ms)
{
    DWORD start = GetTickCount();
    MSG msg;
    while (GetTickCount() - start < ms) {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        Sleep(10);
    }
}

/* 1 if a is above b in Wine's z-order. */
static int above(HWND a, HWND b)
{
    HWND w;
    for (w = GetWindow(b, GW_HWNDPREV); w; w = GetWindow(w, GW_HWNDPREV))
        if (w == a) return 1;
    return 0;
}

static HWND make(DWORD ex, const wchar_t *title, int x)
{
    return CreateWindowExW(ex, L"ZTest", title, WS_OVERLAPPEDWINDOW | WS_VISIBLE, x, 100, 300, 200,
                           NULL, NULL, GetModuleHandleW(NULL), NULL);
}

int main(int argc, char **argv)
{
    WNDCLASSW wc = { .lpfnWndProc = proc, .hInstance = GetModuleHandleW(NULL), .lpszClassName = L"ZTest" };
    STARTUPINFOA si = { .cb = sizeof si };
    PROCESS_INFORMATION pi;
    HMODULE dll;
    findalldevs_fn findalldevs;
    void *devs = NULL;
    char err[256] = "";
    HWND normal, noact, topmost, game = NULL;
    int i, ok;

    if (argc != 3) { fprintf(stderr, "usage: zorder-test <wpcap.dll> <fake-game.exe>\n"); return 2; }
    /* Proton's "run" does not pass a console program's output on; QL_TEST_OUT names a file for it. */
    if (getenv("QL_TEST_OUT") && !freopen(getenv("QL_TEST_OUT"), "w", stdout)) return 3;
    setvbuf(stdout, NULL, _IONBF, 0);
    SetEnvironmentVariableA("QL_CAPTURE_PORT", "1");

    RegisterClassW(&wc);
    normal = make(0, L"Questlog (ordinary window)", 0);
    noact = make(WS_EX_NOACTIVATE, L"Questlog overlay (no activate)", 320);
    topmost = make(WS_EX_TOPMOST | WS_EX_NOACTIVATE, L"Questlog overlay (topmost)", 640);
    pump(300);

    if (!CreateProcessA(argv[2], NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        printf("FAIL: could not start %s (%lu)\n", argv[2], GetLastError());
        return 1;
    }
    for (i = 0; i < 100 && !game; i++) {
        pump(50);
        game = FindWindowW(L"FakeGame", NULL);
    }
    for (i = 0; i < 100 && GetForegroundWindow() != game; i++) pump(50);
    if (!game || GetForegroundWindow() != game) {
        printf("FAIL: the fake game did not get the foreground\n");
        TerminateProcess(pi.hProcess, 0);
        return 1;
    }

    /* Questlog's state when the game was raised by the window manager: its windows still above. */
    SetWindowPos(normal, HWND_TOP, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE);
    SetWindowPos(noact, HWND_TOP, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE);
    printf("before: ordinary above game %d, no-activate above game %d, topmost above game %d\n",
           above(normal, game), above(noact, game), above(topmost, game));

    dll = LoadLibraryA(argv[1]);
    findalldevs = dll ? (findalldevs_fn)(void *)GetProcAddress(dll, "pcap_findalldevs") : NULL;
    if (!findalldevs) {
        printf("FAIL: could not load %s\n", argv[1]);
        TerminateProcess(pi.hProcess, 0);
        return 1;
    }
    findalldevs(&devs, err); /* fails (closed port); starts the guard */
    pump(500);

    printf("after:  ordinary above game %d, no-activate above game %d, topmost above game %d, foreground is game %d\n",
           above(normal, game), above(noact, game), above(topmost, game), GetForegroundWindow() == game);
    ok = !above(normal, game) && above(noact, game) && above(topmost, game) && GetForegroundWindow() == game;
    printf("%s\n", ok ? "PASS" : "FAIL");

    TerminateProcess(pi.hProcess, 0);
    return ok ? 0 : 1;
}
