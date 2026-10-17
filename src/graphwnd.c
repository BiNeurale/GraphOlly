////////////////////////////////////////////////////////////////////////////////
// GraphOlly - MDI graph window.
//
// The class is registered with Registerpluginclass() (it formally belongs to
// the OllyDbg instance, not to the DLL), the MDI child is created inside Olly's
// MDI client, Plugingetvalue(VAL_HWCLIENT). From there on it is hand-written GDI.
//
// Fonts and colours are NOT reinvented: they come from Olly through
// Plugingetvalue(VAL_FONTS) and VAL_COLORS, so the graph follows the scheme
// the user chose in ollydbg.ini.
//
// TWO COORDINATE SYSTEMS, never to be mixed up:
//   graph pixels   those of the layout, independent of zoom. block->x/y/dx/dy,
//                  edge paths, gw->ox/oy and the scroll bars live here.
//   client pixels  those of the window. Reached only at drawing time, with
//                  GOCX()/GOCY(), and converted back with GOGX()/GOGY() when a
//                  click arrives.
// Text metrics (gw->cw0, gw->ch0) are ALWAYS the unscaled ones: the layout
// measured the blocks with them, and they are what maps the y of a click back
// to the line that was hit.
//
// Edge paths are computed by layout.c: here the polyline that arrives ready
// in t_gograph.route is just drawn.
////////////////////////////////////////////////////////////////////////////////

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plugin_packed.h"
#include "grapholly.h"

char             gographclass[32];     // Name of the class registered by Olly

#define GO_NZFONT      6               // Per-zoom fonts kept in cache

typedef struct t_gownd {               // State of one graph window
  t_gograph      *graph;
  int            ox,oy;                // View origin, in GRAPH PIXELS
  int            zoom;                 // Percentage: 100 = one to one
  int            dragging;
  POINT          dragorg;
  int            dragox,dragoy;
  int            curblock;             // Block under the cursor (-1 = none)
  int            curline;              // Line under the cursor within that block
  int            hotblock;             // Block under the mouse (-1 = none)
  // Where we were before a "follow": Backspace goes back. Without it,
  // following a jump in a large function means getting lost.
  int            histblock[32],histline[32],nhist;
  int            eipblock;             // Block containing EIP (-1 = none)
  ulong          eip;                  // EIP at the last pause, 0 if not running
  // Per-zoom fonts: a fixed TrueType created at the exact pixel height, not
  // Olly's font stretched (if it is a bitmap font, stretching makes it grainy).
  // A few are kept: the wheel goes back and forth over the same zoom levels.
  HFONT          zfont[GO_NZFONT];
  int            zfontzoom[GO_NZFONT]; // Zoom of each one (0 = free)
  int            zfontnext;            // Next one to replace
  HFONT          lfont[2];             // Addresses in the overview
  int            lfontcw[2],lfontch[2];
  char           ttface[LF_FACESIZE];  // Chosen TrueType face ("" = to be found)
  int            ttweight;             // Weight of Olly's font
  int            cw0,ch0;              // Metrics of the UNSCALED font
  // GDI objects created once instead of on every WM_PAINT: during a drag
  // there are dozens of WM_PAINT per second.
  HPEN           pentrue,penfalse,penjmp,pencall,penbox,pencur,peneip;
  HPEN           pentrue2,penfalse2,penjmp2;   // The same, thick
  HBRUSH         brtrue,brfalse,brjmp,brcall;
  HBRUSH         bkbrush,eipbrush,hotbrush,selbrush,ovbrush;
  HBRUSH         bpbrush,bpdisbrush,curbrush;
  HBRUSH         findbrush;            // Background of lines matched by search
  ulong          bpsig;                // Breakpoint signature at the last paint
  int            npaint;               // Counter for the trace
  double         paintus;              // Total drawing time, microseconds
  // Double buffer KEPT between paints: creating one on every WM_PAINT cost a
  // window-sized bitmap dozens of times per second while panning. And since it
  // stays valid, a partial repaint (the block under the mouse) redoes only the
  // invalidated rectangle.
  HDC            bbdc;
  HBITMAP        bbbmp,bbold;
  int            bbw,bbh;
  // Search: text searched for (already lowercase) and which blocks contain it.
  // The per-block marks are for the overview and the minimap, where lines are
  // not visible.
  char           find[TEXTLEN];
  int            nfound;               // Lines that contain it
  uchar          findhit[GO_MAXBLOCKS];
  // Minimap: the image of the whole graph, recomputed only when the layout or
  // the search changes. On each paint only the view rectangle is drawn on top.
  HBITMAP        mapbmp;
  int            mapw,maph;            // Size of the cached image
  int            mapok;                // 0 = to be redone
  RECT           maprc;                // Where it is in the window (empty = hidden)
  double         mapscale;             // Map pixels per graph pixel
  int            mapdrag;              // A drag inside the minimap is in progress
} t_gownd;

#define GO_TIMERID     1
#define GO_TIMERMS     400             // How often to check whether something
                                       // changed outside the graph

// Graph pixels <-> client pixels. The computation goes through a double (a
// layout reaches 18000x54000 and zoom 400%: no 32-bit products, and no 64-bit
// divisions that would drag in libgcc), and always rounds down, also left of
// or above the view: with truncation towards zero pixel -1 and pixel 0 landed
// on the same coordinate, and drawing and clicking disagreed right on the
// edge. The double is exact: an integer quotient comes out integer, and a
// non-integer one is at least 1/den away from it.
static int Goscale(int v,int num,int den) {
  double q;
  int r;
  q=(double)v*num/den;
  if (q> 2.0e9) return  2000000000;
  if (q<-2.0e9) return -2000000000;
  r=(int)q;
  if ((double)r>q) r--;
  return r;
};
#define GOCX(gw,x)  Goscale((x)-(gw)->ox,(gw)->zoom,100)
#define GOCY(gw,y)  Goscale((y)-(gw)->oy,(gw)->zoom,100)
#define GOCL(gw,v)  Goscale((v),(gw)->zoom,100)       // length only
#define GOGX(gw,x)  (Goscale((x),100,(gw)->zoom)+(gw)->ox)
#define GOGY(gw,y)  (Goscale((y),100,(gw)->zoom)+(gw)->oy)

// Context menu items.
#define GOM_CPU        1
#define GOM_EXPAND     2
#define GOM_ZOOMIN     3
#define GOM_ZOOMOUT    4
#define GOM_ZOOM100    5
#define GOM_FIT        6
#define GOM_FOLLOWEIP  7
#define GOM_GOEIP      8
#define GOM_COPY       9
#define GOM_ENTRY     10
#define GOM_FOLLOW    11
#define GOM_BACK      12
#define GOM_BP        13
#define GOM_FOLLOWFUN 14
#define GOM_FIND      15
#define GOM_FINDNEXT  16
#define GOM_FINDPREV  17
#define GOM_MINIMAP   18
#define GOM_EXPORT    19

// Minimap: maximum size and margin from the edge, in client pixels.
#define GO_MAPMAXW     240
#define GO_MAPMAXH     200
#define GO_MAPGAP      8

// Export: ceiling on the bitmap. At 24 bits 16 Mpixel is 48 MB, already a lot
// for the address space of a 32-bit process; beyond that the scale is reduced
// instead of risking an allocation that fails halfway.
#define GO_EXPMAXPIX   (16*1024*1024)
#define GO_EXPMAXSIDE  16384
#define GO_EXPMARGIN   16              // White border around the graph

// GetTickCount has 15 ms granularity: a paint takes a fraction of that and
// measured this way always comes out 0. The high-resolution counter is needed.
static double Gousec(void) {
  LARGE_INTEGER c,f;
  if (!QueryPerformanceCounter(&c) || !QueryPerformanceFrequency(&f) || f.QuadPart==0)
    return (double)GetTickCount()*1000.0;
  return (double)c.QuadPart*1000000.0/(double)f.QuadPart;
};

// Breakpoints are also set from the Disassembler, and then the graph receives
// no message: it would keep showing the old ones until it is moved. Instead of
// redrawing blindly twice a second, a SIGNATURE of the breakpoint table is
// computed and the graph is redrawn only if it changed.
static ulong Gobpsignature(void) {
  ulong a,t,sig;
  int cmd,n;
  a=0; sig=0; n=0;
  while (n<8192) {
    t=0; cmd=0;
    a=Getnextbreakpoint(a,&t,&cmd);
    if (a==0)
      break;
    sig=sig*31+(a^t);
    n++;
  };
  return sig+(ulong)n;
};

static void Gomakegdi(t_gownd *gw);
static void Goupdatescroll(HWND hw,t_gownd *gw);
static void Goshowblock(HWND hw,t_gownd *gw,int i);
static void Goclamp(HWND hw,t_gownd *gw);
static void Goshowline(HWND hw,t_gownd *gw,int i,int line);
static void Gofindmark(t_gownd *gw);

// Olly's fixed font, or fallback to a fixed system font.
static HFONT Gogetfont(void) {
  HFONT *fonts;
  fonts=(HFONT *)Plugingetvalue(VAL_FONTS);
  if (fonts!=NULL && fonts[FIXEDFONT]!=NULL)
    return fonts[FIXEDFONT];
  return (HFONT)GetStockObject(ANSI_FIXED_FONT);
};

// Is text drawn at this zoom? What counts is the line height on screen, not
// the zoom: with a larger Olly font text is readable at smaller zoom too.
// Drawing, clicks and search all ask here, so a line that is not visible is
// never hit.
static int Gohastext(t_gownd *gw) {
  return GOCL(gw,gw->ch0)>=GO_TEXTMINPX;
};

// Fixed-pitch TrueType faces, in the order they are tried if Olly's font is
// not TrueType. The last two are the ones wine usually finds.
static const char *gottfaces[]={
  "Lucida Console","Consolas","Courier New","Liberation Mono","DejaVu Sans Mono"
};
#define GO_NTTFACES  ((int)(sizeof(gottfaces)/sizeof(gottfaces[0])))

// A fixed-pitch, antialiased TrueType with a line "cellh" pixels tall and a
// character no wider than "cellw": this way it fits the column the layout
// measured with Olly's font. If the face is wider than that it is narrowed with
// lfWidth, which for a TrueType means redrawing the outline, not stretching
// pixels. *pcw and *pch receive the real metrics.
static HFONT Gottfont(t_gownd *gw,int cellh,int cellw,int *pcw,int *pch) {
  LOGFONT lf,of;
  HDC dc;
  HFONT f,old;
  TEXTMETRIC tm;
  SIZE sz;
  char face[LF_FACESIZE];
  int k,ok;
  if (cellh<1) cellh=1;
  if (cellw<1) cellw=1;
  *pcw=cellw; *pch=cellh;
  dc=GetDC(NULL);
  if (dc==NULL)
    return NULL;
  if (gw->ttweight==0) {
    // First time: if Olly's font is already TrueType its face is used, so the
    // graph keeps what the user chose.
    gw->ttweight=FW_NORMAL;
    if (GetObject(Gogetfont(),sizeof(of),&of)!=0 && of.lfWeight>0)
      gw->ttweight=of.lfWeight;
    old=(HFONT)SelectObject(dc,Gogetfont());
    if (GetTextMetrics(dc,&tm) && (tm.tmPitchAndFamily & TMPF_TRUETYPE)!=0)
      GetTextFace(dc,LF_FACESIZE,gw->ttface);
    SelectObject(dc,old);
  };
  memset(&lf,0,sizeof(lf));
  lf.lfHeight=cellh;                   // Positive: height of the CELL
  lf.lfWeight=gw->ttweight;
  lf.lfCharSet=DEFAULT_CHARSET;
  lf.lfOutPrecision=OUT_TT_ONLY_PRECIS;
  lf.lfClipPrecision=CLIP_DEFAULT_PRECIS;
  // Below 10 pixels antialiasing smears one-pixel strokes over two grey
  // pixels: on the pink of a block the text became a haze (seen on Windows 7
  // at 45%). Small text is drawn with solid pixels, which are readable.
  lf.lfQuality=(cellh<10 ? NONANTIALIASED_QUALITY : ANTIALIASED_QUALITY);
  lf.lfPitchAndFamily=FIXED_PITCH|FF_MODERN;
  f=NULL;
  // k=-1 the face already chosen, then the list, and last (k=GO_NTTFACES)
  // any fixed TrueType the system is willing to give.
  for (k=-1; k<=GO_NTTFACES && f==NULL; k++) {
    if (k<0 && gw->ttface[0]=='\0')
      continue;
    if (k<0)
      lstrcpyn(lf.lfFaceName,gw->ttface,LF_FACESIZE);
    else if (k<GO_NTTFACES)
      lstrcpyn(lf.lfFaceName,gottfaces[k],LF_FACESIZE);
    else
      lf.lfFaceName[0]='\0';
    lf.lfWidth=0;
    f=CreateFontIndirect(&lf);
    if (f==NULL)
      continue;
    old=(HFONT)SelectObject(dc,f);
    face[0]='\0';
    GetTextFace(dc,LF_FACESIZE,face);
    // Ask for a face that is not there and Windows silently gives another
    // one: the name gives it away.
    ok=(GetTextMetrics(dc,&tm) && (tm.tmPitchAndFamily & TMPF_TRUETYPE)!=0 &&
        (tm.tmPitchAndFamily & TMPF_FIXED_PITCH)==0 &&   // bit 0 = fixed
        (lf.lfFaceName[0]=='\0' || lstrcmpi(face,lf.lfFaceName)==0));
    if (ok && GetTextExtentPoint32(dc,"0",1,&sz) && sz.cx>cellw) {
      SelectObject(dc,old);
      DeleteObject(f);
      lf.lfWidth=cellw;
      f=CreateFontIndirect(&lf);
      if (f==NULL)
        continue;
      old=(HFONT)SelectObject(dc,f);
    };
    if (!ok) {
      SelectObject(dc,old);
      DeleteObject(f);
      f=NULL;
      continue;
    };
    lstrcpyn(gw->ttface,face,LF_FACESIZE);   // Next times, straight away
    if (GetTextMetrics(dc,&tm)) *pch=tm.tmHeight;
    if (GetTextExtentPoint32(dc,"0",1,&sz)) *pcw=sz.cx;
    SelectObject(dc,old);
  };
  if (f==NULL) {
    // No fixed TrueType: Olly's font recreated at that height, which is what
    // was done before. Better than nothing.
    if (GetObject(Gogetfont(),sizeof(of),&of)!=0) {
      of.lfHeight=cellh;
      of.lfWidth=cellw;
      f=CreateFontIndirect(&of);
    };
  };
  ReleaseDC(NULL,dc);
  return f;
};

// The text font at the current zoom. At 100% it is Olly's; otherwise the cache
// is searched and a new one is created only if missing: a CreateFontIndirect on
// every WM_PAINT would cost more than all the rest of the drawing.
static HFONT Gozoomfont(t_gownd *gw) {
  HFONT f;
  int k,cw,ch;
  if (gw->zoom==100)
    return Gogetfont();
  for (k=0; k<GO_NZFONT; k++)
    if (gw->zfont[k]!=NULL && gw->zfontzoom[k]==gw->zoom)
      return gw->zfont[k];
  f=Gottfont(gw,GOCL(gw,gw->ch0),GOCL(gw,gw->cw0),&cw,&ch);
  if (f==NULL)
    return Gogetfont();
  k=gw->zfontnext;
  gw->zfontnext=(k+1)%GO_NZFONT;
  if (gw->zfont[k]!=NULL)
    DeleteObject(gw->zfont[k]);
  gw->zfont[k]=f;
  gw->zfontzoom[k]=gw->zoom;
  Gotrace("  font for zoom %d%%: %s, cell %dx%d",gw->zoom,gw->ttface,cw,ch);
  return f;
};

// Address fonts in the overview: k=0 the comfortable one, k=1 the smallest
// still readable, for short blocks. Fixed, they do not follow the zoom.
static HFONT Golabelfont(t_gownd *gw,int k) {
  int h;
  if (gw->lfont[k]==NULL) {
    h=(k==0 ? GO_LABELPX : GO_TEXTMINPX+1);
    gw->lfont[k]=Gottfont(gw,h,h,&gw->lfontcw[k],&gw->lfontch[k]);
  };
  return gw->lfont[k];
};

// Olly's common colour by index (BLACK, WHITE, ...), with fallback.
static COLORREF Gogetcolor(int index,COLORREF def) {
  COLORREF *colors;
  int ncolors;
  ncolors=Plugingetvalue(VAL_NCOLORS);
  colors=(COLORREF *)Plugingetvalue(VAL_COLORS);
  if (colors!=NULL && index>=0 && index<ncolors)
    return colors[index];
  return def;
};

// Blend towards white: used for the light tints (block under the mouse, chosen
// block) that must read UNDER black text without covering it.
static COLORREF Gopale(COLORREF c,int perc) {
  int r,g,b;
  r=GetRValue(c); g=GetGValue(c); b=GetBValue(c);
  r=255-(255-r)*perc/100;
  g=255-(255-g)*perc/100;
  b=255-(255-b)*perc/100;
  return RGB(r,g,b);
};

static void Goupdatescroll(HWND hw,t_gownd *gw) {
  SCROLLINFO si;
  RECT rc;
  if (gw->graph==NULL)
    return;
  GetClientRect(hw,&rc);
  memset(&si,0,sizeof(si));
  si.cbSize=sizeof(si);
  si.fMask=SIF_RANGE|SIF_PAGE|SIF_POS;
  // The bars work in graph pixels: the page is how much of the graph fits in
  // the window, and that depends on the zoom.
  // The range is closed: nMax is the LAST pixel, not the width. With the
  // width the thumb went one pixel beyond what Goclamp allows.
  si.nMin=0;
  si.nMax=gw->graph->width-1;
  si.nPage=GOGX(gw,rc.right)-gw->ox;
  si.nPos=gw->ox;
  SetScrollInfo(hw,SB_HORZ,&si,TRUE);
  si.nMax=gw->graph->height-1;
  si.nPage=GOGY(gw,rc.bottom)-gw->oy;
  si.nPos=gw->oy;
  SetScrollInfo(hw,SB_VERT,&si,TRUE);
};

// Keeps the origin within the graph bounds. If the whole graph fits, it sticks
// to the top-left instead of leaving the view wandering in the void.
static void Goclamp(HWND hw,t_gownd *gw) {
  RECT rc;
  int pw,ph;
  GetClientRect(hw,&rc);
  pw=GOGX(gw,rc.right)-gw->ox;
  ph=GOGY(gw,rc.bottom)-gw->oy;
  if (gw->ox>gw->graph->width -pw) gw->ox=gw->graph->width -pw;
  if (gw->oy>gw->graph->height-ph) gw->oy=gw->graph->height-ph;
  if (gw->ox<0) gw->ox=0;
  if (gw->oy<0) gw->oy=0;
};

// Changes zoom keeping the point (cx,cy) of the window fixed: that is what
// makes wheel zoom feel natural, because what you were looking at does not run
// away.
static void Gosetzoom(HWND hw,t_gownd *gw,int z,int cx,int cy) {
  int gx,gy;
  if (z<GO_ZOOMMIN) z=GO_ZOOMMIN;
  if (z>GO_ZOOMMAX) z=GO_ZOOMMAX;
  if (z==gw->zoom)
    return;
  gx=GOGX(gw,cx);
  gy=GOGY(gw,cy);
  gw->zoom=z;
  gw->ox=gx-cx*100/z;
  gw->oy=gy-cy*100/z;
  Goclamp(hw,gw);
  Goupdatescroll(hw,gw);
  InvalidateRect(hw,NULL,FALSE);
};

// Zoom anchored to the block under the cursor instead of the window centre.
// On a large graph the window centre is almost always empty space between two
// columns: pressing "0" left you staring at white.
static void Gozoomcur(HWND hw,t_gownd *gw,int z) {
  Gosetzoom(hw,gw,z,0,0);
  if (gw->curblock>=0)
    Goshowblock(hw,gw,gw->curblock);
  InvalidateRect(hw,NULL,FALSE);
};

// Zoom that fits the whole graph in the window.
static void Gofit(HWND hw,t_gownd *gw) {
  RECT rc;
  int zx,zy,z;
  if (gw->graph==NULL || gw->graph->width<=0 || gw->graph->height<=0)
    return;
  GetClientRect(hw,&rc);
  zx=rc.right *100/gw->graph->width;
  zy=rc.bottom*100/gw->graph->height;
  z=(zx<zy ? zx : zy);
  if (z>100) z=100;                    // "Fit" never enlarges
  gw->ox=gw->oy=0;
  if (z<GO_ZOOMMIN) {
    // The graph does not fit even at the smallest scale: instead of showing an
    // unreadable postage stamp, fit the WIDTH only and move to the cursor. On
    // a 342-level ladder function it is the only sensible thing to show.
    z=(zx<GO_ZOOMMIN ? GO_ZOOMMIN : zx);
    if (z>100) z=100;
    gw->zoom=z;
    Goclamp(hw,gw);
    if (gw->curblock>=0)
      Goshowblock(hw,gw,gw->curblock);
    Goupdatescroll(hw,gw);
    InvalidateRect(hw,NULL,FALSE);
    return;
  };
  gw->zoom=z;
  Goclamp(hw,gw);
  Goupdatescroll(hw,gw);
  InvalidateRect(hw,NULL,FALSE);
};

// Draws the edge and makes it explicit: the arrowhead is solid and wide, and
// below the start port of a conditional jump there is a T or an F. Colour
// alone is not enough for tired eyes or a badly tuned screen, and in black
// and white it would say nothing at all.
static void Godrawedge(HDC hdc,t_gownd *gw,int e,int withlabel) {
  t_goroute *r;
  POINT p[GO_MAXPT],tip[3];
  int i,x2,y2,d;
  char c;
  r=&(gw->graph->route[e]);
  if (r->np<2)
    return;
  for (i=0; i<r->np && i<GO_MAXPT; i++) {
    p[i].x=GOCX(gw,r->x[i]);
    p[i].y=GOCY(gw,r->y[i]);
  };
  Polyline(hdc,p,r->np);
  // Solid arrowhead on the destination, as large as the zoom allows.
  d=GOCL(gw,11);
  if (d<4) d=4;
  x2=p[r->np-1].x; y2=p[r->np-1].y;
  tip[0].x=x2;         tip[0].y=y2;
  tip[1].x=x2-d*5/12;  tip[1].y=y2-d;
  tip[2].x=x2+d*5/12;  tip[2].y=y2-d;
  Polygon(hdc,tip,3);
  if (!withlabel || !golabels)
    return;                            // Colour and port give the direction
  c='\0';
  if (gw->graph->edge[e].type==GO_E_TRUE)  c='T';
  if (gw->graph->edge[e].type==GO_E_FALSE) c='F';
  if (c!='\0')
    TextOut(hdc,p[0].x+3,p[0].y+1,&c,1);
};

// Does an edge touch the view? Its extent is the rectangle containing the
// whole path. Skipping it avoids dozens of off-screen Polyline calls on a
// large function.
static int Goedgevisible(t_gownd *gw,int e,RECT *rc) {
  t_goroute *r;
  int i,l,t,rr,bb;
  r=&(gw->graph->route[e]);
  if (r->np<2)
    return 0;
  l=rr=r->x[0]; t=bb=r->y[0];
  for (i=1; i<r->np; i++) {
    if (r->x[i]<l)  l=r->x[i];
    if (r->x[i]>rr) rr=r->x[i];
    if (r->y[i]<t)  t=r->y[i];
    if (r->y[i]>bb) bb=r->y[i];
  };
  l=GOCX(gw,l); rr=GOCX(gw,rr); t=GOCY(gw,t); bb=GOCY(gw,bb);
  i=8+GOCL(gw,11);                     // The arrowhead sticks out of the path
  return !(rr<rc->left-i || bb<rc->top-i || l>rc->right+i || t>rc->bottom+i);
};

// Pen and brush of an edge. "thick" is for the edges of the chosen block,
// which must be read before the others.
static void Goedgegdi(HDC hdc,t_gownd *gw,int type,int thick) {
  // "Thick" only where there is room: below 70% four pixels of edge covered
  // the blocks instead of making them stand out.
  if (thick && gw->zoom<70)
    thick=0;
  switch (type) {
    case GO_E_TRUE:
      SelectObject(hdc,(thick ? gw->pentrue2 : gw->pentrue));
      SelectObject(hdc,gw->brtrue);
      SetTextColor(hdc,Gogetcolor(GREEN,RGB(0,128,0)));
      break;
    case GO_E_FALSE:
      SelectObject(hdc,(thick ? gw->penfalse2 : gw->penfalse));
      SelectObject(hdc,gw->brfalse);
      SetTextColor(hdc,Gogetcolor(RED,RGB(192,0,0)));
      break;
    case GO_E_CALL:
      SelectObject(hdc,gw->pencall);
      SelectObject(hdc,gw->brcall);
      break;
    default:
      SelectObject(hdc,(thick ? gw->penjmp2 : gw->penjmp));
      SelectObject(hdc,gw->brjmp);
      break;
  };
};

// Case-insensitive search: "s" is a graph line, "lc" the searched text
// ALREADY lowercase.
static int Gomatch(const char *s,const char *lc) {
  const uchar *p,*q,*r;
  if (lc[0]=='\0')
    return 0;
  for (p=(const uchar *)s; *p!='\0'; p++) {
    for (q=p,r=(const uchar *)lc; *r!='\0' && *q!='\0'; q++,r++) {
      if ((*q>='A' && *q<='Z' ? *q-'A'+'a' : *q)!=*r)
        break;
    };
    if (*r=='\0')
      return 1;
  };
  return 0;
};

// Draws the graph into the given DC with the window's origin and zoom,
// limited to what falls in "clip" (client pixels). Used both by the window and
// by image export: a single drawing path, so the saved file is exactly what
// is on screen.
static void Godrawgraph(HDC mem,t_gownd *gw,RECT *clip) {
  HFONT oldfont;
  HPEN oldpen;
  HBRUSH oldbrush;
  RECT r,hr;
  RECT in;
  int i,j,k,n,ch,j0,j1,chosen,overview,searching,tx,ty,x0,ly;
  int dx[GO_LINELEN];
  ulong bp;
  t_goblock *b;
  t_gograph *g;
  t_goline *ln;
  char label[32];

  g=gw->graph;
  // Below a certain scale text is a grey line and cannot be read: switch to
  // the overview, solid blocks only. That is how a large function is looked
  // at from afar to figure out where to go.
  overview=!Gohastext(gw);
  ch=GOCL(gw,gw->ch0);
  if (ch<1) ch=1;
  searching=(gw->find[0]!='\0' && gw->nfound>0);

  oldfont=(HFONT)SelectObject(mem,(overview ? Gogetfont() : Gozoomfont(gw)));
  SetBkMode(mem,TRANSPARENT);
  SetTextColor(mem,Gogetcolor(BLACK,RGB(0,0,0)));

  // Edges below the blocks. First the normal ones, then those of the chosen
  // block: this way its edges stay ON TOP and can be followed through the rest.
  oldpen=(HPEN)SelectObject(mem,gw->penjmp);
  oldbrush=(HBRUSH)SelectObject(mem,gw->brjmp);
  chosen=gw->curblock;
  if (g->route!=NULL) {
    for (i=0; i<g->nedge && i<g->nroute; i++) {
      if (g->edge[i].from==chosen || g->edge[i].to==chosen)
        continue;
      if (!Goedgevisible(gw,i,clip))
        continue;
      Goedgegdi(mem,gw,g->edge[i].type,0);
      Godrawedge(mem,gw,i,!overview);
    };
    for (i=0; i<g->nedge && i<g->nroute; i++) {
      if (g->edge[i].from!=chosen && g->edge[i].to!=chosen)
        continue;
      if (!Goedgevisible(gw,i,clip))
        continue;
      Goedgegdi(mem,gw,g->edge[i].type,1);
      Godrawedge(mem,gw,i,!overview);
    };
  };
  SetTextColor(mem,Gogetcolor(BLACK,RGB(0,0,0)));

  // Blocks: only those intersecting the view are drawn, and of each only the
  // lines that fall inside it (a block can have 128).
  for (i=0; i<g->nblock; i++) {
    b=&(g->block[i]);
    // Each edge is converted on its own: r.left+GOCL(dx) rounds twice, and
    // the drawn block no longer matched the one a click hits.
    r.left  =GOCX(gw,b->x);
    r.top   =GOCY(gw,b->y);
    r.right =GOCX(gw,b->x+b->dx);
    r.bottom=GOCY(gw,b->y+b->dy);
    // The margin covers the thick frame and, below, the unresolved-exit
    // label.
    if (r.right+4<clip->left || r.bottom+ch+4<clip->top ||
        r.left-4>clip->right || r.top-4>clip->bottom)
      continue;
    // Background: tells at a glance where you are (EIP), what you chose and
    // what is under the mouse. Light tints, because black text sits on top.
    if (i==gw->eipblock)      SelectObject(mem,gw->eipbrush);
    else if (i==chosen)       SelectObject(mem,gw->selbrush);
    else if (i==gw->hotblock) SelectObject(mem,gw->hotbrush);
    else if (overview && searching && gw->findhit[i]) SelectObject(mem,gw->findbrush);
    else if (overview)        SelectObject(mem,gw->ovbrush);
    else                      SelectObject(mem,GetStockObject(HOLLOW_BRUSH));
    if (i==gw->eipblock)      SelectObject(mem,gw->peneip);
    else if (i==chosen)       SelectObject(mem,gw->pencur);
    else                      SelectObject(mem,gw->penbox);
    Rectangle(mem,r.left,r.top,r.right,r.bottom);

    // Inside the frame: text never leaves it, not even at 400%.
    in.left=r.left+1; in.top=r.top+1; in.right=r.right-1; in.bottom=r.bottom-1;

    if (overview) {
      // From afar only the block address is written, in a readable font and
      // not the zoom one: first the comfortable one, then the smallest
      // readable one, and only if it fits.
      sprintf(label,"%08lX",b->start);
      for (k=0; k<2; k++) {
        if (Golabelfont(gw,k)==NULL)
          continue;
        if (8*gw->lfontcw[k]+4>in.right-in.left || gw->lfontch[k]>in.bottom-in.top)
          continue;
        SelectObject(mem,gw->lfont[k]);
        ExtTextOut(mem,in.left+2,in.top+(in.bottom-in.top-gw->lfontch[k])/2,
          ETO_CLIPPED,&in,label,8,NULL);
        break;
      };
      continue;
    };

    // Lines are placed in graph pixels and converted one by one, as the click
    // does (Golineat): adding up an already rounded height made the error grow
    // line after line, and at 128 lines a double click picked the next one.
    // Same for the columns, below.
    ly=b->y+GO_PADYPX;
    x0=b->x+GO_PADXPX;
    tx=GOCX(gw,x0);
    j0=Goscale(GOGY(gw,clip->top)-ly,1,gw->ch0)-1;
    if (j0<0) j0=0;
    j1=Goscale(GOGY(gw,clip->bottom)-ly,1,gw->ch0)+2;
    if (j1>b->nlines) j1=b->nlines;
    for (j=j0; j<j1; j++) {
      ln=&GOLINE(g,b,j);
      ty=GOCY(gw,ly+j*gw->ch0);
      hr.left=in.left; hr.right=in.right;
      hr.top=ty; hr.bottom=GOCY(gw,ly+(j+1)*gw->ch0);
      // Three states, three backgrounds, in order of importance: where I am
      // now (EIP), where my cursor is, and where I set a breakpoint. It is the
      // same grammar as Olly's Disassembler, so there is no second one to
      // learn. Lines found by search come after the cursor.
      if (gw->eip!=0 && ln->addr==gw->eip)
        FillRect(mem,&hr,gw->eipbrush);
      else if (i==chosen && j==gw->curline)
        FillRect(mem,&hr,gw->curbrush);
      else if (searching && gw->findhit[i] && Gomatch(ln->text,gw->find))
        FillRect(mem,&hr,gw->findbrush);
      // Breakpoint: red background under the address field only, as Olly
      // marks it. Grey if disabled.
      bp=Getbreakpointtype(ln->addr);
      if (bp!=0 && (bp & (TY_ACTIVE|TY_DISABLED|TY_ONESHOT))!=0) {
        RECT br;
        br.left=tx-1;
        br.right=GOCX(gw,x0+8*gw->cw0)+1;
        if (br.right>in.right) br.right=in.right;
        br.top=hr.top; br.bottom=hr.bottom;
        FillRect(mem,&br,((bp & TY_DISABLED)!=0 ? gw->bpdisbrush : gw->bpbrush));
      };
      // Every character in its place in graph pixels: the columns (address,
      // bytes, instruction) stay aligned at any zoom, whatever the real width
      // of the TrueType.
      n=strlen(ln->text);
      if (n>GO_LINELEN) n=GO_LINELEN;
      for (k=0; k<n; k++)
        dx[k]=GOCX(gw,x0+(k+1)*gw->cw0)-GOCX(gw,x0+k*gw->cw0);
      ExtTextOut(mem,tx,ty,ETO_CLIPPED,&in,ln->text,n,dx);
    };
    if (b->term==GO_T_UNRES) {
      // Explicit marker: from here the CFG is incomplete, it is not a leaf.
      SetTextColor(mem,Gogetcolor(RED,RGB(192,0,0)));
      TextOut(mem,tx,r.bottom+2,"unresolved exit",15);
      SetTextColor(mem,Gogetcolor(BLACK,RGB(0,0,0)));
    };
  };

  SelectObject(mem,oldbrush);
  SelectObject(mem,oldpen);
  SelectObject(mem,oldfont);
};

// Where the minimap goes and at what scale, for a w x h window. It is hidden
// when the whole graph already fits (it would say nothing and would cover some
// blocks) or when the window is too small to hold both.
static void Gomaplayout(t_gownd *gw,int w,int h) {
  t_gograph *g;
  int maxw,maxh,mw,mh,pw,ph;
  double sx,sy,s;
  SetRectEmpty(&gw->maprc);
  g=gw->graph;
  if (!gominimap || g==NULL || g->nblock==0 || g->width<=0 || g->height<=0)
    return;
  if (w<200 || h<150)
    return;
  pw=w*100/gw->zoom;
  ph=h*100/gw->zoom;
  if (pw>=g->width && ph>=g->height)
    return;
  maxw=w/4; if (maxw>GO_MAPMAXW) maxw=GO_MAPMAXW;
  maxh=h/3; if (maxh>GO_MAPMAXH) maxh=GO_MAPMAXH;
  sx=(double)maxw/g->width;
  sy=(double)maxh/g->height;
  s=(sx<sy ? sx : sy);
  mw=(int)(g->width*s)+1;
  mh=(int)(g->height*s)+1;
  if (mw<12) mw=12;                    // A graph with a very long ladder still
  if (mh<12) mh=12;                    // gets a clickable strip
  gw->mapscale=s;
  gw->maprc.right=w-GO_MAPGAP;
  gw->maprc.bottom=h-GO_MAPGAP;
  gw->maprc.left=gw->maprc.right-mw;
  gw->maprc.top=gw->maprc.bottom-mh;
};

// The minimap image: blocks and edges of the whole graph. Rebuilt only when
// the layout, the search or the size changes; the rest of the time it is a
// BitBlt.
static void Gomapbuild(HDC ref,t_gownd *gw,int mw,int mh) {
  HDC dc;
  HBITMAP old;
  HPEN pen,oldpen;
  RECT r;
  t_gograph *g;
  t_goroute *rt;
  POINT p[GO_MAXPT];
  double s;
  int i,k;
  if (gw->mapbmp!=NULL && (gw->mapw!=mw || gw->maph!=mh)) {
    DeleteObject(gw->mapbmp);
    gw->mapbmp=NULL;
  };
  if (gw->mapbmp==NULL) {
    gw->mapbmp=CreateCompatibleBitmap(ref,mw,mh);
    if (gw->mapbmp==NULL)
      return;
    gw->mapw=mw; gw->maph=mh;
  };
  dc=CreateCompatibleDC(ref);
  if (dc==NULL)
    return;
  old=(HBITMAP)SelectObject(dc,gw->mapbmp);
  g=gw->graph;
  s=gw->mapscale;
  r.left=r.top=0; r.right=mw; r.bottom=mh;
  FillRect(dc,&r,gw->bkbrush);
  pen=CreatePen(PS_SOLID,1,Gopale(Gogetcolor(BLUE,RGB(0,0,192)),45));
  oldpen=(HPEN)SelectObject(dc,(pen!=NULL ? pen : gw->penbox));
  if (g->route!=NULL) {
    for (i=0; i<g->nedge && i<g->nroute; i++) {
      rt=&(g->route[i]);
      if (rt->np<2)
        continue;
      for (k=0; k<rt->np && k<GO_MAXPT; k++) {
        p[k].x=(int)(rt->x[k]*s);
        p[k].y=(int)(rt->y[k]*s);
      };
      Polyline(dc,p,k);
    };
  };
  for (i=0; i<g->nblock; i++) {
    r.left  =(int)(g->block[i].x*s);
    r.top   =(int)(g->block[i].y*s);
    r.right =(int)((g->block[i].x+g->block[i].dx)*s)+1;
    r.bottom=(int)((g->block[i].y+g->block[i].dy)*s)+1;
    FillRect(dc,&r,(gw->find[0]!='\0' && gw->findhit[i] ? gw->findbrush : gw->ovbrush));
    FrameRect(dc,&r,(HBRUSH)GetStockObject(GRAY_BRUSH));
  };
  SelectObject(dc,oldpen);
  if (pen!=NULL)
    DeleteObject(pen);
  SelectObject(dc,old);
  DeleteDC(dc);
  gw->mapok=1;
};

// The minimap over the graph: the cached image, the EIP block and the view
// rectangle. Costs one BitBlt and three rectangles.
static void Godrawmap(HDC mem,t_gownd *gw,int w,int h) {
  HDC dc;
  HBITMAP old;
  HBRUSH oldbrush;
  HPEN oldpen;
  RECT r,*m;
  t_goblock *b;
  double s;
  int mw,mh;
  m=&gw->maprc;
  if (IsRectEmpty(m))
    return;
  mw=m->right-m->left;
  mh=m->bottom-m->top;
  if (gw->mapok==0 || gw->mapbmp==NULL || gw->mapw!=mw || gw->maph!=mh)
    Gomapbuild(mem,gw,mw,mh);
  if (gw->mapbmp==NULL)
    return;
  dc=CreateCompatibleDC(mem);
  if (dc==NULL)
    return;
  old=(HBITMAP)SelectObject(dc,gw->mapbmp);
  BitBlt(mem,m->left,m->top,mw,mh,dc,0,0,SRCCOPY);
  SelectObject(dc,old);
  DeleteDC(dc);
  s=gw->mapscale;
  if (gw->eipblock>=0 && gw->eipblock<gw->graph->nblock) {
    b=&(gw->graph->block[gw->eipblock]);
    r.left  =m->left+(int)(b->x*s)-1;
    r.top   =m->top +(int)(b->y*s)-1;
    r.right =m->left+(int)((b->x+b->dx)*s)+2;
    r.bottom=m->top +(int)((b->y+b->dy)*s)+2;
    FillRect(mem,&r,gw->bpbrush);
  };
  r=*m;
  InflateRect(&r,1,1);
  FrameRect(mem,&r,(HBRUSH)GetStockObject(BLACK_BRUSH));
  // The view rectangle, clipped to the minimap edges: with a graph narrower
  // than the window it would stick out.
  r.left  =m->left+(int)(gw->ox*s);
  r.top   =m->top +(int)(gw->oy*s);
  r.right =m->left+(int)(GOGX(gw,w)*s)+1;
  r.bottom=m->top +(int)(GOGY(gw,h)*s)+1;
  if (r.left<m->left)     r.left=m->left;
  if (r.top<m->top)       r.top=m->top;
  if (r.right>m->right)   r.right=m->right;
  if (r.bottom>m->bottom) r.bottom=m->bottom;
  oldpen=(HPEN)SelectObject(mem,gw->pencur);
  oldbrush=(HBRUSH)SelectObject(mem,GetStockObject(HOLLOW_BRUSH));
  Rectangle(mem,r.left,r.top,r.right,r.bottom);
  SelectObject(mem,oldbrush);
  SelectObject(mem,oldpen);
};

static void Gopaint(HWND hw,t_gownd *gw) {
  PAINTSTRUCT ps;
  HDC hdc;
  RECT rc,clip;
  int w,h,full;
  double t0;
  t_gograph *g;

  t0=Gousec();
  hdc=BeginPaint(hw,&ps);
  GetClientRect(hw,&rc);
  w=rc.right; h=rc.bottom;
  if (w<1) w=1;
  if (h<1) h=1;
  if (gw->bkbrush==NULL)
    Gomakegdi(gw);

  // Double buffer: draw into an in-memory bitmap and copy it in one go.
  // Without it, while panning, the white background and the blocks alternate
  // on every WM_PAINT and the window flickers. The bitmap is kept and rebuilt
  // only when the window changes size.
  full=0;
  if (gw->bbdc==NULL) {
    gw->bbdc=CreateCompatibleDC(hdc);
    gw->bbold=NULL;
  };
  if (gw->bbdc!=NULL && (gw->bbbmp==NULL || gw->bbw!=w || gw->bbh!=h)) {
    HBITMAP nb=CreateCompatibleBitmap(hdc,w,h);
    if (nb!=NULL) {
      HBITMAP prev=(HBITMAP)SelectObject(gw->bbdc,nb);
      if (gw->bbold==NULL) gw->bbold=prev;
      if (gw->bbbmp!=NULL) DeleteObject(gw->bbbmp);
      gw->bbbmp=nb; gw->bbw=w; gw->bbh=h;
    };
    full=1;                           // New bitmap: must be filled entirely
  };
  if (gw->bbdc==NULL || gw->bbbmp==NULL || gw->bbw!=w || gw->bbh!=h) {
    FillRect(hdc,&rc,gw->bkbrush);     // Out of memory: at least no garbage
    EndPaint(hw,&ps);
    return;
  };
  clip=(full ? rc : ps.rcPaint);
  IntersectRect(&clip,&clip,&rc);
  SelectClipRgn(gw->bbdc,NULL);
  IntersectClipRect(gw->bbdc,clip.left,clip.top,clip.right,clip.bottom);
  FillRect(gw->bbdc,&clip,gw->bkbrush);

  g=gw->graph;
  if (g!=NULL && g->nblock>0) {
    Godrawgraph(gw->bbdc,gw,&clip);
    Gomaplayout(gw,w,h);
    Godrawmap(gw->bbdc,gw,w,h);
  };
  SelectClipRgn(gw->bbdc,NULL);
  BitBlt(hdc,clip.left,clip.top,clip.right-clip.left,clip.bottom-clip.top,
    gw->bbdc,clip.left,clip.top,SRCCOPY);
  EndPaint(hw,&ps);

  if (g==NULL || g->nblock==0)
    return;
  gw->npaint++;
  gw->paintus+=Gousec()-t0;
  if ((gw->npaint%10)==0)
    Gotrace("  draw: %d paints, %d us each (zoom %d%%, %d blocks, %d edges)",
      gw->npaint,(int)(gw->paintus/gw->npaint),gw->zoom,g->nblock,g->nedge);
};

// Graph pens and brushes: created once per window, not on every paint. The
// colours stay the ones the user chose in ollydbg.ini.
// Edges are 2 px wide: at 1 px, overlapping, green and red could no longer be
// told apart.
static void Gomakegdi(t_gownd *gw) {
  COLORREF ctrue,cfalse,cjmp,ccall,cblack;
  HDC hdc;
  HFONT oldfont;
  TEXTMETRIC tm;
  SIZE sz;

  ctrue =Gogetcolor(GREEN,RGB(0,128,0));
  cfalse=Gogetcolor(RED,RGB(192,0,0));
  cjmp  =Gogetcolor(BLUE,RGB(0,0,192));
  ccall =Gogetcolor(DARKGRAY,RGB(112,112,112));
  cblack =Gogetcolor(BLACK,RGB(0,0,0));
  gw->pentrue  =CreatePen(PS_SOLID,2,ctrue);
  gw->penfalse =CreatePen(PS_SOLID,2,cfalse);
  gw->penjmp   =CreatePen(PS_SOLID,2,cjmp);
  gw->pentrue2 =CreatePen(PS_SOLID,4,ctrue);
  gw->penfalse2=CreatePen(PS_SOLID,4,cfalse);
  gw->penjmp2  =CreatePen(PS_SOLID,4,cjmp);
  gw->pencall  =CreatePen(PS_DOT,1,ccall);
  gw->brtrue   =CreateSolidBrush(ctrue);
  gw->brfalse  =CreateSolidBrush(cfalse);
  gw->brjmp    =CreateSolidBrush(cjmp);
  gw->brcall   =CreateSolidBrush(ccall);
  gw->penbox   =CreatePen(PS_SOLID,1,cblack);
  gw->pencur   =CreatePen(PS_SOLID,2,cjmp);
  gw->peneip   =CreatePen(PS_SOLID,3,cfalse);
  gw->bkbrush  =CreateSolidBrush(Gogetcolor(WHITE,RGB(255,255,255)));
  gw->eipbrush =CreateSolidBrush(Gopale(cfalse,18));
  gw->selbrush =CreateSolidBrush(Gopale(cjmp,14));
  gw->hotbrush =CreateSolidBrush(Gopale(cblack,7));
  gw->ovbrush  =CreateSolidBrush(Gopale(cblack,12));
  gw->curbrush =CreateSolidBrush(Gopale(cblack,16));   // cursor: grey, like Olly
  gw->bpbrush  =CreateSolidBrush(cfalse);             // active breakpoint
  gw->bpdisbrush=CreateSolidBrush(Gopale(cblack,30));  // disabled breakpoint
  gw->findbrush=CreateSolidBrush(Gopale(Gogetcolor(YELLOW,RGB(255,255,0)),55));

  // Metrics of the UNSCALED font: the layout measured the blocks with them,
  // and they map the y of a click back to the line that was hit.
  hdc=GetDC(NULL);
  oldfont=(HFONT)SelectObject(hdc,Gogetfont());
  GetTextMetrics(hdc,&tm);
  gw->cw0=tm.tmAveCharWidth;
  gw->ch0=tm.tmHeight;
  if (GetTextExtentPoint32(hdc,"0",1,&sz))
    gw->cw0=sz.cx;
  SelectObject(hdc,oldfont);
  ReleaseDC(NULL,hdc);
  if (gw->ch0<1) gw->ch0=1;
  if (gw->cw0<1) gw->cw0=1;
};

#define GOFREE(h)  do { if ((h)!=NULL) { DeleteObject(h); (h)=NULL; }; } while (0)

static void Gofreegdi(t_gownd *gw) {
  int i;
  GOFREE(gw->pentrue);  GOFREE(gw->penfalse); GOFREE(gw->penjmp);
  GOFREE(gw->pentrue2); GOFREE(gw->penfalse2);GOFREE(gw->penjmp2);
  GOFREE(gw->pencall);  GOFREE(gw->penbox);   GOFREE(gw->pencur);
  GOFREE(gw->peneip);   GOFREE(gw->brtrue);   GOFREE(gw->brfalse);
  GOFREE(gw->brjmp);    GOFREE(gw->brcall);   GOFREE(gw->bkbrush);
  GOFREE(gw->eipbrush); GOFREE(gw->selbrush); GOFREE(gw->hotbrush);
  GOFREE(gw->ovbrush);  GOFREE(gw->curbrush); GOFREE(gw->bpbrush);
  GOFREE(gw->bpdisbrush);GOFREE(gw->findbrush);
  for (i=0; i<GO_NZFONT; i++) {
    GOFREE(gw->zfont[i]);
    gw->zfontzoom[i]=0;
  };
  GOFREE(gw->lfont[0]); GOFREE(gw->lfont[1]);
  GOFREE(gw->mapbmp);
  // The buffer: first put back into the DC the bitmap it had of its own, then
  // delete ours and the DC.
  if (gw->bbdc!=NULL && gw->bbold!=NULL)
    SelectObject(gw->bbdc,gw->bbold);
  GOFREE(gw->bbbmp);
  if (gw->bbdc!=NULL) {
    DeleteDC(gw->bbdc);
    gw->bbdc=NULL;
  };
};

// Line of block b under client y "cy", or -1. The computation is the drawing
// one reversed (docs/NOTES.md §9): line j covers the pixels from GOCY(line j)
// to GOCY(line j+1) exclusive. GOGY gives an almost right line, and one
// correction step is enough because the error is below one graph pixel.
static int Golineat(t_gownd *gw,t_goblock *b,int cy) {
  int ly,j;
  ly=b->y+GO_PADYPX;
  j=Goscale(GOGY(gw,cy)-ly,1,gw->ch0);
  if (GOCY(gw,ly+(j+1)*gw->ch0)<=cy) j++;
  else if (GOCY(gw,ly+j*gw->ch0)>cy) j--;
  if (j<0 || j>=b->nlines)
    return -1;
  return j;
};

// Block under the client point, or -1. If pline is not NULL it also receives
// the index of the line hit (-1 if the point is in the block but outside the
// text, or in the overview where there is no text).
static int Gohittest(t_gownd *gw,int cx,int cy,int *pline) {
  int i;
  POINT pt;
  t_goblock *b;
  if (pline!=NULL)
    *pline=-1;
  if (gw->graph==NULL || gw->ch0<1)
    return -1;
  pt.x=cx; pt.y=cy;
  if (PtInRect(&gw->maprc,pt))
    return -1;                         // Under the minimap there is no graph
  // In client pixels, with the same edges as drawing: in graph pixels a click
  // on the last pixel column of the block could fall outside.
  for (i=0; i<gw->graph->nblock; i++) {
    b=&(gw->graph->block[i]);
    if (cx<GOCX(gw,b->x) || cx>=GOCX(gw,b->x+b->dx) ||
        cy<GOCY(gw,b->y) || cy>=GOCY(gw,b->y+b->dy))
      continue;
    if (pline!=NULL && Gohastext(gw))
      *pline=Golineat(gw,b,cy);
    return i;
  };
  return -1;
};

// Brings block i into view, without jumping around if it is already there.
static void Goshowblock(HWND hw,t_gownd *gw,int i) {
  RECT rc;
  int pw,ph;
  t_goblock *b;
  if (gw->graph==NULL || i<0 || i>=gw->graph->nblock)
    return;
  b=&(gw->graph->block[i]);
  GetClientRect(hw,&rc);
  pw=GOGX(gw,rc.right)-gw->ox;
  ph=GOGY(gw,rc.bottom)-gw->oy;
  if (b->x<gw->ox || b->x+b->dx>gw->ox+pw)
    gw->ox=b->x+b->dx/2-pw/2;
  if (b->y<gw->oy || b->y+b->dy>gw->oy+ph)
    gw->oy=b->y+b->dy/2-ph/2;
  Goclamp(hw,gw);
  Goupdatescroll(hw,gw);
};

// Redoes the layout after an expansion and updates bars and title.
static void Gorelayout(HWND hw,t_gownd *gw) {
  HDC hdc;
  HFONT oldfont;
  char title[160];
  hdc=GetDC(hw);
  oldfont=(HFONT)SelectObject(hdc,Gogetfont());
  Golayout(gw->graph,hdc);
  SelectObject(hdc,oldfont);
  ReleaseDC(hw,hdc);
  sprintf(title,"Graph: %.60s%s%s",gw->graph->title,
    (gw->graph->nfunc>1 ? "  [+ expanded functions]" : ""),
    (gw->graph->truncated ? "  [truncated]" : ""));
  SetWindowText(hw,title);
  Gofindmark(gw);                      // New blocks: the minimap too
  Goclamp(hw,gw);
  Goupdatescroll(hw,gw);
};

// Expands the function called by the line (block i, line "line"), if it is a
// CALL with a known destination. Returns 1 if the graph changed.
static int Goexpandline(HWND hw,t_gownd *gw,int i,int line) {
  ulong target;
  int nb;
  if (i<0 || line<0)
    return 0;
  target=GOLINE(gw->graph,&(gw->graph->block[i]),line).calltarget;
  if (target==0)
    return 0;
  nb=Goexpand(gw->graph,target,i);
  if (nb<0)
    return 0;
  gw->curblock=nb;
  Gorelayout(hw,gw);
  gw->eipblock=Goblockat(gw->graph,gw->eip);
  Goshowblock(hw,gw,nb);
  InvalidateRect(hw,NULL,FALSE);
  return 1;
};

// The chosen block goes to the clipboard as text.
static void Gocopyblock(t_gownd *gw,int i) {
  HGLOBAL h;
  char *p;
  int j,n;
  t_goblock *b;
  if (gw->graph==NULL || i<0 || i>=gw->graph->nblock)
    return;
  b=&(gw->graph->block[i]);
  n=b->nlines*(GO_LINELEN+2)+64;
  h=GlobalAlloc(GMEM_MOVEABLE,n);
  if (h==NULL)
    return;
  p=(char *)GlobalLock(h);
  if (p==NULL) {
    GlobalFree(h);
    return;
  };
  p[0]='\0';
  for (j=0; j<b->nlines; j++) {
    strcat(p,GOLINE(gw->graph,b,j).text);
    strcat(p,"\r\n");
  };
  GlobalUnlock(h);
  if (OpenClipboard(NULL)) {
    EmptyClipboard();
    SetClipboardData(CF_TEXT,h);       // From here on the memory belongs to Windows
    CloseClipboard();
  }
  else
    GlobalFree(h);
};

// Remembers where we were before jumping elsewhere. Backspace walks it back.
static void Gohistpush(t_gownd *gw) {
  int k;
  if (gw->curblock<0)
    return;
  if (gw->nhist>=32) {                 // Stack full: the oldest is dropped
    for (k=1; k<32; k++) {
      gw->histblock[k-1]=gw->histblock[k];
      gw->histline[k-1] =gw->histline[k];
    };
    gw->nhist=31;
  };
  gw->histblock[gw->nhist]=gw->curblock;
  gw->histline[gw->nhist]=gw->curline;
  gw->nhist++;
};

// Moves the cursor to a block and a line, and brings it into view.
static void Gogoto(HWND hw,t_gownd *gw,int blk,int row) {
  if (blk<0 || blk>=gw->graph->nblock)
    return;
  gw->curblock=blk;
  gw->curline=row;
  if (gw->curline<0) gw->curline=0;
  if (gw->curline>=gw->graph->block[blk].nlines)
    gw->curline=gw->graph->block[blk].nlines-1;
  Goshowline(hw,gw,blk,gw->curline);
  InvalidateRect(hw,NULL,FALSE);
};

// The line under the cursor, or NULL.
static t_goline *Gocurline(t_gownd *gw) {
  t_goblock *b;
  if (gw->graph==NULL || gw->curblock<0 || gw->curblock>=gw->graph->nblock)
    return NULL;
  b=&(gw->graph->block[gw->curblock]);
  if (gw->curline<0 || gw->curline>=b->nlines)
    return NULL;
  return &GOLINE(gw->graph,b,gw->curline);
};

// ENTER: follows what is under the cursor, INSIDE the graph.
//   CALL line          -> expands the called function and goes there
//   jump line          -> moves the cursor to the destination block
//   anything else      -> nothing (the double click goes to the CPU window)
// Returns 1 if it followed something.
static int Gofollow(HWND hw,t_gownd *gw) {
  t_goline *ln;
  t_goblock *b;
  int nb;
  ln=Gocurline(gw);
  if (ln==NULL) {
    Gotrace("  follow: no line under the cursor (block %d line %d)",
      gw->curblock,gw->curline);
    return 0;
  };
  Gotrace("  follow: %08lX call=%08lX",ln->addr,ln->calltarget);
  if (ln->calltarget!=0) {
    Gohistpush(gw);
    if (Goexpandline(hw,gw,gw->curblock,gw->curline))
      return 1;
    gw->nhist--;
    return 0;
  };
  b=&(gw->graph->block[gw->curblock]);
  // The jump is always the last line of the block: if the cursor is there and
  // the block has a resolved destination, follow it.
  if (gw->curline==b->nlines-1 && b->target!=0 &&
      (b->term==GO_T_JMP || b->term==GO_T_COND)) {
    nb=Goblockat(gw->graph,b->target);
    Gotrace("  follow: jump to %08lX -> block %d",b->target,nb);
    if (nb>=0) {
      Gohistpush(gw);
      Gogoto(hw,gw,nb,0);
      return 1;
    };
  };
  return 0;
};

// Backspace: goes back to where we were before the last "follow".
static void Goback(HWND hw,t_gownd *gw) {
  if (gw->nhist<=0) {
    Flash("GraphOlly: nothing to go back to");
    return;
  };
  gw->nhist--;
  Gogoto(hw,gw,gw->histblock[gw->nhist],gw->histline[gw->nhist]);
};

// "*": back to the current instruction, as in Olly's Disassembler. Moves the
// cursor to EIP's line, not just to its block.
static void Gogoeip(HWND hw,t_gownd *gw) {
  t_goblock *b;
  int j;
  if (gw->eipblock<0 || gw->eip==0) {
    Flash("GraphOlly: EIP is not in this graph");
    return;
  };
  Gohistpush(gw);
  b=&(gw->graph->block[gw->eipblock]);
  for (j=0; j<b->nlines; j++) {
    if (GOLINE(gw->graph,b,j).addr==gw->eip)
      break;
  };
  Gogoto(hw,gw,gw->eipblock,(j<b->nlines ? j : 0));
};

// F2: sets or clears the breakpoint on the line under the cursor, as in Olly.
static void Gotoggle(HWND hw,t_gownd *gw) {
  t_goline *ln;
  ulong bptype;
  ln=Gocurline(gw);
  if (ln==NULL)
    return;
  bptype=Getbreakpointtype(ln->addr);
  if ((bptype & (TY_ACTIVE|TY_DISABLED))!=0)
    Deletebreakpoints(ln->addr,ln->addr+1,1);
  else
    Setbreakpoint(ln->addr,TY_ACTIVE|TY_KEEPCODE,0);
  Gotrace("  breakpoint at %08lX: was %08lX, now %08lX",
    ln->addr,bptype,Getbreakpointtype(ln->addr));
  InvalidateRect(hw,NULL,FALSE);
};

// Brings line "line" of block i into view. Different from Goshowblock: a
// 128-line block, centred, shows its middle, and the cursor line (or EIP's, or
// the search hit) stayed out. A block that fits is centred whole; one taller
// than the window is scrolled to the LINE. Horizontally, if the block is wider
// than the window, its start is shown, where address and instruction are.
static void Goshowline(HWND hw,t_gownd *gw,int i,int line) {
  RECT rc;
  int pw,ph,ly;
  t_goblock *b;
  if (gw->graph==NULL || i<0 || i>=gw->graph->nblock)
    return;
  b=&(gw->graph->block[i]);
  GetClientRect(hw,&rc);
  pw=GOGX(gw,rc.right)-gw->ox;
  ph=GOGY(gw,rc.bottom)-gw->oy;
  ly=b->y+GO_PADYPX+line*gw->ch0;
  if (b->x<gw->ox || b->x+b->dx>gw->ox+pw)
    gw->ox=(b->dx<=pw ? b->x+b->dx/2-pw/2 : b->x-GO_EXPMARGIN);
  if (b->dy<=ph) {
    if (b->y<gw->oy || b->y+b->dy>gw->oy+ph)
      gw->oy=b->y+b->dy/2-ph/2;
  }
  else if (ly<gw->oy || ly+gw->ch0>gw->oy+ph)
    gw->oy=ly+gw->ch0/2-ph/2;
  Goclamp(hw,gw);
  Goupdatescroll(hw,gw);
};

// Marks the blocks containing the searched text and counts the lines. Redone
// on every new search and whenever the graph changes (expansion, function
// switch): it costs one pass over the lines, not one pass per paint.
static void Gofindmark(t_gownd *gw) {
  t_gograph *g;
  t_goblock *b;
  int i,j,n;
  g=gw->graph;
  gw->nfound=0;
  gw->mapok=0;                         // Found blocks are shown there too
  memset(gw->findhit,0,sizeof(gw->findhit));
  if (g==NULL || gw->find[0]=='\0')
    return;
  for (i=0; i<g->nblock; i++) {
    b=&(g->block[i]);
    for (j=n=0; j<b->nlines; j++)
      if (Gomatch(GOLINE(g,b,j).text,gw->find)) n++;
    gw->findhit[i]=(n>0);
    gw->nfound+=n;
  };
};

// Lines are walked in ADDRESS order, not in the order in which the CFG
// discovered the blocks: F3 must go down the code as it would in the
// Disassembler, not jump randomly between branches. Hand-written Shell sort:
// qsort would pull one more import from the CRT for two thousand integers.
static void Gosortblocks(t_gograph *g,int *ord,int n) {
  int gap,i,j,t;
  for (i=0; i<n; i++) ord[i]=i;
  for (gap=n/2; gap>0; gap/=2) {
    for (i=gap; i<n; i++) {
      t=ord[i];
      for (j=i; j>=gap && g->block[ord[j-gap]].start>g->block[t].start; j-=gap)
        ord[j]=ord[j-gap];
      ord[j]=t;
    };
  };
};

// Moves to the next (dir=1) or previous (dir=-1) match. With "inclusive" the
// cursor line counts too: needed by the first search, which must not skip
// what is already in front of you.
static int Gofindstep(HWND hw,t_gownd *gw,int dir,int inclusive) {
  t_gograph *g;
  int ord[GO_MAXBLOCKS];
  int n,r,j,k,limit,wrapped,nb,which;
  g=gw->graph;
  if (g==NULL || g->nblock==0 || gw->find[0]=='\0')
    return 0;
  if (gw->nfound==0) {
    Flash("GraphOlly: \"%s\" is not in this graph",gw->find);
    return 0;
  };
  n=g->nblock;
  Gosortblocks(g,ord,n);
  r=-1;
  for (k=0; k<n; k++) {
    if (ord[k]==gw->curblock) { r=k; break; };
  };
  if (r<0) {                           // No cursor: from the start or the end
    r=(dir>0 ? 0 : n-1);
    j=(dir>0 ? -1 : g->block[ord[r]].nlines);
    inclusive=0;
  }
  else
    j=gw->curline;
  limit=g->nline+2*n+2;
  wrapped=0;
  for (k=0; k<limit; k++) {
    if (k>0 || inclusive==0) {
      j+=dir;
      if (j>=g->block[ord[r]].nlines && dir>0) {
        r++; j=0;
        if (r>=n) { r=0; wrapped=1; };
      }
      else if (j<0 && dir<0) {
        r--;
        if (r<0) { r=n-1; wrapped=1; };
        j=g->block[ord[r]].nlines-1;
      };
    };
    nb=ord[r];
    if (j<0 || j>=g->block[nb].nlines)
      continue;
    if (!Gomatch(GOLINE(g,&(g->block[nb]),j).text,gw->find))
      continue;
    // Found. Which match is it, out of all of them? Say so, together with the
    // wrap-around: without it, F3 seems to never end.
    which=0;
    {
      int rr,jj;
      for (rr=0; rr<n; rr++) {
        t_goblock *b=&(g->block[ord[rr]]);
        if (!gw->findhit[ord[rr]])
          continue;
        for (jj=0; jj<b->nlines; jj++) {
          if (Gomatch(GOLINE(g,b,jj).text,gw->find)) which++;
          if (rr==r && jj==j) break;
        };
        if (rr==r) break;
      };
    };
    gw->curblock=nb;
    gw->curline=j;
    if (!Gohastext(gw))          // From afar the line would not be readable
      Gosetzoom(hw,gw,100,0,0);
    Goshowline(hw,gw,nb,j);
    InvalidateRect(hw,NULL,FALSE);
    Flash("GraphOlly: \"%s\" %d of %d%s",gw->find,which,gw->nfound,
      (wrapped ? (dir>0 ? " (wrapped to the start)" : " (wrapped to the end)") : ""));
    Gotrace("  search \"%s\": %08lX, %d of %d",gw->find,
      GOLINE(g,&(g->block[nb]),j).addr,which,gw->nfound);
    return 1;
  };
  return 0;
};

// New search. The text is kept lowercase; a leading "0x" is removed, because
// Olly writes addresses without it.
static int Gofindnew(HWND hw,t_gownd *gw,const char *text) {
  int k;
  while (*text==' ' || *text=='\t') text++;
  if (text[0]=='0' && (text[1]=='x' || text[1]=='X') && text[2]!='\0')
    text+=2;
  for (k=0; text[k]!='\0' && k<TEXTLEN-1; k++)
    gw->find[k]=(text[k]>='A' && text[k]<='Z' ? text[k]-'A'+'a' : text[k]);
  while (k>0 && (gw->find[k-1]==' ' || gw->find[k-1]=='\t'))
    k--;
  gw->find[k]='\0';
  Gofindmark(gw);
  InvalidateRect(hw,NULL,FALSE);
  if (gw->find[0]=='\0')
    return 0;
  if (gw->nfound==0) {
    Flash("GraphOlly: \"%s\" is not in this graph",gw->find);
    return 0;
  };
  Gohistpush(gw);                      // Backspace returns to before the search
  return Gofindstep(hw,gw,1,1);
};

// Ctrl+F: asks for the text with Olly's small input box (with its search
// history) and searches.
static void Gofindask(HWND hw,t_gownd *gw) {
  char s[TEXTLEN];
  strcpy(s,gw->find);
  if (Gettext("Search in graph (address, instruction, operand)",
      s,0,NM_REFTXT,FIXEDFONT)<0)
    return;
  Gofindnew(hw,gw,s);
};

// Saves the WHOLE graph, one to one if it fits, as a 24-bit BMP. A DIB section
// and not a compatible bitmap: it does not depend on the screen and its bytes
// are already those of the file. If the graph is huge the scale goes down; if
// even then memory is not enough, it gives up without damage.
// Returns the zoom used, 0 on failure.
static int Goexportfile(t_gownd *gw,const char *path) {
  t_gograph *g;
  BITMAPINFO bi;
  BITMAPFILEHEADER fh;
  HDC dc;
  HBITMAP bmp,old;
  void *bits;
  RECT r;
  FILE *f;
  int z,w,h,stride,ok;
  int sox,soy,szoom,shot,scur;
  double gw0,gh0;

  g=gw->graph;
  if (g==NULL || g->nblock==0 || g->width<=0 || g->height<=0)
    return 0;
  if (gw->bkbrush==NULL)
    Gomakegdi(gw);
  gw0=g->width+2*GO_EXPMARGIN;
  gh0=g->height+2*GO_EXPMARGIN;
  for (z=100; z>1; z--) {
    if (gw0*z/100<=GO_EXPMAXSIDE && gh0*z/100<=GO_EXPMAXSIDE &&
        (gw0*z/100)*(gh0*z/100)<=GO_EXPMAXPIX)
      break;
  };
  dc=CreateCompatibleDC(NULL);
  if (dc==NULL)
    return 0;
  bmp=NULL; bits=NULL;
  for (;;) {
    w=(int)(gw0*z/100); h=(int)(gh0*z/100);
    if (w<1) w=1;
    if (h<1) h=1;
    memset(&bi,0,sizeof(bi));
    bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth=w;
    bi.bmiHeader.biHeight=h;           // Positive: rows bottom-up, as in the file
    bi.bmiHeader.biPlanes=1;
    bi.bmiHeader.biBitCount=24;
    bi.bmiHeader.biCompression=BI_RGB;
    bmp=CreateDIBSection(dc,&bi,DIB_RGB_COLORS,&bits,NULL,0);
    if (bmp!=NULL && bits!=NULL)
      break;
    if (bmp!=NULL) DeleteObject(bmp);
    bmp=NULL;
    Gotrace("  export: %dx%d at %d%% does not fit in memory",w,h,z);
    if (z<=5) {
      DeleteDC(dc);
      return 0;
    };
    z=z*3/4;
  };
  old=(HBITMAP)SelectObject(dc,bmp);

  // Drawn with the same code as the window, moving the view onto the whole
  // graph for a moment. No hover block and no cursor: in an image to attach
  // to a report they would be noise.
  sox=gw->ox; soy=gw->oy; szoom=gw->zoom; shot=gw->hotblock; scur=gw->curblock;
  gw->zoom=z;
  gw->ox=-GO_EXPMARGIN; gw->oy=-GO_EXPMARGIN;
  gw->hotblock=-1; gw->curblock=-1;
  r.left=r.top=0; r.right=w; r.bottom=h;
  FillRect(dc,&r,gw->bkbrush);
  Godrawgraph(dc,gw,&r);
  GdiFlush();
  gw->ox=sox; gw->oy=soy; gw->zoom=szoom; gw->hotblock=shot; gw->curblock=scur;

  ok=0;
  stride=((w*3+3)/4)*4;
  f=fopen(path,"wb");
  if (f!=NULL) {
    memset(&fh,0,sizeof(fh));
    fh.bfType=0x4D42;                  // "BM"
    fh.bfOffBits=sizeof(BITMAPFILEHEADER)+sizeof(BITMAPINFOHEADER);
    fh.bfSize=fh.bfOffBits+(DWORD)stride*h;
    bi.bmiHeader.biSizeImage=(DWORD)stride*h;
    ok=(fwrite(&fh,sizeof(fh),1,f)==1 &&
        fwrite(&bi.bmiHeader,sizeof(BITMAPINFOHEADER),1,f)==1 &&
        fwrite(bits,1,(size_t)stride*h,f)==(size_t)stride*h);
    if (fclose(f)!=0) ok=0;
  };
  SelectObject(dc,old);
  DeleteObject(bmp);
  DeleteDC(dc);
  Gotrace("  export \"%s\": %dx%d at %d%% -> %s",path,w,h,z,(ok ? "ok" : "FAILED"));
  return (ok ? z : 0);
};

// Asks where to save (the standard Windows dialog) and exports.
static void Goexportask(HWND hw,t_gownd *gw) {
  OPENFILENAME of;
  char path[MAX_PATH];
  int z;
  if (gw->graph==NULL)
    return;
  sprintf(path,"graph_%08lX.bmp",gw->graph->procstart);
  memset(&of,0,sizeof(of));
  of.lStructSize=sizeof(of);
  of.hwndOwner=hwmain;
  of.lpstrFilter="Bitmap (*.bmp)\0*.bmp\0All files\0*.*\0";
  of.lpstrFile=path;
  of.nMaxFile=sizeof(path);
  of.lpstrTitle="Export graph as image";
  of.lpstrDefExt="bmp";
  of.Flags=OFN_OVERWRITEPROMPT|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR|OFN_HIDEREADONLY;
  if (!GetSaveFileName(&of))
    return;
  z=Goexportfile(gw,path);
  if (z==0)
    Flash("GraphOlly: export failed (memory or file)");
  else if (z<100)
    Flash("GraphOlly: graph saved, reduced to %d%% because it is too large",z);
  else
    Flash("GraphOlly: graph saved to %s",path);
  InvalidateRect(hw,NULL,FALSE);
};

// A point of the minimap becomes the centre of the view.
static void Gomapgo(HWND hw,t_gownd *gw,int cx,int cy) {
  RECT rc;
  int gx,gy;
  if (IsRectEmpty(&gw->maprc) || gw->mapscale<=0)
    return;
  if (cx<gw->maprc.left) cx=gw->maprc.left;
  if (cx>gw->maprc.right) cx=gw->maprc.right;
  if (cy<gw->maprc.top) cy=gw->maprc.top;
  if (cy>gw->maprc.bottom) cy=gw->maprc.bottom;
  gx=(int)((cx-gw->maprc.left)/gw->mapscale);
  gy=(int)((cy-gw->maprc.top)/gw->mapscale);
  GetClientRect(hw,&rc);
  gw->ox=gx-(GOGX(gw,rc.right)-gw->ox)/2;
  gw->oy=gy-(GOGY(gw,rc.bottom)-gw->oy)/2;
  Goclamp(hw,gw);
  Goupdatescroll(hw,gw);
  InvalidateRect(hw,NULL,FALSE);
};

// Block i changed appearance (mouse over or away): only it is redrawn, not the
// whole window. The margin covers the thick frame and the label under an
// unresolved exit.
static void Goinvalblock(HWND hw,t_gownd *gw,int i) {
  RECT r;
  t_goblock *b;
  if (gw->graph==NULL || i<0 || i>=gw->graph->nblock)
    return;
  b=&(gw->graph->block[i]);
  r.left  =GOCX(gw,b->x)-4;
  r.top   =GOCY(gw,b->y)-4;
  r.right =GOCX(gw,b->x+b->dx)+4;
  r.bottom=GOCY(gw,b->y+b->dy)+4;
  if (b->term==GO_T_UNRES)
    r.bottom+=GOCL(gw,gw->ch0)+4;
  InvalidateRect(hw,&r,FALSE);
};

// Context menu. The items are the ones actually used, in the order they are
// needed; shortcuts are written next to them so they get learned.
static void Gocontextmenu(HWND hw,t_gownd *gw,int sx,int sy) {
  HMENU m;
  int i,line,cmd;
  POINT pt;
  ulong target;
  char s[80];

  pt.x=sx; pt.y=sy;
  ScreenToClient(hw,&pt);
  i=Gohittest(gw,pt.x,pt.y,&line);
  if (i>=0 && (i!=gw->curblock || line!=gw->curline)) {
    gw->curblock=i;
    if (line>=0) gw->curline=line;
    InvalidateRect(hw,NULL,FALSE);
    UpdateWindow(hw);                  // Must be visible BEFORE the menu opens
  };
  target=0;
  if (i>=0 && line>=0)
    target=GOLINE(gw->graph,&(gw->graph->block[i]),line).calltarget;

  m=CreatePopupMenu();
  if (m==NULL)
    return;
  AppendMenu(m,MF_STRING|(i>=0 ? 0 : MF_GRAYED),GOM_FOLLOW,
    "&Follow jump or call\tEnter");
  AppendMenu(m,MF_STRING|(gw->nhist>0 ? 0 : MF_GRAYED),GOM_BACK,
    "Go bac&k\tBackspace");
  AppendMenu(m,MF_STRING|(i>=0 ? 0 : MF_GRAYED),GOM_CPU,
    "Show in &CPU window\tdouble click");
  AppendMenu(m,MF_STRING|(i>=0 && line>=0 ? 0 : MF_GRAYED),GOM_BP,
    "&Breakpoint here\tF2");
  if (target!=0) {
    sprintf(s,"Ex&pand function %08lX\tdouble click",target);
    AppendMenu(m,MF_STRING,GOM_EXPAND,s);
  }
  else
    AppendMenu(m,MF_STRING|MF_GRAYED,GOM_EXPAND,
      "Ex&pand called function\t(needs a CALL line)");
  AppendMenu(m,MF_SEPARATOR,0,NULL);
  AppendMenu(m,MF_STRING,GOM_ZOOMIN, "Zoom &in\t+   (or Ctrl+wheel)");
  AppendMenu(m,MF_STRING,GOM_ZOOMOUT,"Zoom &out\t-");
  sprintf(s,"Zoom &100%%  (now %d%%)\t0",gw->zoom);
  AppendMenu(m,MF_STRING,GOM_ZOOM100,s);
  AppendMenu(m,MF_STRING,GOM_FIT,    "Fi&t to window\tEnd");
  AppendMenu(m,MF_SEPARATOR,0,NULL);
  AppendMenu(m,MF_STRING,GOM_ENTRY,  "Go to function &entry\tHome");
  AppendMenu(m,MF_STRING|(gw->eipblock>=0 ? 0 : MF_GRAYED),GOM_GOEIP,
    "Back to current instruction\t*");
  AppendMenu(m,MF_STRING|(gofolloweip ? MF_CHECKED : 0),GOM_FOLLOWEIP,
    "Follow EIP &while the program runs");
  AppendMenu(m,MF_STRING|(gofollowfunc ? MF_CHECKED : 0),GOM_FOLLOWFUN,
    "Switch f&unction when execution enters it");
  AppendMenu(m,MF_SEPARATOR,0,NULL);
  AppendMenu(m,MF_STRING|(i>=0 ? 0 : MF_GRAYED),GOM_COPY,
    "Cop&y block to clipboard\tCtrl+C");
  AppendMenu(m,MF_STRING,GOM_EXPORT,"E&xport graph as image...");
  AppendMenu(m,MF_SEPARATOR,0,NULL);
  AppendMenu(m,MF_STRING,GOM_FIND,"&Search in graph...\tCtrl+F");
  AppendMenu(m,MF_STRING|(gw->nfound>0 ? 0 : MF_GRAYED),GOM_FINDNEXT,
    "Find &next\tF3");
  AppendMenu(m,MF_STRING|(gw->nfound>0 ? 0 : MF_GRAYED),GOM_FINDPREV,
    "Find pre&vious\tShift+F3");
  AppendMenu(m,MF_STRING|(gominimap ? MF_CHECKED : 0),GOM_MINIMAP,
    "&Minimap\tM");

  cmd=TrackPopupMenu(m,TPM_LEFTALIGN|TPM_RETURNCMD|TPM_RIGHTBUTTON,
    sx,sy,0,hw,NULL);
  DestroyMenu(m);

  switch (cmd) {
    case GOM_CPU:
      if (i>=0)
        Setcpu(0,(line>=0 ? GOLINE(gw->graph,&(gw->graph->block[i]),line).addr
                          : gw->graph->block[i].start),0,0,
          CPU_ASMHIST|CPU_ASMCENTER|CPU_ASMFOCUS);
      break;
    case GOM_EXPAND:   Gohistpush(gw); Goexpandline(hw,gw,i,line); break;
    case GOM_FOLLOW:   Gofollow(hw,gw); break;
    case GOM_BACK:     Goback(hw,gw); break;
    case GOM_BP:       Gotoggle(hw,gw); break;
    case GOM_ZOOMIN:   Gosetzoom(hw,gw,gw->zoom*5/4+1,pt.x,pt.y); break;
    case GOM_ZOOMOUT:  Gosetzoom(hw,gw,gw->zoom*4/5-1,pt.x,pt.y); break;
    case GOM_ZOOM100:  Gosetzoom(hw,gw,100,pt.x,pt.y); break;
    case GOM_FIT:      Gofit(hw,gw); break;
    case GOM_ENTRY:    Goshowblock(hw,gw,0); InvalidateRect(hw,NULL,FALSE); break;
    case GOM_GOEIP:    Gogoeip(hw,gw); break;
    case GOM_FOLLOWEIP:
      gofolloweip=(gofolloweip ? 0 : 1);
      Pluginwriteinttoini(hinst,"FollowEIP",gofolloweip);
      break;
    case GOM_FOLLOWFUN:
      gofollowfunc=(gofollowfunc ? 0 : 1);
      Pluginwriteinttoini(hinst,"FollowFunction",gofollowfunc);
      Gofolloweip();
      break;
    case GOM_COPY:     Gocopyblock(gw,i); break;
    case GOM_EXPORT:   Goexportask(hw,gw); break;
    case GOM_FIND:     Gofindask(hw,gw); break;
    case GOM_FINDNEXT: Gofindstep(hw,gw,1,0); break;
    case GOM_FINDPREV: Gofindstep(hw,gw,-1,0); break;
    case GOM_MINIMAP:  Gocmdminimap(); break;
  };
};

LRESULT CALLBACK Gowinproc(HWND hw,UINT msg,WPARAM wp,LPARAM lp) {
  t_gownd *gw;
  int i,line,delta,step;
  RECT rc;
  SCROLLINFO si;
  POINT pt;

  gw=(t_gownd *)GetWindowLong(hw,GWL_USERDATA);

  switch (msg) {
    case WM_TIMER:
      // Two things that happen OUTSIDE the graph and that nobody reports to
      // it: breakpoints set from the Disassembler, and - when Olly is stopped
      // between two events - EIP moving. They are checked here, and the graph
      // is redrawn only if something really changed.
      if (gw!=NULL && wp==GO_TIMERID) {
        ulong sig=Gobpsignature();
        if (sig!=gw->bpsig) {
          gw->bpsig=sig;
          InvalidateRect(hw,NULL,FALSE);
        };
        return 0;
      };
      break;

    case WM_DESTROY:
      if (gw!=NULL) {
        KillTimer(hw,GO_TIMERID);
        Gotrace("graph window closed: %d paints, %d us each",
          gw->npaint,(gw->npaint>0 ? (int)(gw->paintus/gw->npaint) : 0));
        Gofreegdi(gw);
        Gofreegraph(gw->graph);        // Also frees the line pool
        free(gw);
        SetWindowLong(hw,GWL_USERDATA,0);
      };
      break;

    case WM_PAINT:
      if (gw!=NULL) {
        Gopaint(hw,gw);
        return 0;
      };
      break;

    case WM_ERASEBKGND:
      return 1;                        // Background drawn in WM_PAINT

    case WM_SIZE:
      if (gw!=NULL) {
        Goclamp(hw,gw);
        Goupdatescroll(hw,gw);
        // The buffer must be rebuilt and the minimap moves with the corner:
        // without this, when the window shrinks Windows would ask for no paint.
        InvalidateRect(hw,NULL,FALSE);
      };
      break;

    case WM_HSCROLL:
    case WM_VSCROLL:
      if (gw!=NULL) {
        int *pos,page;
        GetClientRect(hw,&rc);
        if (msg==WM_HSCROLL) {
          pos=&(gw->ox); page=GOGX(gw,rc.right)-gw->ox;
        }
        else {
          pos=&(gw->oy); page=GOGY(gw,rc.bottom)-gw->oy;
        };
        switch (LOWORD(wp)) {
          case SB_LINEUP:   *pos-=gw->ch0; break;
          case SB_LINEDOWN: *pos+=gw->ch0; break;
          case SB_PAGEUP:   *pos-=page; break;
          case SB_PAGEDOWN: *pos+=page; break;
          case SB_TOP:      *pos=0; break;
          case SB_BOTTOM:   *pos=(msg==WM_HSCROLL ? gw->graph->width : gw->graph->height); break;
          case SB_THUMBTRACK:
          case SB_THUMBPOSITION:
            memset(&si,0,sizeof(si));
            si.cbSize=sizeof(si);
            si.fMask=SIF_TRACKPOS;
            GetScrollInfo(hw,(msg==WM_HSCROLL ? SB_HORZ : SB_VERT),&si);
            *pos=si.nTrackPos;
            break;
        };
        Goclamp(hw,gw);
        Goupdatescroll(hw,gw);
        InvalidateRect(hw,NULL,FALSE);
        return 0;
      };
      break;

    case WM_MOUSEWHEEL:
      if (gw!=NULL) {
        delta=(short)HIWORD(wp);
        pt.x=(short)LOWORD(lp); pt.y=(short)HIWORD(lp);
        ScreenToClient(hw,&pt);
        Gotrace("  wheel: flags=%04X delta=%d zoom=%d%% at client %ld,%ld",
          (int)LOWORD(wp),delta,gw->zoom,pt.x,pt.y);
        if (LOWORD(wp) & MK_CONTROL) {
          // Ctrl+wheel: zoom on the point under the mouse, as in any modern
          // tool. The step is multiplicative, not additive: additive would be
          // very slow at the top and jumpy at the bottom.
          Gosetzoom(hw,gw,(delta>0 ? gw->zoom*5/4+1 : gw->zoom*4/5-1),pt.x,pt.y);
        }
        else if (LOWORD(wp) & MK_SHIFT) {
          // Shift+wheel: sideways. On wide graphs it is the missing gesture.
          gw->ox-=delta*gw->cw0*8/120;
          Goclamp(hw,gw);
          Goupdatescroll(hw,gw);
          InvalidateRect(hw,NULL,FALSE);
        }
        else {
          gw->oy-=delta*gw->ch0*3/120;
          Goclamp(hw,gw);
          Goupdatescroll(hw,gw);
          InvalidateRect(hw,NULL,FALSE);
        };
        return 0;
      };
      break;

    case WM_LBUTTONDOWN:
    case WM_MBUTTONDOWN:               // Pan by dragging
      if (gw!=NULL) {
        SetFocus(hw);                  // Without it the keyboard never gets here
        pt.x=(short)LOWORD(lp); pt.y=(short)HIWORD(lp);
        if (msg==WM_LBUTTONDOWN && PtInRect(&gw->maprc,pt)) {
          // In the minimap a click moves the view, and dragging moves it
          // around.
          gw->mapdrag=1;
          SetCapture(hw);
          Gomapgo(hw,gw,pt.x,pt.y);
          return 0;
        };
        if (msg==WM_LBUTTONDOWN) {
          i=Gohittest(gw,(short)LOWORD(lp),(short)HIWORD(lp),&line);
          if (i>=0 && (i!=gw->curblock || line!=gw->curline)) {
            gw->curblock=i;
            if (line>=0) gw->curline=line;
            InvalidateRect(hw,NULL,FALSE);
          };
        };
        gw->dragging=1;
        gw->dragorg.x=(short)LOWORD(lp);
        gw->dragorg.y=(short)HIWORD(lp);
        gw->dragox=gw->ox;
        gw->dragoy=gw->oy;
        SetCapture(hw);
        return 0;
      };
      break;

    case WM_MOUSEMOVE:
      if (gw!=NULL && gw->mapdrag) {
        Gomapgo(hw,gw,(short)LOWORD(lp),(short)HIWORD(lp));
        return 0;
      };
      if (gw!=NULL && gw->dragging && (wp & (MK_LBUTTON|MK_MBUTTON))==0) {
        // The button was released outside our view (another window took the
        // mouse): without this, the view would follow the mouse with the
        // button up.
        gw->dragging=0;
        ReleaseCapture();
      };
      if (gw!=NULL && gw->dragging) {
        // The drag is measured in client pixels and converted: at 50% zoom
        // the hand must move half the graph distance, not twice as much.
        gw->ox=gw->dragox-((short)LOWORD(lp)-gw->dragorg.x)*100/gw->zoom;
        gw->oy=gw->dragoy-((short)HIWORD(lp)-gw->dragorg.y)*100/gw->zoom;
        Goclamp(hw,gw);
        Goupdatescroll(hw,gw);
        InvalidateRect(hw,NULL,FALSE);
        return 0;
      };
      if (gw!=NULL) {
        i=Gohittest(gw,(short)LOWORD(lp),(short)HIWORD(lp),NULL);
        if (i!=gw->hotblock) {
          // Only the two blocks that change tint are redrawn: moving the mouse
          // over a large graph redid the whole window for every block.
          Goinvalblock(hw,gw,gw->hotblock);
          gw->hotblock=i;
          Goinvalblock(hw,gw,i);
        };
      };
      break;

    case WM_LBUTTONUP:
    case WM_MBUTTONUP:
      if (gw!=NULL && gw->mapdrag) {
        gw->mapdrag=0;
        ReleaseCapture();
        return 0;
      };
      if (gw!=NULL && gw->dragging) {
        gw->dragging=0;
        ReleaseCapture();
        return 0;
      };
      break;

    case WM_CAPTURECHANGED:
      if (gw!=NULL) {
        gw->dragging=0;                // Capture lost (Alt+Tab, a menu):
        gw->mapdrag=0;                 // no drag is left hanging
      };
      break;

    case WM_CONTEXTMENU:
      if (gw!=NULL) {
        Gocontextmenu(hw,gw,(short)LOWORD(lp),(short)HIWORD(lp));
        return 0;
      };
      break;

    case WM_KEYDOWN:
      if (gw!=NULL) {
        GetClientRect(hw,&rc);
        step=gw->ch0*3;
        switch (wp) {
          case VK_ADD:
            Gozoomcur(hw,gw,gw->zoom*5/4+1); return 0;
          case VK_SUBTRACT: case VK_OEM_MINUS:
            Gozoomcur(hw,gw,gw->zoom*4/5-1); return 0;
          case '0': case VK_NUMPAD0:
            Gozoomcur(hw,gw,100); return 0;
          case VK_END:
            Gofit(hw,gw); return 0;
          case VK_HOME:
            Gogoto(hw,gw,0,0); return 0;
          case VK_MULTIPLY:            // Numeric keypad "*"
            Gogoeip(hw,gw); return 0;
          case VK_SPACE:
            Gogoeip(hw,gw); return 0;
          case VK_OEM_PLUS:
            // On many keyboards "*" is Shift + the "+" key: with Shift it
            // means "back to EIP", without it means "zoom in".
            if (GetKeyState(VK_SHIFT)<0) { Gogoeip(hw,gw); return 0; };
            Gozoomcur(hw,gw,gw->zoom*5/4+1); return 0;
          case VK_RETURN:
            if (Gofollow(hw,gw)==0)
              Flash("GraphOlly: no jump or call to follow here");
            return 0;
          case VK_BACK:
            Goback(hw,gw); return 0;
          case VK_F2:
            Gotoggle(hw,gw); return 0;
          case 'C':
            if (GetKeyState(VK_CONTROL)<0)
              Gocopyblock(gw,gw->curblock);
            return 0;
          // Search. Ctrl+F and Ctrl+L are the ones of Olly's Disassembler
          // ("search" and "search next"); F3 the one of every other program.
          // The trace tells which one really arrives.
          case 'F':
            Gotrace("  key F ctrl=%d",GetKeyState(VK_CONTROL)<0);
            if (GetKeyState(VK_CONTROL)<0)
              Gofindask(hw,gw);
            return 0;
          case 'L':
            if (GetKeyState(VK_CONTROL)<0)
              Gofindstep(hw,gw,(GetKeyState(VK_SHIFT)<0 ? -1 : 1),0);
            return 0;
          case VK_F3:
            Gotrace("  key F3 shift=%d",GetKeyState(VK_SHIFT)<0);
            if (gw->find[0]=='\0')
              Gofindask(hw,gw);
            else
              Gofindstep(hw,gw,(GetKeyState(VK_SHIFT)<0 ? -1 : 1),0);
            return 0;
          case VK_ESCAPE:              // Clear the search marks
            if (gw->find[0]!='\0') {
              gw->find[0]='\0';
              Gofindmark(gw);
              InvalidateRect(hw,NULL,FALSE);
            };
            return 0;
          case 'M':
            if (GetKeyState(VK_CONTROL)>=0)
              Gocmdminimap();
            return 0;
          // The arrows move the CURSOR inside the block, as in the
          // Disassembler; with Ctrl they scroll the view.
          case VK_UP:
            if (GetKeyState(VK_CONTROL)>=0 && gw->curblock>=0) {
              if (gw->curline>0) Gogoto(hw,gw,gw->curblock,gw->curline-1);
              return 0;
            };
            gw->oy-=step; break;
          case VK_DOWN:
            if (GetKeyState(VK_CONTROL)>=0 && gw->curblock>=0) {
              Gogoto(hw,gw,gw->curblock,gw->curline+1);
              return 0;
            };
            gw->oy+=step; break;
          case VK_LEFT:  gw->ox-=step; break;
          case VK_RIGHT: gw->ox+=step; break;
          case VK_PRIOR: gw->oy-=GOGY(gw,rc.bottom)-gw->oy; break;
          case VK_NEXT:  gw->oy+=GOGY(gw,rc.bottom)-gw->oy; break;
          default:
            return DefMDIChildProc(hw,msg,wp,lp);
        };
        Goclamp(hw,gw);
        Goupdatescroll(hw,gw);
        InvalidateRect(hw,NULL,FALSE);
        return 0;
      };
      break;

    case WM_LBUTTONDBLCLK:
      // Double click on a CALL line: the called function is expanded into the
      // graph. On any other line: the CPU window jumps there. Two different
      // gestures because they are two different questions - "where does it
      // go?" and "show me in the Disassembler".
      if (gw!=NULL) {
        pt.x=(short)LOWORD(lp); pt.y=(short)HIWORD(lp);
        if (PtInRect(&gw->maprc,pt)) {
          Gomapgo(hw,gw,pt.x,pt.y);    // Nothing to open in the minimap
          return 0;
        };
        i=Gohittest(gw,(short)LOWORD(lp),(short)HIWORD(lp),&line);
        if (i<0)
          break;
        gw->curblock=i;
        if (Goexpandline(hw,gw,i,line)==0)
          Setcpu(0,(line>=0 ?
                    GOLINE(gw->graph,&(gw->graph->block[i]),line).addr :
                    gw->graph->block[i].start),0,0,
            CPU_ASMHIST|CPU_ASMCENTER|CPU_ASMFOCUS);
        return 0;
      };
      break;
  };
  return DefMDIChildProc(hw,msg,wp,lp);
};

// Graph window already open on the same function, or NULL. Without this,
// asking twice for the graph of the same function opened two identical
// windows and kept two graphs alive.
static HWND Gofindwindow(HWND hwclient,ulong procstart) {
  HWND hw;
  t_gownd *gw;
  char cls[64];
  for (hw=GetWindow(hwclient,GW_CHILD); hw!=NULL; hw=GetWindow(hw,GW_HWNDNEXT)) {
    cls[0]='\0';
    GetClassName(hw,cls,sizeof(cls));
    if (strcmp(cls,gographclass)!=0)
      continue;
    gw=(t_gownd *)GetWindowLong(hw,GWL_USERDATA);
    if (gw!=NULL && gw->graph!=NULL && gw->graph->procstart==procstart)
      return hw;
  };
  return NULL;
};

// Rebuilds this window's graph on another function, keeping the existing
// window. It is what "follow the function I am in" needs: you step into a
// CALL and the graph switches by itself, as in modern disassemblers. Functions
// expanded by hand are lost, and rightly so: the graph now tells another story.
static int Gorebuild(HWND hw,t_gownd *gw,ulong addr) {
  t_gograph *g;
  HDC hdc;
  HFONT oldfont;
  char title[160];

  g=(t_gograph *)malloc(sizeof(t_gograph));
  if (g==NULL)
    return 0;
  memset(g,0,sizeof(t_gograph));
  if (Gobuildgraph(addr,g)==0) {
    Gofreegraph(g);
    return 0;
  };
  Gofreegraph(gw->graph);
  gw->graph=g;
  gw->curblock=Goblockat(g,addr);
  gw->curline=0;
  gw->hotblock=-1;
  gw->eipblock=-1;
  gw->nhist=0;
  gw->ox=gw->oy=0;
  Gofindmark(gw);                      // The search stays, on the new graph

  hdc=GetDC(hw);
  oldfont=(HFONT)SelectObject(hdc,Gogetfont());
  Golayout(g,hdc);
  SelectObject(hdc,oldfont);
  ReleaseDC(hw,hdc);

  sprintf(title,"Graph: %.60s%s%s",g->title,
    (g->noproc ? "  [no procedure: analyse the module, Ctrl+A]" : ""),
    (g->truncated ? "  [truncated]" : ""));
  SetWindowText(hw,title);
  Goupdatescroll(hw,gw);
  if (!Gohastext(gw))
    Gofit(hw,gw);
  if (gw->curblock>=0)
    Goshowblock(hw,gw,gw->curblock);
  InvalidateRect(hw,NULL,TRUE);
  Gotrace("  graph rebuilt at %08lX: %d blocks, %d edges",addr,g->nblock,g->nedge);
  return 1;
};

// Every time the debuggee stops, all open graph windows realign to EIP:
// highlighted block, highlighted line and, if following is on, the view moved
// onto it. The requirement is "follow the code in the graph too, as in the
// normal view".
void Gofolloweip(void) {
  HWND hwclient,hw;
  t_gownd *gw;
  t_thread *pt;
  char cls[64];
  ulong eip,prev;
  int i,n;

  hwclient=(HWND)Plugingetvalue(VAL_HWCLIENT);
  if (hwclient==NULL)
    return;
  eip=0;
  if (Getstatus()!=STAT_NONE && Getstatus()!=STAT_FINISHED) {
    pt=Findthread(Getcputhreadid());
    if (pt!=NULL && pt->regvalid)
      eip=pt->reg.ip;
  };
  n=0;
  for (hw=GetWindow(hwclient,GW_CHILD); hw!=NULL; hw=GetWindow(hw,GW_HWNDNEXT)) {
    cls[0]='\0';
    GetClassName(hw,cls,sizeof(cls));
    if (strcmp(cls,gographclass)!=0)
      continue;
    gw=(t_gownd *)GetWindowLong(hw,GWL_USERDATA);
    if (gw==NULL || gw->graph==NULL)
      continue;
    n++;
    if (gw->eip==eip)
      continue;                        // Nothing to redraw
    // gw->eip at 0 = window just opened. The function is followed only if EIP
    // MOVED while the window was there: otherwise asking for the graph of a
    // function other than EIP's rebuilt it right away on EIP's function, and
    // the requested function was never seen (observed on Windows 7).
    prev=gw->eip;
    gw->eip=eip;
    i=Goblockat(gw->graph,eip);
    if (i<0 && eip!=0 && prev!=0 && gofollowfunc) {
      // EIP left this graph: execution entered another function. The graph is
      // rebuilt there, but only if Olly recognises it as a DIFFERENT function:
      // without this check, a jump into unanalysed code would rebuild the graph
      // at every step.
      ulong newproc=Findprocbegin(eip);
      if (newproc!=0 && newproc!=gw->graph->procstart && Gorebuild(hw,gw,eip)) {
        gw->eip=eip;
        i=Goblockat(gw->graph,eip);
      };
    };
    gw->eipblock=i;
    if (i>=0 && gofolloweip) {
      int j;
      t_goblock *b=&(gw->graph->block[i]);
      for (j=0; j<b->nlines; j++)
        if (GOLINE(gw->graph,b,j).addr==eip) break;
      Goshowline(hw,gw,i,(j<b->nlines ? j : 0));
    };
    InvalidateRect(hw,NULL,FALSE);
  };
  if (n>0)
    Gotrace("  EIP %08lX: %d graph windows realigned",eip,n);
};

// Client coordinates of the first CALL line in an open graph window, and the
// window containing it. Used by automated QA: under wine the real mouse does
// not arrive, but a posted WM_LBUTTONDBLCLK does. By returning only the
// COORDINATES, the test then goes through the double click and hence through
// the real Gohittest and Goexpand, instead of bypassing them.
// If the line is out of view, the view moves onto it; and in the overview the
// zoom goes back to full, because there lines cannot even be hit.
int Gofindcallline(HWND *phw,int *px,int *py) {
  HWND hwclient,hw;
  t_gownd *gw;
  t_gograph *g;
  t_goblock *b;
  RECT rc;
  char cls[64];
  int i,j;

  hwclient=(HWND)Plugingetvalue(VAL_HWCLIENT);
  if (hwclient==NULL)
    return 0;
  for (hw=GetWindow(hwclient,GW_CHILD); hw!=NULL; hw=GetWindow(hw,GW_HWNDNEXT)) {
    cls[0]='\0';
    GetClassName(hw,cls,sizeof(cls));
    if (strcmp(cls,gographclass)!=0)
      continue;
    gw=(t_gownd *)GetWindowLong(hw,GWL_USERDATA);
    if (gw==NULL || (g=gw->graph)==NULL || gw->ch0<1)
      continue;
    if (!Gohastext(gw))
      Gosetzoom(hw,gw,100,0,0);
    for (i=0; i<g->nblock; i++) {
      b=&(g->block[i]);
      for (j=0; j<b->nlines; j++) {
        if (GOLINE(g,b,j).calltarget==0)
          continue;
        Goshowblock(hw,gw,i);
        GetClientRect(hw,&rc);
        *px=GOCX(gw,b->x+b->dx/2);
        *py=GOCY(gw,b->y+GO_PADYPX+j*gw->ch0+gw->ch0/2);
        if (*px<0 || *py<0 || *px>=rc.right || *py>=rc.bottom)
          continue;                    // Does not fit anyway: try the next one
        *phw=hw;
        InvalidateRect(hw,NULL,FALSE);
        Gotrace("  QA: first CALL at %08lX -> %08lX, client %d,%d (zoom %d%%)",
          GOLINE(g,b,j).addr,GOLINE(g,b,j).calltarget,*px,*py,gw->zoom);
        return 1;
      };
    };
  };
  return 0;
};

// Creates (or reuses) a graph window for the function containing addr.
int Gocreatewindow(ulong addr) {
  HWND hwclient,hw;
  HINSTANCE hi;
  HDC hdc;
  HFONT oldfont;
  t_gograph *g;
  t_gownd *gw;
  char title[160];

  Gotrace("Gocreatewindow(%08lX)",addr);
  hwclient=(HWND)Plugingetvalue(VAL_HWCLIENT);
  hi=(HINSTANCE)Plugingetvalue(VAL_HINST);
  if (hwclient==NULL) {
    Flash("GraphOlly: MDI client not available");
    return 0;
  };

  g=(t_gograph *)malloc(sizeof(t_gograph));
  if (g==NULL)
    return 0;
  memset(g,0,sizeof(t_gograph));
  if (Gobuildgraph(addr,g)==0) {
    Gotrace("  Gobuildgraph FAILED");
    Gofreegraph(g);
    return 0;
  };

  // Already open on this function? Reuse it: update the current block and
  // bring the window to the front.
  hw=Gofindwindow(hwclient,g->procstart);
  if (hw!=NULL) {
    gw=(t_gownd *)GetWindowLong(hw,GWL_USERDATA);
    gw->curblock=Goblockat(gw->graph,addr);
    Gofreegraph(g);
    SendMessage(hwclient,WM_MDIACTIVATE,(WPARAM)hw,0);
    Goshowblock(hw,gw,gw->curblock);
    InvalidateRect(hw,NULL,FALSE);
    Gotrace("  window reused hw=%p, current block %d",hw,gw->curblock);
    return 1;
  };

  gw=(t_gownd *)malloc(sizeof(t_gownd));
  if (gw==NULL) {
    Gofreegraph(g);
    return 0;
  };
  memset(gw,0,sizeof(t_gownd));
  gw->graph=g;
  gw->zoom=100;
  gw->curblock=Goblockat(g,addr);
  gw->hotblock=-1;
  gw->eipblock=-1;
  Gomakegdi(gw);                       // Needed already here: Goshowblock uses ch0

  // The layout measures text, so it needs a DC with Olly's fixed font.
  hdc=GetDC(hwclient);
  oldfont=(HFONT)SelectObject(hdc,Gogetfont());
  Golayout(g,hdc);
  SelectObject(hdc,oldfont);
  ReleaseDC(hwclient,hdc);

  sprintf(title,"Graph: %.60s%s%s",g->title,
    (g->noproc ? "  [no procedure: analyse the module, Ctrl+A]" : ""),
    (g->truncated ? "  [truncated]" : ""));

  hw=CreateWindowEx(WS_EX_MDICHILD,gographclass,title,
    WS_CHILD|WS_VISIBLE|WS_OVERLAPPEDWINDOW|WS_VSCROLL|WS_HSCROLL,
    CW_USEDEFAULT,CW_USEDEFAULT,760,540,
    hwclient,NULL,hi,NULL);
  if (hw==NULL) {
    Gofreegraph(g);
    Gofreegdi(gw);
    free(gw);
    Flash("GraphOlly: window creation failed");
    return 0;
  };
  Gotrace("  window created hw=%p, layout %dx%d",hw,g->width,g->height);
  SetWindowLong(hw,GWL_USERDATA,(LONG)gw);
  gw->bpsig=Gobpsignature();
  SetTimer(hw,GO_TIMERID,GO_TIMERMS,NULL);
  // It opens already fitted: better to see the whole function and then zoom
  // in than to open on a corner without knowing where you are. If it all fits
  // one to one, Gofit leaves the zoom at 100 and never enlarges beyond it.
  Goupdatescroll(hw,gw);
  Gofit(hw,gw);
  if (Gohastext(gw))
    Goshowblock(hw,gw,gw->curblock);
  Gofolloweip();                       // EIP right away, without waiting for a pause
  InvalidateRect(hw,NULL,TRUE);
  Addtolist(g->procstart,0,"GraphOlly: %d blocks, %d edges, zoom %d%%",
    g->nblock,g->nedge,gw->zoom);
  return 1;
};

// The graph window a command from Olly's menu is aimed at: the active one if
// it is a graph, otherwise the topmost of the open ones (the order of the
// children is the stacking order).
static HWND Gotopgraph(t_gownd **pgw) {
  HWND hwclient,hw;
  t_gownd *gw;
  char cls[64];
  *pgw=NULL;
  hwclient=(HWND)Plugingetvalue(VAL_HWCLIENT);
  if (hwclient==NULL)
    return NULL;
  hw=(HWND)SendMessage(hwclient,WM_MDIGETACTIVE,0,0);
  if (hw==NULL || GetClassName(hw,cls,sizeof(cls))==0 || strcmp(cls,gographclass)!=0)
    hw=GetWindow(hwclient,GW_CHILD);
  for (; hw!=NULL; hw=GetWindow(hw,GW_HWNDNEXT)) {
    cls[0]='\0';
    GetClassName(hw,cls,sizeof(cls));
    if (strcmp(cls,gographclass)!=0)
      continue;
    gw=(t_gownd *)GetWindowLong(hw,GWL_USERDATA);
    if (gw!=NULL && gw->graph!=NULL) {
      *pgw=gw;
      return hw;
    };
  };
  return NULL;
};

// "Search in graph" item of the plugin menu.
void Gocmdfind(void) {
  HWND hw;
  t_gownd *gw;
  hw=Gotopgraph(&gw);
  if (hw==NULL) {
    Flash("GraphOlly: no graph open");
    return;
  };
  Gofindask(hw,gw);
};

// "Export graph as image" item of the plugin menu.
void Gocmdexport(void) {
  HWND hw;
  t_gownd *gw;
  hw=Gotopgraph(&gw);
  if (hw==NULL) {
    Flash("GraphOlly: no graph open");
    return;
  };
  Goexportask(hw,gw);
};

// Turns the minimap on or off in ALL graph windows, and remembers it in the
// ini.
void Gocmdminimap(void) {
  HWND hwclient,hw;
  char cls[64];
  gominimap=(gominimap ? 0 : 1);
  Pluginwriteinttoini(hinst,"Minimap",gominimap);
  hwclient=(HWND)Plugingetvalue(VAL_HWCLIENT);
  if (hwclient==NULL)
    return;
  for (hw=GetWindow(hwclient,GW_CHILD); hw!=NULL; hw=GetWindow(hw,GW_HWNDNEXT)) {
    cls[0]='\0';
    GetClassName(hw,cls,sizeof(cls));
    if (strcmp(cls,gographclass)==0)
      InvalidateRect(hw,NULL,FALSE);
  };
};

// Service port: the same search as Ctrl+F, but with text coming from outside
// (under wine Olly's input box cannot be filled in). With NULL text it is
// F3 / Shift+F3. Returns the number of matching lines, -1 if there is no graph.
int Goqafind(const char *text,int dir) {
  HWND hw;
  t_gownd *gw;
  hw=Gotopgraph(&gw);
  if (hw==NULL)
    return -1;
  if (text!=NULL)
    Gofindnew(hw,gw,text);
  else
    Gofindstep(hw,gw,dir,0);
  UpdateWindow(hw);
  Gotrace("  QA: search -> %d lines, cursor block %d line %d",
    gw->nfound,gw->curblock,gw->curline);
  return gw->nfound;
};

// Service port: exports without the save dialog. Returns the zoom used, 0 on
// failure.
int Goqaexport(const char *path) {
  HWND hw;
  t_gownd *gw;
  hw=Gotopgraph(&gw);
  if (hw==NULL)
    return 0;
  return Goexportfile(gw,path);
};

// Service port: where the minimap is (client coordinates). 0 if hidden, so
// QA clicks it through the real WM_LBUTTONDOWN.
int Goqamaprect(RECT *r) {
  HWND hw;
  t_gownd *gw;
  hw=Gotopgraph(&gw);
  if (hw==NULL)
    return 0;
  InvalidateRect(hw,NULL,FALSE);       // The position is set by drawing
  UpdateWindow(hw);
  *r=gw->maprc;
  return !IsRectEmpty(r);
};

// Service port: exact zoom, anchored to the cursor block like the "0" key.
// Returns the zoom obtained (the limits clip it) and in *pcell the line height
// on screen, 0 in the overview: this way QA knows which of the two it is
// looking at without guessing from the screenshot.
int Goqazoom(int zoom,int *pcell) {
  HWND hw;
  t_gownd *gw;
  *pcell=0;
  hw=Gotopgraph(&gw);
  if (hw==NULL)
    return 0;
  Gozoomcur(hw,gw,zoom);
  UpdateWindow(hw);
  if (Gohastext(gw))
    *pcell=GOCL(gw,gw->ch0);
  Gotrace("  QA: zoom %d%% -> %d%%, line %d px",zoom,gw->zoom,*pcell);
  return gw->zoom;
};
