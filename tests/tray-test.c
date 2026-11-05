/* Checks questlog-ctl.exe hide-tray / show-tray against a stand-in for Electron's tray window.
 *
 *   tray-test.exe <path to questlog-ctl.exe>
 *
 * Opens a window of Electron's tray class, adds an icon with id 3 (Electron's first id), then
 * runs questlog-ctl with QL_CTL_IMAGE set to this program, so it treats this process as Questlog.
 * hide-tray and show-tray must find exactly that icon. */
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>

static LRESULT CALLBACK proc(HWND w, UINT m, WPARAM wp, LPARAM lp) { return DefWindowProcW(w, m, wp, lp); }

static DWORD run(const char *cmdline)
{
    STARTUPINFOA si = { .cb = sizeof si };
    PROCESS_INFORMATION pi;
    char buf[1024];
    DWORD code = 99, start = GetTickCount();
    MSG msg;

    lstrcpynA(buf, cmdline, sizeof buf);
    if (!CreateProcessA(NULL, buf, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return 98;
    /* keep answering messages while the helper talks to the tray */
    while (MsgWaitForMultipleObjects(1, &pi.hProcess, FALSE, 20000, QS_ALLINPUT) == WAIT_OBJECT_0 + 1) {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        if (GetTickCount() - start > 20000) break;
    }
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code;
}

int main(int argc, char **argv)
{
    WNDCLASSW wc = { .lpfnWndProc = proc, .hInstance = GetModuleHandleW(NULL),
                     .lpszClassName = L"Electron_NotifyIconHostWindow" };
    NOTIFYICONDATAW nid = { .cbSize = sizeof nid };
    char self[MAX_PATH], *base, cmd[1024];
    DWORD hide, show, none;
    HWND w;
    int ok;

    if (argc != 2) { fprintf(stderr, "usage: tray-test <questlog-ctl.exe>\n"); return 2; }
    /* Proton's "run" does not pass a console program's output on; QL_TEST_OUT names a file for it. */
    if (getenv("QL_TEST_OUT") && !freopen(getenv("QL_TEST_OUT"), "w", stdout)) return 3;
    setvbuf(stdout, NULL, _IONBF, 0);
    GetModuleFileNameA(NULL, self, MAX_PATH);
    base = strrchr(self, '\\');
    SetEnvironmentVariableA("QL_CTL_IMAGE", base ? base + 1 : self);

    RegisterClassW(&wc);
    w = CreateWindowExW(0, wc.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0, NULL, NULL, wc.hInstance, NULL);
    nid.hWnd = w;
    nid.uID = 3;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    nid.uCallbackMessage = WM_APP;
    nid.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    lstrcpyW(nid.szTip, L"tray-test");
    if (!Shell_NotifyIconW(NIM_ADD, &nid)) {
        printf("FAIL: could not add a tray icon (no tray in this Wine session?)\n");
        return 1;
    }

    snprintf(cmd, sizeof cmd, "\"%s\" hide-tray 5", argv[1]);
    hide = run(cmd);
    snprintf(cmd, sizeof cmd, "\"%s\" show-tray", argv[1]);
    show = run(cmd);
    Shell_NotifyIconW(NIM_DELETE, &nid);
    snprintf(cmd, sizeof cmd, "\"%s\" show-tray", argv[1]);
    none = run(cmd); /* the icon is gone: must report nothing found */

    printf("hide-tray exit %lu, show-tray exit %lu, show-tray without icon exit %lu\n", hide, show, none);
    ok = hide == 0 && show == 0 && none == 1;
    printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
