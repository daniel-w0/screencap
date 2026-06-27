#ifndef GDIPLUS_FLAT_H
#define GDIPLUS_FLAT_H

#ifdef __cplusplus
extern "C" {
#endif

#ifndef WINGDIPAPI
#define WINGDIPAPI __stdcall
#endif

#ifndef GDIPCONST
#define GDIPCONST const
#endif

/* GDI+ coordinates are float. Your pch had `#define REAL int`, which silently
 * corrupted every coordinate -- kill it and use the correct type. */
#ifdef REAL
#undef REAL
#endif
typedef float REAL;

typedef DWORD ARGB;   /* 0xAARRGGBB */

/* ------------------------------------------------------------------ status */
typedef enum {
    Ok                        = 0,
    GenericError              = 1,
    InvalidParameter          = 2,
    OutOfMemory               = 3,
    ObjectBusy                = 4,
    InsufficientBuffer        = 5,
    NotImplemented            = 6,
    Win32Error                = 7,
    WrongState                = 8,
    Aborted                   = 9,
    FileNotFound              = 10,
    ValueOverflow             = 11,
    AccessDenied              = 12,
    UnknownImageFormat        = 13,
    FontFamilyNotFound        = 14,
    FontStyleNotFound         = 15,
    NotTrueTypeFont           = 16,
    UnsupportedGdiplusVersion = 17,
    GdiplusNotInitialized     = 18,
    PropertyNotFound          = 19,
    PropertyNotSupported      = 20,
    ProfileNotFound           = 21
} GpStatus;

/* ------------------------------------------------------------------- enums */
typedef enum { FillModeAlternate = 0, FillModeWinding = 1 } GpFillMode;

typedef enum {
    SmoothingModeInvalid     = -1,
    SmoothingModeDefault     = 0,
    SmoothingModeHighSpeed   = 1,
    SmoothingModeHighQuality = 2,
    SmoothingModeNone        = 3,
    SmoothingModeAntiAlias   = 4
} SmoothingMode;

typedef enum {
    PixelOffsetModeInvalid     = -1,
    PixelOffsetModeDefault     = 0,
    PixelOffsetModeHighSpeed   = 1,
    PixelOffsetModeHighQuality = 2,
    PixelOffsetModeNone        = 3,
    PixelOffsetModeHalf        = 4
} PixelOffsetMode;

typedef enum {
    UnitWorld      = 0,
    UnitDisplay    = 1,
    UnitPixel      = 2,
    UnitPoint      = 3,
    UnitInch       = 4,
    UnitDocument   = 5,
    UnitMillimeter = 6
} GpUnit;

/* --------------------------------------------------- concrete value structs */
typedef struct GpPointF { REAL X, Y; }                GpPointF;
typedef struct GpPoint  { INT  X, Y; }                GpPoint;
typedef struct GpRectF  { REAL X, Y, Width, Height; } GpRectF;
typedef struct GpRect   { INT  X, Y, Width, Height; } GpRect;

typedef struct GpPathData {
    INT       Count;
    GpPointF* Points;
    BYTE*     Types;
} GpPathData;

/* ----------------------------------------------------------- opaque handles */
typedef struct GpGraphics GpGraphics;
typedef struct GpPath     GpPath;
typedef struct GpBrush    GpBrush;
typedef GpBrush           GpSolidFill;   /* pointer-compatible with GpBrush */
typedef struct GpPen      GpPen;

/* ----------------------------------------------------------------- startup */
typedef struct GdiplusStartupInput {
    UINT32 GdiplusVersion;           /* set to 1 */
    void*  DebugEventCallback;       /* NULL */
    BOOL   SuppressBackgroundThread; /* FALSE */
    BOOL   SuppressExternalCodecs;   /* FALSE */
} GdiplusStartupInput;

typedef struct GdiplusStartupOutput {
    void* NotificationHook;
    void* NotificationUnhook;
} GdiplusStartupOutput;

GpStatus WINGDIPAPI GdiplusStartup(ULONG_PTR* token,
                                   GDIPCONST GdiplusStartupInput* input,
                                   GdiplusStartupOutput* output);
void     WINGDIPAPI GdiplusShutdown(ULONG_PTR token);

/* ------------------------------------------------------- flat API (in use) */
GpStatus WINGDIPAPI GdipCreateFromHDC(HDC hdc, GpGraphics** graphics);
GpStatus WINGDIPAPI GdipDeleteGraphics(GpGraphics* graphics);
GpStatus WINGDIPAPI GdipSetSmoothingMode(GpGraphics* graphics, SmoothingMode mode);
GpStatus WINGDIPAPI GdipSetPixelOffsetMode(GpGraphics* graphics, PixelOffsetMode mode);

GpStatus WINGDIPAPI GdipCreatePath(GpFillMode brushMode, GpPath** path);
GpStatus WINGDIPAPI GdipDeletePath(GpPath* path);
GpStatus WINGDIPAPI GdipAddPathArc(GpPath* path, REAL x, REAL y,
                                   REAL width, REAL height,
                                   REAL startAngle, REAL sweepAngle);
GpStatus WINGDIPAPI GdipClosePathFigure(GpPath* path);

GpStatus WINGDIPAPI GdipCreateSolidFill(ARGB color, GpSolidFill** brush);
GpStatus WINGDIPAPI GdipDeleteBrush(GpBrush* brush);

GpStatus WINGDIPAPI GdipCreatePen1(ARGB color, REAL width, GpUnit unit, GpPen** pen);
GpStatus WINGDIPAPI GdipDeletePen(GpPen* pen);

GpStatus WINGDIPAPI GdipFillPath(GpGraphics* graphics, GpBrush* brush, GpPath* path);
GpStatus WINGDIPAPI GdipDrawPath(GpGraphics* graphics, GpPen* pen, GpPath* path);
GpStatus WINGDIPAPI GdipFillEllipse(GpGraphics* graphics, GpBrush* brush,
                                    REAL x, REAL y, REAL width, REAL height);
GpStatus WINGDIPAPI GdipFillRectangleI(GpGraphics* graphics, GpBrush* brush,
                                       INT x, INT y, INT width, INT height);

#ifdef _MSC_VER
#pragma comment(lib, "gdiplus.lib")
#endif

/* =========================================================================
 *  Convenience helpers -- direct C ports of your C++ wrappers.
 *  These let your draw_* functions stay almost identical.
 * ========================================================================= */

/* COLORREF (0x00BBGGRR) + alpha  ->  ARGB (0xAARRGGBB). Replaces gpc(). */
static __inline ARGB gpColor(COLORREF c, BYTE a) {
    return ((ARGB)a            << 24) |
           ((ARGB)GetRValue(c) << 16) |
           ((ARGB)GetGValue(c) <<  8) |
            (ARGB)GetBValue(c);
}

/* Create a Graphics from an HDC with your standard AA + pixel-offset setup.
 * Replaces:  Gdiplus::Graphics g(dc); gp_setup(g);
 * Pair every gpg_begin() with gpg_end(). */
static __inline GpGraphics* gpGraphicsBegin(HDC dc) {
    GpGraphics* g = 0;
    GdipCreateFromHDC(dc, &g);
    if (g) {
        GdipSetSmoothingMode(g, SmoothingModeAntiAlias);
        GdipSetPixelOffsetMode(g, PixelOffsetModeHalf);
    }
    return g;
}
static __inline void gpGraphicsEnd(GpGraphics* g) {
    if (g) GdipDeleteGraphics(g);
}

/* internal: trace a rounded-rect figure (same math as your make_round_path) */
static __inline void gp__round_figure(GpPath* path, float x, float y,
                                      float w, float h, float rad) {
    float d = rad * 2.0f;
    if (d > w) d = w;
    if (d > h) d = h;
    GdipAddPathArc(path, x,             y,             d, d, 180.0f, 90.0f);
    GdipAddPathArc(path, x + w - d,     y,             d, d, 270.0f, 90.0f);
    GdipAddPathArc(path, x + w - d,     y + h - d,     d, d,   0.0f, 90.0f);
    GdipAddPathArc(path, x,             y + h - d,     d, d,  90.0f, 90.0f);
    GdipClosePathFigure(path);
}

/* Filled rounded rect. Replaces gp_fill_round(g, r, rad, color). */
static __inline void gpFillRound(GpGraphics* g, RECT r, float rad, ARGB c) {
    GpPath*  path  = 0;
    GpBrush* brush = 0;
    GdipCreatePath(FillModeAlternate, &path);
    gp__round_figure(path, (float)r.left, (float)r.top,
                     (float)(r.right - r.left), (float)(r.bottom - r.top), rad);
    GdipCreateSolidFill(c, &brush);
    GdipFillPath(g, brush, path);
    GdipDeleteBrush(brush);
    GdipDeletePath(path);
}

/* Stroked rounded rect (note the -1 inset, matching your original).
 * Replaces gp_stroke_round(g, r, rad, color, width). width has no default in C. */
static __inline void gp_stroke_round(GpGraphics* g, RECT r, float rad,
                                     ARGB c, float width) {
    GpPath* path = 0;
    GpPen*  pen  = 0;
    GdipCreatePath(FillModeAlternate, &path);
    gp__round_figure(path, (float)r.left, (float)r.top,
                     (float)(r.right - r.left) - 1.0f,
                     (float)(r.bottom - r.top) - 1.0f, rad);
    GdipCreatePen1(c, width, UnitWorld, &pen);
    GdipDrawPath(g, pen, path);
    GdipDeletePen(pen);
    GdipDeletePath(path);
}

/* Filled ellipse. Replaces: SolidBrush b(c); g.FillEllipse(&b, x,y,w,h); */
static __inline void gp_fill_ellipse(GpGraphics* g, float x, float y,
                                     float w, float h, ARGB c) {
    GpBrush* b = 0;
    GdipCreateSolidFill(c, &b);
    GdipFillEllipse(g, b, x, y, w, h);
    GdipDeleteBrush(b);
}

/* Filled rect (integer). Replaces the sidebar FillRectangle call. */
static __inline void gpFillRectI(GpGraphics* g, int x, int y,
                                    int w, int h, ARGB c) {
    GpBrush* b = 0;
    GdipCreateSolidFill(c, &b);
    GdipFillRectangleI(g, b, x, y, w, h);
    GdipDeleteBrush(b);
}

/* One-call startup. Replaces the GdiplusStartupInput/GdiplusStartup pair. */
static __inline void gp_startup(ULONG_PTR* token) {
    GdiplusStartupInput in;
    in.GdiplusVersion           = 1;
    in.DebugEventCallback       = 0;
    in.SuppressBackgroundThread = FALSE;
    in.SuppressExternalCodecs   = FALSE;
    GdiplusStartup(token, &in, 0);
}

#ifdef __cplusplus
}
#endif

#endif /* GDIPLUS_FLAT_H */