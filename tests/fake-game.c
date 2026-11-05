/* Stand-in for the game in zorder-test: one visible window that takes the foreground,
   then waits until it is closed or killed. */
#include <windows.h>

static LRESULT CALLBACK proc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(w, m, wp, lp);
}

int main(void)
{
    WNDCLASSW wc = { .lpfnWndProc = proc, .hInstance = GetModuleHandleW(NULL), .lpszClassName = L"FakeGame" };
    MSG msg;
    HWND w;

    RegisterClassW(&wc);
    w = CreateWindowExW(0, L"FakeGame", L"fake game", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                        0, 0, 800, 600, NULL, NULL, wc.hInstance, NULL);
    SetForegroundWindow(w);
    while (GetMessageW(&msg, NULL, 0, 0) > 0) DispatchMessageW(&msg);
    return 0;
}
