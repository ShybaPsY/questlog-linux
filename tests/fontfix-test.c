/* Checks fontfix/dwrite.dll: run it with the stand-in dwrite.dll next to this program and
 * WINEDLLOVERRIDES=dwrite=n,b (that is how Questlog.exe gets it).
 *
 * A factory must still be created by Wine's DirectWrite, font faces must still answer
 * IDWriteFontFace .. IDWriteFontFace4, and must no longer answer IDWriteFontFace5. */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <dwrite_3.h>
#include <stdio.h>
#include <stdlib.h>

typedef HRESULT (WINAPI *create_fn)(DWRITE_FACTORY_TYPE, REFIID, IUnknown **);

static int has(IDWriteFontFace *face, REFIID riid)
{
    IUnknown *u = NULL;
    if (FAILED(IDWriteFontFace_QueryInterface(face, riid, (void **)&u)) || !u) return 0;
    IUnknown_Release(u);
    return 1;
}

int main(void)
{
    wchar_t mod_path[MAX_PATH] = L"?";
    IDWriteFactory *factory = NULL;
    IDWriteFontCollection *coll = NULL;
    IDWriteFontFamily *fam = NULL;
    IDWriteFont *font = NULL;
    IDWriteFontFace *face = NULL;
    HMODULE dw;
    create_fn create;
    int f4, f5, f0, ok;

    if (getenv("QL_TEST_OUT") && !freopen(getenv("QL_TEST_OUT"), "w", stdout)) return 3;
    setvbuf(stdout, NULL, _IONBF, 0);

    dw = LoadLibraryW(L"dwrite.dll");
    if (dw) GetModuleFileNameW(dw, mod_path, MAX_PATH);
    printf("dwrite.dll loaded from: %ls\n", mod_path);
    create = dw ? (create_fn)(void *)GetProcAddress(dw, "DWriteCreateFactory") : NULL;
    if (!create || FAILED(create(DWRITE_FACTORY_TYPE_SHARED, &IID_IDWriteFactory, (IUnknown **)&factory))) {
        printf("FAIL: no DirectWrite factory\n");
        return 1;
    }
    if (FAILED(IDWriteFactory_GetSystemFontCollection(factory, &coll, FALSE))
        || FAILED(IDWriteFontCollection_GetFontFamily(coll, 0, &fam))
        || FAILED(IDWriteFontFamily_GetFont(fam, 0, &font))
        || FAILED(IDWriteFont_CreateFontFace(font, &face))) {
        printf("FAIL: no font face from the system collection\n");
        return 1;
    }
    f0 = has(face, &IID_IDWriteFontFace);
    f4 = has(face, &IID_IDWriteFontFace4);
    f5 = has(face, &IID_IDWriteFontFace5);
    printf("font face answers: IDWriteFontFace %d, IDWriteFontFace4 %d, IDWriteFontFace5 %d (glyph count %u)\n",
           f0, f4, f5, IDWriteFontFace_GetGlyphCount(face));
    ok = f0 && f4 && !f5 && IDWriteFontFace_GetGlyphCount(face) > 0;
    printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
