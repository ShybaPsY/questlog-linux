/*
 * questlog-ctl.exe: small Windows helper that questlog-linux runs inside the game's Wine session.
 * It acts on Questlog's own icon and window only, never on the game or other programs.
 *
 *   questlog-ctl hide-tray [seconds]   hide Questlog's notification-area icon (waits for it to appear)
 *   questlog-ctl show-tray             show it again
 *   questlog-ctl hide-window           hide Questlog's main window (used just before Questlog is ended)
 *
 * Why hide the tray icon: under Wine, Questlog's tray menu opens only the first time. Electron
 * must take the foreground before showing the menu, and wineserver allows that only once per
 * window when the click came through the desktop's tray (explorer.exe got the input).
 * questlog-linux shows its own tray icon instead (drawn by the Linux desktop) and hides this one.
 *
 * Questlog is recognised by its program file name (QL_CTL_IMAGE overrides it, for tests).
 * Exit status: 0 done, 1 nothing found, 2 usage.
 *
 * Build: x86_64-w64-mingw32-gcc -O2 -municode -o questlog-ctl.exe questlog-ctl.c
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#define TRAY_CLASS L"Electron_NotifyIconHostWindow"
#define MAIN_CLASS L"Chrome_WidgetWin_1"
#define MAIN_TITLE L"Questlog"
#define MAX_ICON_ID 32

static wchar_t image_name[MAX_PATH] = L"Questlog.exe";

static int pid_runs_image(DWORD pid)
{
    wchar_t img[MAX_PATH];
    DWORD len = MAX_PATH;
    const wchar_t *base;
    HANDLE h;
    int match = 0;

    h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return 0;
    if (QueryFullProcessImageNameW(h, 0, img, &len)) {
        base = wcsrchr(img, L'\\');
        match = _wcsicmp(base ? base + 1 : img, image_name) == 0;
    }
    CloseHandle(h);
    return match;
}

static int window_runs_image(HWND w)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    return pid && pid_runs_image(pid);
}

/* Sets or clears NIS_HIDDEN on every icon of Questlog's tray windows. Electron numbers its icons
   from a base id, so the ids are probed: NIM_MODIFY fails for an id that does not exist. */
static int set_tray_hidden(int hidden)
{
    HWND w = NULL;
    int changed = 0;

    while ((w = FindWindowExW(NULL, w, TRAY_CLASS, NULL)) != NULL) {
        UINT id;
        if (!window_runs_image(w)) continue;
        for (id = 0; id <= MAX_ICON_ID; id++) {
            NOTIFYICONDATAW nid;
            ZeroMemory(&nid, sizeof nid);
            nid.cbSize = sizeof nid;
            nid.hWnd = w;
            nid.uID = id;
            nid.uFlags = NIF_STATE;
            nid.dwState = hidden ? NIS_HIDDEN : 0;
            nid.dwStateMask = NIS_HIDDEN;
            if (Shell_NotifyIconW(NIM_MODIFY, &nid)) {
                wprintf(L"%ls tray icon %u of window %p\n", hidden ? L"hid" : L"showed", id, (void *)w);
                changed++;
            }
        }
    }
    return changed;
}

static int hide_main_window(void)
{
    HWND w = NULL;
    int n = 0;

    while ((w = FindWindowExW(NULL, w, MAIN_CLASS, MAIN_TITLE)) != NULL) {
        if (!window_runs_image(w) || !IsWindowVisible(w)) continue;
        ShowWindowAsync(w, SW_HIDE);
        wprintf(L"hid window %p\n", (void *)w);
        n++;
    }
    return n;
}

int wmain(int argc, wchar_t **argv)
{
    wchar_t env[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"QL_CTL_IMAGE", env, MAX_PATH);

    if (n > 0 && n < MAX_PATH) lstrcpynW(image_name, env, MAX_PATH);
    if (argc < 2) goto usage;

    if (!wcscmp(argv[1], L"hide-tray")) {
        /* Questlog adds its icon a few seconds after it starts; wait for it. */
        DWORD wait_ms = (argc > 2 ? (DWORD)_wtoi(argv[2]) : 60) * 1000, start = GetTickCount();
        for (;;) {
            if (set_tray_hidden(1)) return 0;
            if (GetTickCount() - start >= wait_ms) break;
            Sleep(500);
        }
        fwprintf(stderr, L"no tray icon of %ls found\n", image_name);
        return 1;
    }
    if (!wcscmp(argv[1], L"show-tray")) {
        if (set_tray_hidden(0)) return 0;
        fwprintf(stderr, L"no tray icon of %ls found\n", image_name);
        return 1;
    }
    if (!wcscmp(argv[1], L"hide-window")) {
        return hide_main_window() ? 0 : 1;
    }
usage:
    fwprintf(stderr, L"usage: questlog-ctl hide-tray [seconds] | show-tray | hide-window\n");
    return 2;
}
