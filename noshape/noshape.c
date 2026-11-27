/* noshape.so: preloaded into Questlog's Wine process only (never the game), see questlog-linux.
 *
 * Questlog's overlay widgets are transparent layered windows. Wine clips each one with an X
 * bounding shape around its visible pixels. Under Hyprland (XWayland) the area outside that
 * shape is shown as opaque black, so every widget sits in a black box, while the inside blends
 * correctly. The pixels outside the shape are already fully transparent (alpha 0), so the
 * bounding shape adds nothing visually: this library drops bounding shapes (the window stays
 * unshaped) and passes input shapes through untouched, so clicks still go through to the game
 * exactly as before.
 *
 * Wine's ShapeLayeredWindows=N registry setting does not stop these shapes (tested with
 * Proton 11), hence the preload. Wine's X11 driver links XShapeCombine* directly from libXext,
 * so symbol interposition with LD_PRELOAD works.
 *
 * Self-contained on purpose: no X11 development headers are needed to build it. */
#define _GNU_SOURCE
#include <dlfcn.h>

typedef struct _XDisplay Display;
typedef unsigned long XID;
typedef struct { short x, y; unsigned short width, height; } XRectangle;

#define ShapeSet      0
#define ShapeBounding 0
#define None          0UL

typedef void (*combine_mask_fn)(Display *, XID, int, int, int, XID, int);
typedef void (*combine_rects_fn)(Display *, XID, int, int, int, XRectangle *, int, int, int);

static combine_mask_fn real_combine_mask(void)
{
    static combine_mask_fn fn;
    if (!fn) fn = (combine_mask_fn)dlsym(RTLD_NEXT, "XShapeCombineMask");
    return fn;
}

void XShapeCombineMask(Display *dpy, XID win, int kind, int x, int y, XID mask, int op)
{
    if (kind == ShapeBounding) { mask = None; op = ShapeSet; }   /* unshaped */
    real_combine_mask()(dpy, win, kind, x, y, mask, op);
}

void XShapeCombineRectangles(Display *dpy, XID win, int kind, int x, int y,
                             XRectangle *rects, int n, int op, int ordering)
{
    static combine_rects_fn real;
    if (kind == ShapeBounding) {
        real_combine_mask()(dpy, win, ShapeBounding, 0, 0, None, ShapeSet);
        return;
    }
    if (!real) real = (combine_rects_fn)dlsym(RTLD_NEXT, "XShapeCombineRectangles");
    real(dpy, win, kind, x, y, rects, n, op, ordering);
}
