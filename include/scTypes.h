#ifndef SC_TYPES_H
#define SC_TYPES_H

#include <stdbool.h>

typedef unsigned long long u64;
typedef unsigned long      u32;
typedef unsigned short     u16;
typedef unsigned char      u8;

typedef signed long long   s64;
typedef signed long        s32;
typedef signed short       s16;
typedef signed char        s8;

typedef double f64;
typedef float  f32;

typedef struct {
  s32 x, y;
  s32 w, h;
} scRect;

typedef struct {
  s32 x, y;
} scV2I;

#define scInternal static
#define scGlobal   static

#define SC_PATH_MAX_LEN 255

#endif // SC_TYPES_H