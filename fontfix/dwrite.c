/*
 * dwrite.dll stand-in for Questlog under Wine/Proton: makes its variable fonts render with the
 * right weights.
 *
 * Questlog's interface fonts are variable fonts (Encode Sans Condensed, wght 100-900, default
 * instance Thin; Noto Sans, wght 100-900). Wine's DirectWrite accepts the axis values it is given
 * but does not apply them when rendering, so every weight comes out as one instance: all Encode
 * Sans text is Thin and all Noto Sans text is one weight.
 *
 * Chromium decides how to load a variable web font by asking DirectWrite's font faces for
 * IDWriteFontFace5 (variation support, Windows 10 1709+). With it, Chromium uses DirectWrite;
 * without it, Chromium uses its own bundled FreeType, which applies the axes correctly (as on
 * older Windows versions). This stand-in forwards DWriteCreateFactory to Wine's DirectWrite and
 * only makes font faces answer "no" to IDWriteFontFace5/6/7. Everything else is Wine's code.
 *
 * It is loaded only by Questlog.exe and its child processes, through a per-program DLL override
 * (HKCU\Software\Wine\AppDefaults\Questlog.exe\DllOverrides: dwrite=native,builtin) and a copy
 * in Questlog's folder. The game and other programs keep Wine's DirectWrite as is.
 *
 * QL_BRIDGE_LOG (a Windows path) gets one line per process. Build:
 *   x86_64-w64-mingw32-gcc -O2 -shared -o dwrite.dll dwrite.c -luuid
 */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <dwrite_3.h>
#include <stdio.h>

typedef HRESULT (WINAPI *create_factory_fn)(DWRITE_FACTORY_TYPE, REFIID, IUnknown **);

static HMODULE self;
static HMODULE wine_dwrite;
static create_factory_fn wine_create_factory;
static HRESULT (STDMETHODCALLTYPE *wine_fontface_qi)(IDWriteFontFace *, REFIID, void **);
static INIT_ONCE load_once = INIT_ONCE_STATIC_INIT;
static INIT_ONCE patch_once = INIT_ONCE_STATIC_INIT;

static void note(const char *fmt, ...)
{
    char path[MAX_PATH], exe[MAX_PATH] = "?";
    DWORD n = GetEnvironmentVariableA("QL_BRIDGE_LOG", path, sizeof path);
    SYSTEMTIME t;
    va_list ap;
    FILE *f;

    if (!n || n >= sizeof path || !(f = fopen(path, "a"))) return;
    GetModuleFileNameA(NULL, exe, sizeof exe);
    GetLocalTime(&t);
    fprintf(f, "%04u-%02u-%02u %02u:%02u:%02u [pid %lu] font fix: ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
            t.wSecond, (unsigned long)GetCurrentProcessId());
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fprintf(f, " (%s)\n", strrchr(exe, '\\') ? strrchr(exe, '\\') + 1 : exe);
    fclose(f);
}

static BOOL CALLBACK load_wine_dwrite(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    wchar_t path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH - 12);
    (void)once; (void)param; (void)ctx;

    if (!n || n >= MAX_PATH - 12) return TRUE;
    lstrcatW(path, L"\\dwrite.dll");
    wine_dwrite = LoadLibraryW(path);
    if (!wine_dwrite || wine_dwrite == self) {
        note("could not load Wine's DirectWrite from the system folder (error %lu)", GetLastError());
        wine_dwrite = NULL;
        return TRUE;
    }
    wine_create_factory = (create_factory_fn)(void *)GetProcAddress(wine_dwrite, "DWriteCreateFactory");
    if (!wine_create_factory) note("Wine's DirectWrite has no DWriteCreateFactory");
    return TRUE;
}

static HRESULT STDMETHODCALLTYPE fontface_qi(IDWriteFontFace *face, REFIID riid, void **out)
{
    if (IsEqualIID(riid, &IID_IDWriteFontFace5) || IsEqualIID(riid, &IID_IDWriteFontFace6)
        || IsEqualIID(riid, &IID_IDWriteFontFace7)) {
        if (out) *out = NULL;
        return E_NOINTERFACE;
    }
    return wine_fontface_qi(face, riid, out);
}

/* Any font face of the system collection, to reach the method table all font faces share. */
static IDWriteFontFace *any_font_face(IDWriteFactory *factory)
{
    IDWriteFontCollection *collection = NULL;
    IDWriteFontFamily *family = NULL;
    IDWriteFont *font = NULL;
    IDWriteFontFace *face = NULL;
    UINT32 i, count;

    if (FAILED(IDWriteFactory_GetSystemFontCollection(factory, &collection, FALSE))) return NULL;
    count = IDWriteFontCollection_GetFontFamilyCount(collection);
    for (i = 0; i < count && !face; i++) {
        if (FAILED(IDWriteFontCollection_GetFontFamily(collection, i, &family))) continue;
        if (SUCCEEDED(IDWriteFontFamily_GetFont(family, 0, &font))) {
            IDWriteFont_CreateFontFace(font, &face);
            IDWriteFont_Release(font);
        }
        IDWriteFontFamily_Release(family);
    }
    IDWriteFontCollection_Release(collection);
    return face;
}

static BOOL CALLBACK patch_font_faces(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    IDWriteFactory *factory = param;
    IDWriteFontFace *face;
    void **vtbl;
    DWORD old;
    (void)once; (void)ctx;

    if (!(face = any_font_face(factory))) {
        note("no system font face found; variable fonts stay as Wine renders them");
        return TRUE;
    }
    vtbl = *(void ***)face;
    if (vtbl[0] != (void *)fontface_qi && VirtualProtect(&vtbl[0], sizeof(void *), PAGE_READWRITE, &old)) {
        wine_fontface_qi = vtbl[0];
        vtbl[0] = (void *)fontface_qi;
        VirtualProtect(&vtbl[0], sizeof(void *), old, &old);
        note("active: font faces no longer offer IDWriteFontFace5, so variable web fonts use Chromium's FreeType");
    } else {
        note("could not change the font face method table (error %lu)", GetLastError());
    }
    IDWriteFontFace_Release(face);
    return TRUE;
}

__declspec(dllexport) HRESULT WINAPI DWriteCreateFactory(DWRITE_FACTORY_TYPE type, REFIID riid, IUnknown **out)
{
    IDWriteFactory *factory;
    HRESULT hr;

    InitOnceExecuteOnce(&load_once, load_wine_dwrite, NULL, NULL);
    if (!wine_create_factory) {
        if (out) *out = NULL;
        return E_FAIL;
    }
    hr = wine_create_factory(type, riid, out);
    if (SUCCEEDED(hr) && out && *out
        && SUCCEEDED(IUnknown_QueryInterface(*out, &IID_IDWriteFactory, (void **)&factory))) {
        InitOnceExecuteOnce(&patch_once, patch_font_faces, factory, NULL);
        IDWriteFactory_Release(factory);
    }
    return hr;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        self = inst;
        DisableThreadLibraryCalls(inst);
    }
    return TRUE;
}
