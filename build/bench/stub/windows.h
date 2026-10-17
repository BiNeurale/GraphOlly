// Fake windows.h to compile layout.c on the host (Linux gcc), for the bench
// in build/bench. Only the types and the three calls layout.c really uses.
#ifndef GO_BENCH_WINDOWS_H
#define GO_BENCH_WINDOWS_H

#include <stddef.h>

typedef unsigned char  uchar;
typedef unsigned long  ulong;
typedef unsigned int   UINT;
typedef long           LONG;
typedef int            BOOL;
typedef void          *HDC,*HWND,*HINSTANCE;
typedef long           LRESULT;
typedef unsigned long  WPARAM;
typedef long           LPARAM;
#define CALLBACK

typedef struct { LONG cx,cy; } SIZE;
typedef struct { LONG left,top,right,bottom; } RECT;
typedef struct { LONG tmHeight,tmAveCharWidth; } TEXTMETRIC;

// Fake fixed font: 7x14, like Olly's Courier at 100%.
static inline BOOL GetTextMetrics(HDC h,TEXTMETRIC *tm) {
  (void)h; tm->tmHeight=14; tm->tmAveCharWidth=7; return 1;
}
static inline BOOL GetTextExtentPoint32(HDC h,const char *s,int n,SIZE *sz) {
  (void)h; (void)s; sz->cx=7*n; sz->cy=14; return 1;
}
ulong GetTickCount(void);

#endif
