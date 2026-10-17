////////////////////////////////////////////////////////////////////////////////
// GraphOlly - control-flow graph view inside OllyDbg 1.10
// Shared definitions.
////////////////////////////////////////////////////////////////////////////////

#ifndef GRAPHOLLY_H
#define GRAPHOLLY_H

#include <windows.h>
#include "plugin_packed.h"

#define GO_NAME        "GraphOlly"
#define GO_VERSION     "1.0.0"
#define GO_AUTHOR      "Emanuele Lovato <bineurale@gmail.com>"

// Hard limits: Olly's strength is that it does not crash. On obfuscated code
// recursive descent goes off on a tangent, so every scan has a ceiling.
#define GO_MAXBLOCKS   2048            // Max blocks in a graph
#define GO_MAXEDGES    (GO_MAXBLOCKS*2)
#define GO_MAXCODE     (256*1024)      // Max bytes scanned in one function
#define GO_MAXLINES    128             // Max text lines shown per block
#define GO_LINELEN     96              // Length of a formatted line
#define GO_MAXFUNCS    64              // Functions expanded into the same graph
#define GO_PADYPX      4               // Margin above the first line of a block.
                                       // Used both by the layout (height) and by
                                       // drawing (where a line lands): if the two
                                       // numbers diverge, a click picks the wrong
                                       // line.
#define GO_PADXPX      8               // Margin left of the text: same rule,
                                       // between layout (width) and drawing.

// Edge type.
#define GO_E_TRUE      0               // Condition true  (green)
#define GO_E_FALSE     1               // Condition false (red)
#define GO_E_JMP       2               // Unconditional jump or fall-through (blue)
#define GO_E_CALL      3               // Call expanded on request (grey)

// How a block ends.
#define GO_T_FALL      0               // Falls into the next block
#define GO_T_JMP       1               // Resolved unconditional jump
#define GO_T_COND      2               // Resolved conditional jump
#define GO_T_RET       3               // RET
#define GO_T_UNRES     4               // UNRESOLVED exit (jmp eax, switch...)

// One line of code inside a block. It lives in a shared pool, not inside the
// block: kept inside, a t_goblock weighed 12 KB and a t_gograph 25 MB, zeroed
// for every graph even for a twelve-block function.
// "calltarget" is the destination of the CALL on this line, 0 if it is not a
// CALL or the destination is not constant: it is what a double click expands.
typedef struct t_goline {
  ulong          addr;                 // Instruction address
  ulong          calltarget;           // CALL destination, or 0
  char           text[GO_LINELEN];     // Line, already formatted
} t_goline;

typedef struct t_goblock {
  ulong          start;                // First byte of the block
  ulong          end;                  // Last byte + 1
  ulong          target;               // Destination of the final jump, if resolved
  ulong          func;                 // Entry of the function it belongs to
  int            firstline;            // Index into the line pool
  int            nlines;               // Actual text lines
  int            term;                 // GO_T_xxx
  int            level;                // Level (y) assigned by the layout
  int            order;                // Position within the level
  int            x,y;                  // Top-left corner, in graph pixels
  int            dx,dy;                // Size, in graph pixels
  int            nout,nin;             // Outgoing/incoming edges: used for ports
  int            col;                  // Grid column (used by routing)
} t_goblock;

typedef struct t_goedge {
  int            from;                 // Source block index
  int            to;                   // Destination block index
  int            type;                 // GO_E_xxx
  int            fall;                 // 1 = this is the FALL-THROUGH, not a
                                       // jump: the next block in memory. The
                                       // layout keeps it in the predecessor's
                                       // column.
} t_goedge;

// Path of an edge, computed by the layout and not by drawing: drawing must not
// know anything about levels and lanes, it only draws a polyline.
// An edge that skips several levels goes through a chain of DUMMY NODES, one
// per intermediate level: that reserves room for it in the column and spares it
// a horizontal lane as long as the whole graph. Hence the polyline can have
// quite a few points.
#define GO_MAXPT       40
typedef struct t_goroute {
  int            np;
  int            x[GO_MAXPT];
  int            y[GO_MAXPT];
} t_goroute;

typedef struct t_gograph {
  ulong          procstart;            // Bounds of the starting function
  ulong          procend;
  char           title[64];            // Function name/address
  int            nblock;
  int            nedge;
  int            truncated;            // 1 if a hard limit was hit
  int            noproc;               // 1 if Olly does not know the function bounds
  int            width,height;         // Total size of the layout
  t_goline       *line;                // Line pool, grows in chunks
  int            nline,maxline;
  t_goroute      *route;               // One path per edge, allocated by the layout
  int            nroute;
  ulong          func[GO_MAXFUNCS];    // Functions already expanded (entry)
  int            nfunc;
  t_goblock      block[GO_MAXBLOCKS];
  t_goedge       edge[GO_MAXEDGES];
} t_gograph;

// Line j of block b. The pool can be NULL on an empty graph.
#define GOLINE(g,b,j)  ((g)->line[(b)->firstline+(j)])

// cfg.c
int  Gobuildgraph(ulong addr,t_gograph *g);
int  Goexpand(t_gograph *g,ulong addr,int fromblock);
void Gofreegraph(t_gograph *g);
int  Gofindblock(t_gograph *g,ulong addr);
int  Goblockat(t_gograph *g,ulong addr);

// layout.c
void Golayout(t_gograph *g,HDC hdc);

// Zoom: a percentage. Text is drawn as long as a line, on screen, is at least
// GO_TEXTMINPX pixels tall: that is the real limit for reading an antialiased
// TrueType font. Below it the view switches to the overview, where each block
// keeps only its address, drawn in a readable font (GO_LABELPX) if it fits.
// It is not a zoom threshold because it depends on the font chosen in Olly.
#define GO_ZOOMMIN     15
#define GO_ZOOMMAX     400
#define GO_TEXTMINPX   5
#define GO_LABELPX     11

// graphwnd.c
extern char      gographclass[32];
int  Gocreatewindow(ulong addr);
void Gofolloweip(void);
int  Gofindcallline(HWND *phw,int *px,int *py);
LRESULT CALLBACK Gowinproc(HWND hw,UINT msg,WPARAM wp,LPARAM lp);
void Gocmdfind(void);
void Gocmdexport(void);
void Gocmdminimap(void);
int  Goqafind(const char *text,int dir);
int  Goqaexport(const char *path);
int  Goqamaprect(RECT *r);
int  Goqazoom(int zoom,int *pcell);

// grapholly.c
void Gotrace(const char *format,...);
void Godiag(void);
extern HINSTANCE hinst;
extern HWND      hwmain;
extern int       gofolloweip;          // Does the graph follow EIP? (ini: FollowEIP)
extern int       goshortkey;           // Shortcut letter (ini: Shortcut)
extern int       goshortshift;         // ... with Shift?
extern int       golabels;             // T/F labels on edges? (ini: Labels)
extern int       gofollowfunc;         // Does the graph follow EIP's function?
                                       // (ini: FollowFunction)
extern int       gominimap;            // Minimap in the corner? (ini: Minimap)

#endif
