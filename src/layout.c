////////////////////////////////////////////////////////////////////////////////
// GraphOlly - layered layout and orthogonal edge routing.
//
// Sugiyama, this time in full:
//   1. back edges are found with an iterative DFS and ignored when assigning
//      levels (otherwise a loop sends the levels to infinity);
//   2. level = longest path from the entry;
//   3. DUMMY NODES: an edge that skips more than one level is split into a
//      chain of dummy nodes, one per intermediate level;
//   4. order within a level = barycenter, on dummy nodes as on real ones;
//   5. nodes are aligned on a GRID shared by all levels.
//
// Step 3 is the one that was missing, and it showed: without it, an edge
// jumping from one end of the function to the other had to cross the whole
// graph in a single horizontal channel, and on a large function the result was
// a comb of lines as long as the window. With dummy nodes that edge takes a
// column of its own at every level it crosses, and goes straight down.
//
// Step 5 is what makes routing possible: with aligned columns, between one
// column and the next there is a free vertical CORRIDOR at every level, and
// between one level and the next a horizontal CHANNEL.
//
//   edge between adjacent levels:  down - channel - down
//   long edge:                     down - channel - dummy node column - ...
//   backward edge (loop):          down - channel - corridor - channel - up
//
// Inside channels and corridors each edge takes a different LANE, so two
// parallel polylines do not overlap; and each block spreads its edges over
// different PORTS on its border. For a conditional jump the ports are FIXED:
// false on the left, true on the right, always, so the direction is learned
// once and never read again.
//
// The path is computed by the layout, not by drawing: graphwnd.c just draws
// a polyline.
////////////////////////////////////////////////////////////////////////////////

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plugin_packed.h"
#include "grapholly.h"

#define GO_PADX        GO_PADXPX       // Inner margin of a block
#define GO_PADY        GO_PADYPX       // (the same one drawing uses)
#define GO_GAPX        40              // Width of a vertical corridor
#define GO_GAPY        52              // Height of a horizontal channel
#define GO_MARGIN      24
#define GO_LANE        5               // Distance between two lanes, pixels
#define GO_NLANE       8               // Lanes per channel/corridor
#define GO_DUMMYW      10              // Width of a column holding only dummy nodes

// Two ceilings on dummy nodes, and both are needed.
//
// GO_MAXSPAN: an edge is split only if it skips FEW levels. Measured on a real
// function (00401159 in OLLYDBG.EXE: 599 blocks, 983 edges, 342 levels - an
// if-ladder dispatcher): without this ceiling 5545 dummy nodes were created,
// levels widened to 25 columns and the graph frayed diagonally, worse than the
// comb the dummy nodes were meant to remove. Long edges are few and the detour
// through the corridor suits them fine; the medium-length ones make the comb,
// and those are the ones worth splitting.
//
// GO_MAXNODES: safety net on the total.
#define GO_MAXSPAN     8
#define GO_MAXNODES    (GO_MAXBLOCKS*2)

typedef struct t_gonode {
  int            level;
  int            order;
  int            col;
  int            x,y,dx,dy;
  int            block;                // Index of the real block, or -1 if dummy
} t_gonode;

static char     backedge[GO_MAXEDGES];
static char     visited[GO_MAXBLOCKS];
static t_gonode node[GO_MAXNODES];
static int      nnode;
// Node chain of each edge: chain[chainpos[e] .. chainpos[e]+chainlen[e]-1].
// The first and the last are the source and the destination block.
static int      chain[GO_MAXEDGES*2+GO_MAXNODES];  // two ends per edge + dummies
static int      nchain;
static int      chainpos[GO_MAXEDGES];
static int      chainlen[GO_MAXEDGES];
static int      edgeport[GO_MAXEDGES];  // Port on the source border
static int      edgeport2[GO_MAXEDGES]; // Port on the destination border

// Adjacency lists (outgoing and incoming edges of each block), built once per
// layout with a counting pass: within each list the edges keep the order of
// g->edge. Before, every phase re-read ALL edges for every block -
// O(blocks*edges), some fifteen million comparisons on a full graph.
static int      outstart[GO_MAXBLOCKS+1],outlist[GO_MAXEDGES];
static int      instart[GO_MAXBLOCKS+1],inlist[GO_MAXEDGES];

static void Gobuildadjacency(t_gograph *g) {
  int i,e;
  memset(outstart,0,(size_t)(g->nblock+1)*sizeof(int));
  memset(instart,0,(size_t)(g->nblock+1)*sizeof(int));
  for (e=0; e<g->nedge; e++) {
    outstart[g->edge[e].from+1]++;
    instart[g->edge[e].to+1]++;
  };
  for (i=0; i<g->nblock; i++) {
    outstart[i+1]+=outstart[i];
    instart[i+1]+=instart[i];
  };
  for (e=0; e<g->nedge; e++) {
    outlist[outstart[g->edge[e].from]++]=e;
    inlist[instart[g->edge[e].to]++]=e;
  };
  // The counters now point at the end of each list: move them back to the start.
  for (i=g->nblock; i>0; i--) {
    outstart[i]=outstart[i-1];
    instart[i]=instart[i-1];
  };
  outstart[0]=instart[0]=0;
};

// Iterative DFS (no recursion: on obfuscated code the stack is a risk).
// It restarts from EVERY block not yet seen: since the graph can hold several
// expanded functions, it is no longer certain that everything is reachable
// from block 0. Each node on the stack remembers how far along its edge list
// it got, so every edge is looked at only once.
static void Gomarkbackedges(t_gograph *g) {
  static int stack[GO_MAXBLOCKS],next[GO_MAXBLOCKS];
  int sp,i,e,top,found,root;
  memset(visited,0,(size_t)g->nblock);
  memset(backedge,0,(size_t)g->nedge);
  for (root=0; root<g->nblock; root++) {
    if (visited[root]!=0)
      continue;
    sp=0;
    stack[sp++]=root;
    next[root]=outstart[root];
    visited[root]=1;
    while (sp>0) {
      top=stack[sp-1];
      found=0;
      while (next[top]<outstart[top+1]) {
        e=outlist[next[top]++];
        i=g->edge[e].to;
        if (visited[i]==1)
          backedge[e]=1;               // Backward edge: closes a loop
        else if (visited[i]==0) {
          visited[i]=1;
          next[i]=outstart[i];
          stack[sp++]=i;               // Each block is pushed only once
          found=1;
          break;
        };
      };
      if (found==0) {
        visited[top]=2;
        sp--;
      };
    };
  };
};

// Level = longest path from the entry, ignoring back edges. Without them the
// graph is acyclic, and the longest path comes out of a single pass in
// topological order (Kahn): before, it was a fixed point that re-read all
// edges up to one pass per level.
static int Goassignlevels(t_gograph *g) {
  static int indeg[GO_MAXBLOCKS],queue[GO_MAXBLOCKS];
  int i,k,e,b,head,tail,maxlevel;
  for (i=0; i<g->nblock; i++) {
    g->block[i].level=0;
    indeg[i]=0;
  };
  for (e=0; e<g->nedge; e++) {
    if (!backedge[e])
      indeg[g->edge[e].to]++;
  };
  head=tail=0;
  for (i=0; i<g->nblock; i++) {
    if (indeg[i]==0)
      queue[tail++]=i;
  };
  maxlevel=0;
  while (head<tail) {
    i=queue[head++];
    if (g->block[i].level>maxlevel)
      maxlevel=g->block[i].level;
    for (k=outstart[i]; k<outstart[i+1]; k++) {
      e=outlist[k];
      if (backedge[e])
        continue;
      b=g->edge[e].to;
      if (g->block[b].level<g->block[i].level+1)
        g->block[b].level=g->block[i].level+1;
      if (--indeg[b]==0)
        queue[tail++]=b;
    };
  };
  return maxlevel;
};

// Builds the node list: first the real blocks, then one dummy node for every
// level a long edge crosses. Returns 1 if at least one edge was left without a
// chain (node ceiling reached).
static int Gobuildnodes(t_gograph *g) {
  int i,e,l,la,lb,toomany;
  nnode=0;
  nchain=0;
  toomany=0;
  for (i=0; i<g->nblock && nnode<GO_MAXNODES; i++) {
    node[nnode].level=g->block[i].level;
    node[nnode].block=i;
    node[nnode].dx=g->block[i].dx;
    node[nnode].dy=g->block[i].dy;
    node[nnode].order=node[nnode].col=0;
    nnode++;
  };
  for (e=0; e<g->nedge; e++) {
    chainpos[e]=nchain;
    chainlen[e]=0;
    la=g->block[g->edge[e].from].level;
    lb=g->block[g->edge[e].to].level;
    // Backward or within the same level: no chain, the corridor handles it.
    // Splitting them with dummy nodes would mean negative levels.
    if (lb<=la || lb-la>GO_MAXSPAN)
      continue;
    // The chain takes lb-la+1 slots, not two: with the old check
    // ("nchain+2") a long edge at the end of the table wrote past chain[].
    if (nnode+(lb-la-1)>GO_MAXNODES ||
        nchain+(lb-la+1)>(int)(sizeof(chain)/sizeof(chain[0]))) {
      toomany=1;
      continue;
    };
    chain[nchain++]=g->edge[e].from;
    for (l=la+1; l<lb; l++) {
      node[nnode].level=l;
      node[nnode].block=-1;
      node[nnode].dx=GO_DUMMYW;
      node[nnode].dy=0;
      node[nnode].order=node[nnode].col=0;
      chain[nchain++]=nnode;
      nnode++;
    };
    chain[nchain++]=g->edge[e].to;
    chainlen[e]=nchain-chainpos[e];
  };
  return toomany;
};

// Segments between two nodes on different levels: the consecutive pairs of
// the chains (adjacent levels) plus the edges that have no chain (backward, or
// too long), which pull as well but do not count for crossings. "up" is the
// node on the higher level (lower number), "down" the other one.
#define GO_MAXSEG      (GO_MAXEDGES*2+GO_MAXNODES)
static int      segup[GO_MAXSEG],segdown[GO_MAXSEG],segw[GO_MAXSEG];
static int      nseg;
// Segment indices by level of the "up" node and by level of the "down" node.
static int      byup[GO_MAXSEG],bydown[GO_MAXSEG];
static int      upstart[GO_MAXBLOCKS+2],downstart[GO_MAXBLOCKS+2];
static int      bsum[GO_MAXNODES],bnum[GO_MAXNODES];

static void Gobuildsegments(t_gograph *g,int maxlevel) {
  int e,c,a,b,w,s,level;
  nseg=0;
  for (e=0; e<g->nedge; e++) {
    // The FALL-THROUGH weighs as much as four jumps: it is the natural
    // continuation of the code and must stay in its predecessor's column.
    // Without the weight, a block with many incoming jumps was pulled out of
    // its column and the fall-through became a detour as wide as the graph.
    w=(g->edge[e].fall ? 4 : 1);
    if (chainlen[e]>=2) {
      for (c=0; c<chainlen[e]-1; c++) {
        segup[nseg]=chain[chainpos[e]+c];
        segdown[nseg]=chain[chainpos[e]+c+1];
        segw[nseg++]=w;
      };
      continue;
    };
    a=g->edge[e].from; b=g->edge[e].to;
    if (node[a].level==node[b].level)
      continue;
    if (node[a].level>node[b].level) {
      c=a; a=b; b=c;
    };
    segup[nseg]=a; segdown[nseg]=b; segw[nseg++]=w;
  };
  memset(upstart,0,(size_t)(maxlevel+2)*sizeof(int));
  memset(downstart,0,(size_t)(maxlevel+2)*sizeof(int));
  for (s=0; s<nseg; s++) {
    upstart[node[segup[s]].level+1]++;
    downstart[node[segdown[s]].level+1]++;
  };
  for (level=0; level<=maxlevel; level++) {
    upstart[level+1]+=upstart[level];
    downstart[level+1]+=downstart[level];
  };
  for (s=0; s<nseg; s++) {
    byup[upstart[node[segup[s]].level]++]=s;
    bydown[downstart[node[segdown[s]].level]++]=s;
  };
  for (level=maxlevel+1; level>0; level--) {
    upstart[level]=upstart[level-1];
    downstart[level]=downstart[level-1];
  };
  upstart[0]=downstart[0]=0;
};

// Crossings between segments of adjacent levels, with the current order. For
// each pair of levels: the segments in lexicographic order (up, down) via two
// counting sorts, then the inversions of the "down" ends with a Fenwick tree.
// O(segments * log width), not O(segments^2).
static long Gocrossings(int maxlevel,int *lvlstart) {
  static int cnt[GO_MAXNODES+2],tmp1[GO_MAXSEG],tmp2[GO_MAXSEG],fen[GO_MAXNODES+2];
  int level,k,s,n,wup,wdown,o,j;
  long total,before;
  total=0;
  for (level=0; level<maxlevel; level++) {
    n=0;
    for (k=upstart[level]; k<upstart[level+1]; k++) {
      s=byup[k];
      if (node[segdown[s]].level==level+1)
        tmp1[n++]=s;
    };
    if (n<2)
      continue;
    wup=lvlstart[level+1]-lvlstart[level];
    wdown=lvlstart[level+2]-lvlstart[level+1];
    // By "down", then (stable) by "up".
    memset(cnt,0,(size_t)(wdown+1)*sizeof(int));
    for (k=0; k<n; k++) cnt[node[segdown[tmp1[k]]].order+1]++;
    for (k=0; k<wdown; k++) cnt[k+1]+=cnt[k];
    for (k=0; k<n; k++) tmp2[cnt[node[segdown[tmp1[k]]].order]++]=tmp1[k];
    memset(cnt,0,(size_t)(wup+1)*sizeof(int));
    for (k=0; k<n; k++) cnt[node[segup[tmp2[k]]].order+1]++;
    for (k=0; k<wup; k++) cnt[k+1]+=cnt[k];
    for (k=0; k<n; k++) tmp1[cnt[node[segup[tmp2[k]]].order]++]=tmp2[k];
    // Inversions: segments already seen whose "down" is strictly to the right.
    memset(fen,0,(size_t)(wdown+1)*sizeof(int));
    for (k=0; k<n; k++) {
      o=node[segdown[tmp1[k]]].order+1;
      before=0;
      for (j=o; j>0; j-=j&(-j)) before+=fen[j];
      total+=k-before;
      for (j=o; j<=wdown; j+=j&(-j)) fen[j]++;
    };
  };
  return total;
};

// Reorders one level by barycenter. Insertion sort on the range: levels are
// short, and the sort must be STABLE (nodes without neighbours stay where they
// are). The barycenter is computed once per node, not at every comparison.
static void Gosortlevel(int *seq,int lo,int hi) {
  static float key[GO_MAXNODES];
  int i,k,tmp;
  float ka;
  for (k=lo; k<hi; k++) {
    i=seq[k];
    key[i]=(bnum[i]>0 ? (float)bsum[i]/bnum[i] : (float)node[i].order);
  };
  for (i=lo+1; i<hi; i++) {
    tmp=seq[i];
    ka=key[tmp];
    for (k=i; k>lo && key[seq[k-1]]>ka; k--)
      seq[k]=seq[k-1];
    seq[k]=tmp;
  };
  for (k=lo; k<hi; k++)
    node[seq[k]].order=k-lo;
};

// Orders the nodes within each level with the barycenter method: a node goes
// where its neighbours on the adjacent level are, on average. Alternating
// passes (down, up), level by level: each level is reordered looking at the
// ALREADY updated order of the previous one, not the one from the previous
// pass. The method does not guarantee the optimum (the problem is NP-hard) and
// does not always improve: after each pass the crossings are counted and the
// best order seen is kept. Costs O(passes * segments * log width).
#ifndef GO_PASSES
#define GO_PASSES      8
#endif
static void Goorderlevels(t_gograph *g,int maxlevel,int *seq,int *lvlstart) {
  static int count[GO_MAXBLOCKS+3];
  static int best[GO_MAXNODES];
  int i,k,s,level,pass,from,to;
  long cross,bestcross;

  memset(count,0,(size_t)(maxlevel+3)*sizeof(int));
  for (i=0; i<nnode; i++)
    count[node[i].level+1]++;
  lvlstart[0]=0;
  for (level=0; level<=maxlevel; level++)
    lvlstart[level+1]=lvlstart[level]+count[level+1];
  for (level=0; level<=maxlevel; level++)
    count[level]=lvlstart[level];
  for (i=0; i<nnode; i++)
    seq[count[node[i].level]++]=i;
  for (level=0; level<=maxlevel; level++) {
    for (k=lvlstart[level]; k<lvlstart[level+1]; k++)
      node[seq[k]].order=k-lvlstart[level];
  };

  Gobuildsegments(g,maxlevel);
  bestcross=Gocrossings(maxlevel,lvlstart);
  memcpy(best,seq,(size_t)nnode*sizeof(int));
  for (pass=0; pass<GO_PASSES && bestcross>0; pass++) {
    for (k=0; k<=maxlevel; k++) {
      if ((pass&1)==0) {
        // Down: the level aligns to the ones above.
        level=k;
        from=downstart[level]; to=downstart[level+1];
        for (i=lvlstart[level]; i<lvlstart[level+1]; i++)
          bsum[seq[i]]=bnum[seq[i]]=0;
        for (i=from; i<to; i++) {
          s=bydown[i];
          bsum[segdown[s]]+=node[segup[s]].order*segw[s];
          bnum[segdown[s]]+=segw[s];
        };
      }
      else {
        // Up: the level aligns to the ones below.
        level=maxlevel-k;
        from=upstart[level]; to=upstart[level+1];
        for (i=lvlstart[level]; i<lvlstart[level+1]; i++)
          bsum[seq[i]]=bnum[seq[i]]=0;
        for (i=from; i<to; i++) {
          s=byup[i];
          bsum[segup[s]]+=node[segdown[s]].order*segw[s];
          bnum[segup[s]]+=segw[s];
        };
      };
      Gosortlevel(seq,lvlstart[level],lvlstart[level+1]);
    };
    cross=Gocrossings(maxlevel,lvlstart);
    if (cross<bestcross) {
      bestcross=cross;
      memcpy(best,seq,(size_t)nnode*sizeof(int));
    };
  };
  memcpy(seq,best,(size_t)nnode*sizeof(int));
  for (level=0; level<=maxlevel; level++) {
    for (k=lvlstart[level]; k<lvlstart[level+1]; k++)
      node[seq[k]].order=k-lvlstart[level];
  };
};

// Ports. A conditional jump ALWAYS has the false branch on the left and the
// true one on the right: the requirement is "readable at a glance, without
// reading". For all other blocks the exits are spread out sorted by the
// destination's x, so they do not cross right after leaving.
static void Goassignports(t_gograph *g) {
  int i,e,k,n,tmp,etrue,efalse;
  static int list[GO_MAXEDGES];

  for (i=0; i<g->nblock; i++) {
    g->block[i].nout=g->block[i].nin=0;
  };
  for (e=0; e<g->nedge; e++) {
    g->block[g->edge[e].from].nout++;
    g->block[g->edge[e].to].nin++;
  };
  for (i=0; i<g->nblock; i++) {
    n=0;
    etrue=efalse=-1;
    for (k=outstart[i]; k<outstart[i+1]; k++) {
      e=outlist[k];
      if (g->edge[e].type==GO_E_TRUE)  etrue=e;
      if (g->edge[e].type==GO_E_FALSE) efalse=e;
      list[n++]=e;
    };
    if (n==2 && etrue>=0 && efalse>=0) {
      edgeport[efalse]=0;              // false on the left
      edgeport[etrue] =1;              // true on the right
      continue;
    };
    for (k=1; k<n; k++) {
      int j=k;
      while (j>0 &&
             g->block[g->edge[list[j-1]].to].x > g->block[g->edge[list[j]].to].x) {
        tmp=list[j-1]; list[j-1]=list[j]; list[j]=tmp;
        j--;
      };
    };
    for (k=0; k<n; k++)
      edgeport[list[k]]=k;
  };
  for (i=0; i<g->nblock; i++) {
    n=0;
    for (k=instart[i]; k<instart[i+1]; k++)
      list[n++]=inlist[k];
    for (k=1; k<n; k++) {
      int j=k;
      while (j>0 &&
             g->block[g->edge[list[j-1]].from].x > g->block[g->edge[list[j]].from].x) {
        tmp=list[j-1]; list[j-1]=list[j]; list[j]=tmp;
        j--;
      };
    };
    for (k=0; k<n; k++)
      edgeport2[list[k]]=k;
  };
};

// Adds a point to the polyline, skipping duplicates (a repeated point draws
// nothing and uses up one of the GO_MAXPT).
static void Goaddpt(t_goroute *r,int x,int y) {
  if (r->np>0 && r->x[r->np-1]==x && r->y[r->np-1]==y)
    return;
  if (r->np>=GO_MAXPT)
    return;
  r->x[r->np]=x;
  r->y[r->np]=y;
  r->np++;
};

// Routing. cy[L] = centre of the channel BELOW level L.
static void Goroute(t_gograph *g,int maxlevel,int ncol,
                    int *rowy,int *rowheight,int *colx) {
  int e,a,b,la,lb,x1,y1,x2,y2,k,lane,ych,ych2,xc,c,cur,nxt,xp;
  int *lanech,*lanecor;
  t_goblock *pa,*pb;
  t_goroute *r;

  g->route=(t_goroute *)calloc((size_t)(g->nedge>0 ? g->nedge : 1),sizeof(t_goroute));
  if (g->route==NULL)
    return;
  g->nroute=g->nedge;
  lanech =(int *)calloc((size_t)maxlevel+3,sizeof(int));
  lanecor=(int *)calloc((size_t)ncol+3,sizeof(int));
  if (lanech==NULL || lanecor==NULL) {
    if (lanech!=NULL) free(lanech);
    if (lanecor!=NULL) free(lanecor);
    return;
  };

  for (e=0; e<g->nedge; e++) {
    a=g->edge[e].from; b=g->edge[e].to;
    pa=&(g->block[a]); pb=&(g->block[b]);
    la=pa->level; lb=pb->level;
    x1=pa->x+pa->dx*(edgeport[e]+1)/(pa->nout+1);
    y1=pa->y+pa->dy;
    x2=pb->x+pb->dx*(edgeport2[e]+1)/(pb->nin+1);
    y2=pb->y;
    r=&(g->route[e]);
    r->np=0;
    Goaddpt(r,x1,y1);

    if (chainlen[e]>=2 && chainlen[e]<=GO_MAXPT/2) {
      // Chain of dummy nodes: go down level by level through the column
      // reserved for this edge. No long horizontal segment.
      xp=x1;
      for (c=0; c<chainlen[e]-1; c++) {
        cur =chain[chainpos[e]+c];
        nxt =chain[chainpos[e]+c+1];
        lane=lanech[node[cur].level]++;
        ych=rowy[node[cur].level]+rowheight[node[cur].level]+GO_GAPY/2
            +((lane%GO_NLANE)-GO_NLANE/2)*GO_LANE;
        xc=(node[nxt].block>=0 ? x2 : node[nxt].x+node[nxt].dx/2);
        Goaddpt(r,xp,ych);
        Goaddpt(r,xc,ych);
        xp=xc;
      };
    }
    else if (lb==la+1) {
      // Adjacent levels: the channel is enough, no corridor.
      lane=lanech[la]++;
      ych=rowy[la]+rowheight[la]+GO_GAPY/2
          +((lane%GO_NLANE)-GO_NLANE/2)*GO_LANE;
      if (x1!=x2) {
        Goaddpt(r,x1,ych);
        Goaddpt(r,x2,ych);
      };
    }
    else {
      // Backward, within the same level, or chain too long: go through a
      // vertical corridor, where there are no blocks.
      lane=lanech[la]++;
      ych=rowy[la]+rowheight[la]+GO_GAPY/2
          +((lane%GO_NLANE)-GO_NLANE/2)*GO_LANE;
      k=(g->block[a].x<g->block[b].x ? pb->col : pb->col+1);
      if (k<0) k=0;
      if (k>ncol) k=ncol;
      lane=lanecor[k]++;
      xc=colx[k]-GO_GAPX/2+((lane%GO_NLANE)-GO_NLANE/2)*GO_LANE;
      ych2=rowy[lb]-GO_GAPY/2+((lane%GO_NLANE)-GO_NLANE/2)*GO_LANE;
      if (lb==0)
        ych2=GO_MARGIN/2;
      Goaddpt(r,x1,ych);
      Goaddpt(r,xc,ych);
      Goaddpt(r,xc,ych2);
      Goaddpt(r,x2,ych2);
    };
    Goaddpt(r,x2,y2);
  };
  free(lanech);
  free(lanecor);
};

// Assigns x,y in graph pixels and computes the paths. hdc is only used to
// measure the fixed font.
void Golayout(t_gograph *g,HDC hdc) {
  int i,j,k,level,maxlevel,cw,ch,len,maxlen,ncol,n,base,toomany;
  int *rowheight,*rowy,*seq,*lvlstart,*colw,*colx;
  ulong t0;
  SIZE sz;
  TEXTMETRIC tm;

  if (g->nblock<=0)
    return;
  if (g->route!=NULL) {                // Relayout after an expansion
    free(g->route);
    g->route=NULL;
    g->nroute=0;
  };
  g->width=g->height=0;

  t0=GetTickCount();
  GetTextMetrics(hdc,&tm);
  cw=tm.tmAveCharWidth;
  ch=tm.tmHeight;
  if (GetTextExtentPoint32(hdc,"0",1,&sz))
    cw=sz.cx;

  Gobuildadjacency(g);
  Gomarkbackedges(g);
  maxlevel=Goassignlevels(g);

  // Block sizes from the text.
  for (i=0; i<g->nblock; i++) {
    maxlen=0;
    for (j=0; j<g->block[i].nlines; j++) {
      len=strlen(GOLINE(g,&(g->block[i]),j).text);
      if (len>maxlen)
        maxlen=len;
    };
    if (maxlen<8)
      maxlen=8;
    g->block[i].dx=maxlen*cw+2*GO_PADX;
    g->block[i].dy=(g->block[i].nlines>0 ? g->block[i].nlines : 1)*ch+2*GO_PADY;
  };

  toomany=Gobuildnodes(g);

  rowheight=(int *)calloc(maxlevel+2,sizeof(int));
  rowy     =(int *)calloc(maxlevel+2,sizeof(int));
  seq      =(int *)malloc((size_t)(nnode>0 ? nnode : 1)*sizeof(int));
  lvlstart =(int *)malloc((size_t)(maxlevel+2)*sizeof(int));
  if (rowheight==NULL || rowy==NULL || seq==NULL || lvlstart==NULL) {
    if (rowheight!=NULL) free(rowheight);
    if (rowy!=NULL)      free(rowy);
    if (seq!=NULL)       free(seq);
    if (lvlstart!=NULL)  free(lvlstart);
    return;
  };

  // Height of each level = tallest block on the level, then cumulative y.
  // Below the tallest block there is room for the "unresolved exit" note.
  for (i=0; i<g->nblock; i++) {
    if (g->block[i].dy>rowheight[g->block[i].level])
      rowheight[g->block[i].level]=g->block[i].dy;
  };
  for (level=0; level<=maxlevel; level++) {
    rowheight[level]+=ch;
    rowy[level]=(level==0 ? GO_MARGIN : rowy[level-1]+rowheight[level-1]+GO_GAPY);
  };

  Goorderlevels(g,maxlevel,seq,lvlstart);

  // Columns shared by all levels. A level with few nodes puts them in the
  // CENTRE of the grid: this way a graph that is narrow at the top does not
  // lean entirely to the left, and the corridors stay the same at every height.
  ncol=0;
  for (level=0; level<=maxlevel; level++) {
    n=lvlstart[level+1]-lvlstart[level];
    if (n>ncol)
      ncol=n;
  };
  if (ncol<1)
    ncol=1;
  colw=(int *)calloc(ncol+2,sizeof(int));
  colx=(int *)calloc(ncol+2,sizeof(int));
  if (colw==NULL || colx==NULL) {
    if (colw!=NULL) free(colw);
    if (colx!=NULL) free(colx);
    free(rowheight); free(rowy); free(seq); free(lvlstart);
    return;
  };
  for (level=0; level<=maxlevel; level++) {
    n=lvlstart[level+1]-lvlstart[level];
    base=(ncol-n)/2;
    for (j=0; j<n; j++) {
      i=seq[lvlstart[level]+j];
      node[i].col=base+j;
      if (node[i].dx>colw[base+j])
        colw[base+j]=node[i].dx;
    };
  };
  // Straightening the fall-throughs. Columns are assigned, but the barycenter
  // works on averages and a fall-through can end up off to the side. Here,
  // level by level, a block that is its predecessor's fall-through moves into
  // the predecessor's column if that column is free at that level: this is
  // the swap that keeps the main column of the code straight.
  //
  // Fall-throughs between adjacent levels have a chain too (of two nodes, no
  // dummies): up to 0.1.5 the "chainlen>=2" filter discarded ALL of them and
  // this pass moved nothing. Only the level is checked.
  //
  // Fall-throughs are collected by destination level, in edge order: before,
  // every level re-read all edges.
  {
    static char taken[GO_MAXNODES+2];
    static int  fall[GO_MAXEDGES],fallstart[GO_MAXBLOCKS+2];   // levels <= blocks
    int e2,src,dst,wanted,other,j2;
    memset(fallstart,0,(size_t)(maxlevel+2)*sizeof(int));
    for (e2=0; e2<g->nedge; e2++) {
      if (g->edge[e2].fall)
        fallstart[node[g->edge[e2].to].level+1]++;
    };
    for (level=0; level<=maxlevel; level++)
      fallstart[level+1]+=fallstart[level];
    for (e2=0; e2<g->nedge; e2++) {
      if (g->edge[e2].fall)
        fall[fallstart[node[g->edge[e2].to].level]++]=e2;
    };
    for (level=maxlevel; level>0; level--)
      fallstart[level]=fallstart[level-1];
    fallstart[0]=0;
    for (level=1; level<=maxlevel; level++) {
      memset(taken,0,(size_t)ncol+2);
      for (j2=lvlstart[level]; j2<lvlstart[level+1]; j2++)
        taken[node[seq[j2]].col]=1;
      for (j2=fallstart[level]; j2<fallstart[level+1]; j2++) {
        e2=fall[j2];
        src=g->edge[e2].from; dst=g->edge[e2].to;
        if (node[src].level!=level-1)
          continue;
        wanted=node[src].col;
        if (node[dst].col==wanted || wanted<0 || wanted>=ncol)
          continue;
        if (taken[wanted])
          continue;                    // Someone else is already there: leave it
        other=node[dst].col;
        taken[other]=0;
        node[dst].col=wanted;
        taken[wanted]=1;
      };
    };
    // Column widths must be recomputed after the moves.
    memset(colw,0,(size_t)(ncol+2)*sizeof(int));
    for (i=0; i<nnode; i++) {
      if (node[i].dx>colw[node[i].col])
        colw[node[i].col]=node[i].dx;
    };
  };

  colx[0]=GO_MARGIN+GO_GAPX;
  for (k=0; k<ncol; k++)
    colx[k+1]=colx[k]+colw[k]+GO_GAPX;

  for (i=0; i<nnode; i++) {
    k=node[i].col;
    node[i].x=colx[k]+(colw[k]-node[i].dx)/2;
    node[i].y=rowy[node[i].level];
    if (node[i].block>=0) {
      g->block[node[i].block].x=node[i].x;
      g->block[node[i].block].y=node[i].y;
      g->block[node[i].block].order=node[i].order;
    };
  };
  g->width =colx[ncol]+GO_MARGIN;
  g->height=rowy[maxlevel]+rowheight[maxlevel]+GO_MARGIN;

  Goassignports(g);
  // The columns of real blocks are needed to route back edges.
  for (i=0; i<nnode; i++) {
    if (node[i].block>=0)
      g->block[node[i].block].col=node[i].col;
  };
  Goroute(g,maxlevel,ncol,rowy,rowheight,colx);

  free(colw); free(colx);
  free(seq);  free(lvlstart);
  free(rowheight); free(rowy);
  Gotrace("  layout: %d levels, %d columns, %d nodes (%d dummy)%s, %dx%d px, %lu ms",
    maxlevel+1,ncol,nnode,nnode-g->nblock,(toomany ? " [node ceiling reached]" : ""),
    g->width,g->height,GetTickCount()-t0);
};
