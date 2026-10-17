////////////////////////////////////////////////////////////////////////////////
// Layout bench, compiled on the host with gcc (no wine, no Olly):
// build/bench/bench.sh.
//
// It includes layout.c WHOLE (LAYOUT_C), so it sees its statics: nodes,
// chains, columns. The graph is synthetic but shaped like compiled code:
// sequences, if, if/else, loops, and the if-ladder of the dispatcher in
// docs/NOTES.md §12. For each graph it prints
//   ms         median of Golayout over R runs
//   crossings  crossings between segments of adjacent levels (chains included)
//   falls      fall-throughs kept in the predecessor's column / fall-throughs
//              between adjacent levels
//   sig        hash of positions and routes: equal = identical layout
////////////////////////////////////////////////////////////////////////////////

#define _POSIX_C_SOURCE 199309L
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include LAYOUT_C

ulong GetTickCount(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC,&ts);
  return (ulong)(ts.tv_sec*1000+ts.tv_nsec/1000000);
}

void Gotrace(const char *format,...) { (void)format; }

static double Gonow(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC,&ts);
  return ts.tv_sec*1e3+ts.tv_nsec/1e6;
}

// --- Generator --------------------------------------------------------------
static t_gograph *G;
static unsigned  seed;
static int       target;               // Blocks wanted

static int Rnd(int n) {
  seed=seed*1103515245u+12345u;
  return (int)((seed>>16)%(unsigned)n);
}

static int Newblock(void) {
  int i,j,n;
  t_goline *l;
  if (G->nblock>=GO_MAXBLOCKS-2)
    return -1;
  i=G->nblock++;
  memset(&G->block[i],0,sizeof(t_goblock));
  G->block[i].start=0x401000+i*16;
  G->block[i].end=G->block[i].start+16;
  G->block[i].func=0x401000;
  G->block[i].firstline=G->nline;
  n=1+Rnd(8);
  for (j=0; j<n; j++) {
    if (G->nline>=G->maxline) {
      G->maxline=(G->maxline ? G->maxline*2 : 512);
      G->line=realloc(G->line,G->maxline*sizeof(t_goline));
    }
    l=&G->line[G->nline++];
    l->addr=G->block[i].start+j;
    l->calltarget=0;
    snprintf(l->text,GO_LINELEN,"%08lX  %.*s",l->addr,8+Rnd(32),
      "MOV EAX,DWORD PTR SS:[EBP+8] ; a comment just long enough");
  }
  G->block[i].nlines=n;
  return i;
}

static void Edge(int a,int b,int type,int fall) {
  if (a<0 || b<0 || G->nedge>=GO_MAXEDGES) return;
  G->edge[G->nedge].from=a; G->edge[G->nedge].to=b;
  G->edge[G->nedge].type=type; G->edge[G->nedge].fall=fall;
  G->nedge++;
}

// Each region starts at the block after the last one emitted and ends by
// falling into the block that comes next. "pend" are the fall-throughs still
// open towards "the next block".
static int pend[GO_MAXBLOCKS],npend;
static int epi[GO_MAXBLOCKS],nepi;     // Jumps to the epilogue (shared ret)

static int Block(void) {               // New block: closes the open fall-throughs
  int b,k;
  b=Newblock();
  if (b<0) return -1;
  for (k=0; k<npend; k++)
    Edge(pend[k],b,GO_E_JMP,pend[k]==b-1);
  npend=0;
  return b;
}

static void Region(int depth);

static void Seq(int depth,int n) {
  while (n-->0 && G->nblock<target)
    Region(depth);
}

static void Region(int depth) {
  int c,k,j,r,h,ncase,jmps[64],nj,tru[64];
  r=Rnd(100);
  if (depth>6) r=0;
  if (r<35) {                          // Plain block
    c=Block(); if (c<0) return;
    pend[npend++]=c;
  }
  else if (r<55) {                     // if
    c=Block(); if (c<0) return;
    Edge(c,c+1,GO_E_FALSE,1);
    Seq(depth+1,1+Rnd(3));
    j=Block(); if (j<0) return;        // true jumps past the then
    Edge(c,j,GO_E_TRUE,0);
    pend[npend++]=j;
  }
  else if (r<70) {                     // if/else
    c=Block(); if (c<0) return;
    Edge(c,c+1,GO_E_FALSE,1);
    Seq(depth+1,1+Rnd(3));
    h=Block(); if (h<0) return;        // end of then: jmp past the else
    k=G->nblock;
    Edge(c,k,GO_E_TRUE,0);
    Seq(depth+1,1+Rnd(3));
    j=Block(); if (j<0) return;
    Edge(h,j,GO_E_JMP,0);
    pend[npend++]=j;
  }
  else if (r<85) {                     // loop: head, body, backward jump
    h=Block(); if (h<0) return;
    Edge(h,h+1,GO_E_FALSE,1);
    Seq(depth+1,1+Rnd(4));
    c=Block(); if (c<0) return;
    Edge(c,h,GO_E_JMP,0);
    j=Newblock(); if (j<0) return;
    Edge(h,j,GO_E_TRUE,0);
    pend[npend++]=j;
  }
  else if (r<93) {                     // early exit to the epilogue
    c=Block(); if (c<0) return;
    Edge(c,c+1,GO_E_FALSE,1);
    epi[nepi++]=c;
    Seq(depth+1,1);
  }
  else {                               // if-ladder: cases jumping to the exit
    ncase=2+Rnd(6);
    nj=0;
    for (k=0; k<ncase; k++) {
      c=Block(); if (c<0) return;
      tru[k]=c;
      pend[npend++]=c;                 // false falls into the next test
    }
    for (k=0; k<ncase && nj<64; k++) {
      // Case k: the true branch of test k jumps into it.
      j=G->nblock;
      Seq(depth+2,1);
      if (G->nblock>j) {
        Edge(tru[k],j,GO_E_TRUE,0);
        h=Block(); if (h<0) return;
        jmps[nj++]=h;
      }
    }
    j=Block(); if (j<0) return;
    for (k=0; k<nj; k++) Edge(jmps[k],j,GO_E_JMP,0);
    pend[npend++]=j;
  }
}

// Fall-throughs created by Block() have type JMP; conditional tests want them
// FALSE. Fixed at the end: a block with a TRUE edge has a FALSE fall-through.
static void Fixtypes(void) {
  int e,f;
  for (e=0; e<G->nedge; e++) {
    if (G->edge[e].type!=GO_E_TRUE) continue;
    for (f=0; f<G->nedge; f++)
      if (f!=e && G->edge[f].from==G->edge[e].from && G->edge[f].fall)
        G->edge[f].type=GO_E_FALSE;
  }
}

static void Dedup(void) {              // Duplicates of Edge(c,c+1) + fall-through
  int e,f,n=0;
  for (e=0; e<G->nedge; e++) {
    for (f=0; f<n; f++)
      if (G->edge[f].from==G->edge[e].from && G->edge[f].to==G->edge[e].to) break;
    if (f<n) continue;
    G->edge[n++]=G->edge[e];
  }
  G->nedge=n;
}

static void Make(int nb,unsigned s) {
  int e,k;
  G=calloc(1,sizeof(t_gograph));
  seed=s; target=nb; npend=0; nepi=0;
  Block();
  pend[npend++]=0;
  while (G->nblock<target)
    Region(0);
  e=Block();                           // epilogue: RET
  if (e>=0) {
    for (k=0; k<nepi; k++) Edge(epi[k],e,GO_E_TRUE,0);
    G->block[e].term=GO_T_RET;
  }
  Dedup();
  Fixtypes();
}

// The dispatcher ladder of §12: n tests in a row, each with a case that jumps
// to the shared exit.
static void Makeladder(int ntest) {
  int k,c,b,x,cs[GO_MAXBLOCKS],bs[GO_MAXBLOCKS];
  G=calloc(1,sizeof(t_gograph));
  seed=7;
  for (k=0; k<ntest; k++) {
    c=Newblock(); cs[k]=c;
    b=Newblock(); bs[k]=b;
  }
  x=Newblock();
  for (k=0; k<ntest; k++) {
    Edge(cs[k],(k+1<ntest ? cs[k+1] : x),GO_E_FALSE,0);
    Edge(cs[k],bs[k],GO_E_TRUE,0);
    Edge(bs[k],x,GO_E_JMP,0);
  }
}

// --- Measurements ---------------------------------------------------------
static long Crossings(void) {
  int e,f,c,a1,b1,a2,b2,ns=0;
  long n=0;
  static int sa[GO_MAXEDGES*2+GO_MAXNODES],sb[GO_MAXEDGES*2+GO_MAXNODES];
  for (e=0; e<G->nedge; e++)
    for (c=0; c+1<chainlen[e]; c++) {
      sa[ns]=chain[chainpos[e]+c]; sb[ns]=chain[chainpos[e]+c+1]; ns++;
    }
  for (e=0; e<ns; e++)
    for (f=e+1; f<ns; f++) {
      a1=sa[e]; b1=sb[e]; a2=sa[f]; b2=sb[f];
      if (node[a1].level!=node[a2].level) continue;
      if (a1==a2 || b1==b2) continue;
      if ((long)(node[a1].x-node[a2].x)*(node[b1].x-node[b2].x)<0) n++;
    }
  return n;
}

static void Falls(int *ok,int *tot) {
  int e;
  *ok=*tot=0;
  for (e=0; e<G->nedge; e++) {
    if (!G->edge[e].fall) continue;
    if (node[G->edge[e].to].level!=node[G->edge[e].from].level+1) continue;
    (*tot)++;
    if (node[G->edge[e].to].col==node[G->edge[e].from].col) (*ok)++;
  }
}

static unsigned Signature(void) {
  unsigned h=2166136261u;
  int i,j;
#define MIX(v) (h=(h^(unsigned)(v))*16777619u)
  for (i=0; i<G->nblock; i++) { MIX(G->block[i].x); MIX(G->block[i].y); }
  for (i=0; i<G->nroute; i++)
    for (j=0; j<G->route[i].np; j++) { MIX(G->route[i].x[j]); MIX(G->route[i].y[j]); }
  MIX(G->width); MIX(G->height);
  return h;
}

static int Cmpd(const void *a,const void *b) {
  double x=*(const double *)a,y=*(const double *)b;
  return (x>y)-(x<y);
}

static void Run(const char *name,int reps) {
  double t[64],t0;
  int r,ok,tot;
  if (reps>64) reps=64;
  for (r=0; r<reps; r++) {
    t0=Gonow();
    Golayout(G,NULL);
    t[r]=Gonow()-t0;
  }
  qsort(t,reps,sizeof(double),Cmpd);
  Falls(&ok,&tot);
  printf("%-10s blocks %4d edges %4d nodes %5d  %8.3f ms (min %7.3f)  crossings %6ld  falls %4d/%-4d  %6dx%-6d sig %08x\n",
    name,G->nblock,G->nedge,nnode,t[reps/2],t[0],Crossings(),ok,tot,G->width,G->height,Signature());
  free(G->line); free(G->route); free(G);
}

int main(void) {
  Make(10,1);     Run("small",31);
  Make(200,2);    Run("medium",31);
  Make(2000,3);   Run("large",31);
  Make(2000,11);  Run("large-b",31);
  Makeladder(300);Run("ladder600",31);
  return 0;
}
