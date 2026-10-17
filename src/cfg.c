////////////////////////////////////////////////////////////////////////////////
// GraphOlly - extraction of a function's control flow graph.
//
// Recursive descent from the chosen point, not a linear sweep: ONLY reachable
// code is visited, so a function's graph does not drag along the tail of the
// section when Olly does not know where the procedure ends.
//
//   pass 1  visit: marks reachable instruction starts and leaders (entry,
//           jump destinations, the byte after a conditional jump);
//   pass 2  cuts blocks at the leaders, following the visited instructions;
//   pass 3  edges.
//
// A scan APPENDS to the graph instead of clearing it: this way a double click
// on a CALL expands the called function next to the starting one, inside the
// same graph (Goexpand). The blocks of one scan stay contiguous and sorted by
// address, so lookup within a scan stays binary.
//
// Memory is ALWAYS read with MM_RESTORE, otherwise the breakpoints' 0xCC bytes
// show up in the graph. Disassembly uses Olly's decode array when there is one
// (Finddecode), so instruction boundaries are those of the analysis.
//
// Every loop has a hard ceiling: on obfuscated code the descent must not be
// able to spin forever. Olly's strength is that it does not crash; a plugin
// that brings it down cancels that strength.
////////////////////////////////////////////////////////////////////////////////

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plugin_packed.h"
#include "grapholly.h"

#define GO_V_CODE      0x01            // Start of a reachable instruction
#define GO_V_LEADER    0x02            // Start of a block
#define GO_V_TERM      0x04            // Instruction that ends the block
#define GO_V_QUEUED    0x08            // Already queued (once is enough)

static uchar  *govisit;                // One flag per byte of the window
static ulong  gobase,gosize;

static ulong  goqueue[GO_MAXBLOCKS];
static int    nqueue;
static int    goqfull;                 // 1 if the queue had to drop something

#define GO_MAXSW       1024            // Switch cases per scan
static ulong  goswfrom[GO_MAXSW];      // Indirect jump ...
static ulong  goswto[GO_MAXSW];        // ... and one of its cases
static int    nsw;

static void Gomark(ulong addr,uchar flag) {
  if (addr>=gobase && addr<gobase+gosize)
    govisit[addr-gobase]|=flag;
};

static int Goflag(ulong addr,uchar flag) {
  if (addr<gobase || addr>=gobase+gosize)
    return 0;
  return (govisit[addr-gobase] & flag)!=0;
};

// An address enters the queue only once: the GO_V_QUEUED flag replaces a linear
// search of the queue (up to two million comparisons with a full queue). If the
// queue is full the address is lost, and the graph must be reported truncated.
static void Gopush(ulong addr) {
  if (addr<gobase || addr>=gobase+gosize)
    return;
  if (Goflag(addr,GO_V_CODE|GO_V_QUEUED))
    return;                            // Already visited or already queued
  if (nqueue>=GO_MAXBLOCKS) {
    goqfull=1;
    return;
  };
  Gomark(addr,GO_V_QUEUED);
  goqueue[nqueue++]=addr;
};

// Block that STARTS at addr, searched within the range [lo,hi) produced by a
// single scan. The blocks of a scan are created in increasing address order
// (pass 2 walks the window upwards), so the search is binary: with 2048 blocks
// the old linear search cost up to 4 million comparisons. Across different
// scans the global order no longer holds, which is why the range is passed
// explicitly.
static int Gofindblockin(t_gograph *g,int lo,int hi,ulong addr) {
  int mid;
  hi--;
  while (lo<=hi) {
    mid=(lo+hi)/2;
    if (g->block[mid].start==addr)
      return mid;
    if (g->block[mid].start<addr)
      lo=mid+1;
    else
      hi=mid-1;
  };
  return -1;
};

int Gofindblock(t_gograph *g,ulong addr) {
  int i;
  for (i=0; i<g->nblock; i++) {
    if (g->block[i].start==addr)
      return i;
  };
  return -1;
};

// Block that CONTAINS addr (not only the one starting there), or -1.
int Goblockat(t_gograph *g,ulong addr) {
  int i;
  if (g==NULL)
    return -1;
  for (i=0; i<g->nblock; i++) {
    if (addr>=g->block[i].start && addr<g->block[i].end)
      return i;
  };
  return -1;
};

// --- Line pool ---------------------------------------------------------------
// Grows in steps; the graph of an ordinary function makes it grow once or
// twice at most.
#define GO_LINECHUNK 512

static t_goline *Goaddline(t_gograph *g) {
  t_goline *p;
  int n;
  if (g->nline>=g->maxline) {
    n=(g->maxline==0 ? GO_LINECHUNK : g->maxline*2);
    p=(t_goline *)realloc(g->line,(size_t)n*sizeof(t_goline));
    if (p==NULL)
      return NULL;
    g->line=p;
    g->maxline=n;
  };
  return &(g->line[g->nline++]);
};

void Gofreegraph(t_gograph *g) {
  if (g==NULL)
    return;
  if (g->line!=NULL)
    free(g->line);
  if (g->route!=NULL)
    free(g->route);
  free(g);
};

// "fall" tells the FALL-THROUGH (the next block in memory) apart from a real
// jump. They are two different things even if both are an edge in the graph:
// the fall-through is the natural continuation of the code, and the layout
// keeps it in the column below its predecessor.
static void Goaddedge(t_gograph *g,int from,int to,int type,int fall) {
  if (from<0 || to<0)
    return;
  if (g->nedge>=GO_MAXEDGES) {
    g->truncated=1;                    // A lost edge means an incomplete graph
    return;
  };
  g->edge[g->nedge].from=from;
  g->edge[g->nedge].to=to;
  g->edge[g->nedge].type=type;
  g->edge[g->nedge].fall=fall;
  g->nedge++;
};

// Functions that do not return. After a CALL to one of these the compiler puts
// nothing meaningful: padding, and then the next function. Without this list
// the graph of an entry point ending in exit() dragged along the whole next
// function. They are recognised by the name Olly writes in the instruction
// text, "<JMP.&msvcrt.exit>" or "[<&KERNEL32.ExitProcess>]".
static const char *gonoreturn[] = {
  "ExitProcess", "ExitThread", "FatalExit", "FatalAppExitA", "FatalAppExitW",
  "RtlExitUserProcess", "RtlExitUserThread", "RaiseFailFastException",
  "exit", "_exit", "abort", "_amsg_exit", "longjmp", "_CxxThrowException",
  "__report_gsfailure", "_invalid_parameter_noinfo_noreturn", NULL };

static int Gonoreturn(const char *text) {
  int i,n;
  const char *p;
  for (i=0; gonoreturn[i]!=NULL; i++) {
    n=strlen(gonoreturn[i]);
    for (p=strchr(text,'.'); p!=NULL; p=strchr(p+1,'.'))
      if (strncmp(p+1,gonoreturn[i],n)==0 && p[1+n]=='>')
        return 1;
  };
  return 0;
};

// CALLs do not end a block: in a function's graph a call is a line like any
// other. Except the ones that do not return.
static int Gotermtype(t_disasm *da) {
  int t;
  ulong target;
  t=da->cmdtype & C_TYPEMASK;
  target=da->jmpconst;
  if (t==C_RET)
    return GO_T_RET;
  if (t==C_CAL && Gonoreturn(da->result))
    return GO_T_RET;
  if (t==C_JMC)
    return (target!=0 ? GO_T_COND : GO_T_UNRES);
  if (t==C_JMP || t==C_RTF)
    return (target!=0 ? GO_T_JMP : GO_T_UNRES);
  if (t==C_BAD)
    return GO_T_UNRES;
  return GO_T_FALL;
};

////////////////////////////////////////////////////////////////////////////////
// One scan: from a function's bounds to its blocks and edges, APPENDED after
// those already in the graph. Returns the index of the first block produced,
// or -1.
////////////////////////////////////////////////////////////////////////////////

static int Goscan(t_gograph *g,ulong addr) {
  ulong start,end,ip,next,target,limit;
  uchar *code,*decode;
  ulong decodesize;
  t_memory *pmem;
  t_module *pmod;
  t_disasm da;
  ulong cmdsize;
  int i,k,e,e0,term,index,guard,first;
  t_goline *line;

  nqueue=0;
  goqfull=0;

  // Bounds. Findprocbegin/Findprocend use Olly's analysis; Getproclimits only
  // answers for procedures registered in the .udd, so it is not relied upon.
  start=Findprocbegin(addr);
  end=Findprocend(addr);
  if (start==0 || end<=start) {
    // The module is not analysed: without analysis Olly does not know where a
    // function starts and ends, and the graph comes out flagged "no
    // procedure". Olly is made to do what the user would do with Ctrl+A, once,
    // and then it retries. Analysecode is exported by the PDK, so it also
    // works on patched builds.
    pmod=Findmodule(addr);
    if (pmod!=NULL)
      Gotrace("  bounds unknown at %08lX: module \"%s\" base=%08lX code %08lX+%lu "
        "codedec=%p processed=%d",
        addr,pmod->name,pmod->base,pmod->codebase,pmod->codesize,
        pmod->codedec,pmod->processed);
    else
      Gotrace("  bounds unknown at %08lX: no module",addr);
    if (pmod!=NULL && pmod->codedec==NULL && pmod->codesize>0) {
      Gotrace("  module not analysed: calling Analysecode -> %d",Analysecode(pmod));
      start=Findprocbegin(addr);
      end=Findprocend(addr);
      Gotrace("  after analysis: %08lX-%08lX codedec=%p",start,end,pmod->codedec);
    };
  };
  // Bounds that do not contain addr (stale analysis, overlapping procedure;
  // >end and not >=end because it is not established whether Findprocend
  // returns the last byte or the one after it):
  // the window would not even see the starting point and the graph would come
  // out empty. They are treated as unknown bounds.
  if (addr<start || addr>end)
    start=end=0;
  if (start==0 || end<=start) {
    // Olly does not recognise a procedure here (entry point, code not yet
    // analysed, jump into nowhere). Start from the chosen address and explore
    // within its memory block: with recursive descent only what is reachable
    // is visited, not the whole section.
    pmem=Findmemory(addr);
    if (pmem==NULL) {
      Flash("GraphOlly: no known function or memory at %08lX",addr);
      return -1;
    };
    start=addr;
    end=pmem->base+pmem->size;
    // This is not "truncated": the descent may well cover the whole function.
    // It is "no procedure": Olly does not know where it starts and ends, so
    // the graph starts from the chosen point and is not guaranteed to be the
    // whole function. Two different things, and they must be reported
    // differently.
    g->noproc=1;
  };
  gobase=start;
  gosize=end-start;
  if (gosize>GO_MAXCODE) {
    gosize=GO_MAXCODE;
    end=gobase+gosize;
    g->truncated=1;
  };

  code=(uchar *)malloc(gosize);
  govisit=(uchar *)calloc(gosize,1);
  if (code==NULL || govisit==NULL) {
    if (code!=NULL) free(code);
    if (govisit!=NULL) free(govisit);
    govisit=NULL;
    return -1;
  };
  if (Readmemory(code,gobase,gosize,MM_RESTORE|MM_SILENT)!=gosize) {
    free(code); free(govisit); govisit=NULL;
    Flash("GraphOlly: memory not readable at %08lX",gobase);
    return -1;
  };
  decodesize=0;
  decode=Finddecode(gobase,&decodesize);

  // Switches: Olly recognises jump tables and keeps their cases in jddata
  // (JT_SWITCH, from the indirect jump to each case). Without them a
  // "jmp [eax*4+tab]" is an unresolved exit and the cases stay out of the
  // graph. Only cases with both ends inside the window are kept.
  nsw=0;
  pmod=Findmodule(gobase);
  if (pmod!=NULL && pmod->jddata!=NULL) {
    for (k=0; k<pmod->njddata; k++) {
      if (pmod->jddata[k].type!=JT_SWITCH ||
          pmod->jddata[k].from<gobase || pmod->jddata[k].from>=end ||
          pmod->jddata[k].to<gobase || pmod->jddata[k].to>=end)
        continue;
      if (nsw>=GO_MAXSW) {
        g->truncated=1;
        break;
      };
      goswfrom[nsw]=pmod->jddata[k].from;
      goswto[nsw]=pmod->jddata[k].to;
      nsw++;
    };
  };

  // --- Pass 1: visit --------------------------------------------------------
  Gomark(addr,GO_V_LEADER);
  Gopush(addr);
  guard=0;
  while (nqueue>0) {
    ip=goqueue[--nqueue];
    while (ip>=gobase && ip<end) {
      if (++guard>=GO_MAXCODE) {
        g->truncated=1;
        break;
      };
      if (Goflag(ip,GO_V_CODE)) {
        // Joining code that was already visited. In normal code there is
        // already a leader there; in obfuscated code with OVERLAPPING
        // instructions (a jump into the middle of an instruction, the two
        // flows meeting again further on) there is not, and without a leader
        // this flow's block dragged along a copy of the other one's tail.
        Gomark(ip,GO_V_LEADER);
        break;
      };
      cmdsize=Disasm(code+(ip-gobase),end-ip,ip,
        (decode!=NULL && ip-gobase<decodesize ? decode+(ip-gobase) : NULL),
        &da,DISASM_CODE,0);
      if (cmdsize==0) {
        g->truncated=1;
        break;                         // Undecodable byte
      };
      Gomark(ip,GO_V_CODE);
      next=ip+cmdsize;
      term=Gotermtype(&da);
      if (term==GO_T_FALL) {
        ip=next;
        continue;
      };
      Gomark(ip,GO_V_TERM);
      target=da.jmpconst;
      if (term==GO_T_JMP || term==GO_T_COND) {
        Gomark(target,GO_V_LEADER);
        Gopush(target);
      };
      if (term==GO_T_COND) {
        Gomark(next,GO_V_LEADER);
        Gopush(next);
      };
      if (term==GO_T_UNRES) {
        for (k=0; k<nsw; k++) {        // Switch recognised by Olly?
          if (goswfrom[k]!=ip)
            continue;
          Gomark(goswto[k],GO_V_LEADER);
          Gopush(goswto[k]);
        };
      };
      break;                           // The block ends here
    };
    if (guard>=GO_MAXCODE)
      break;
  };
  if (goqfull)
    g->truncated=1;

  // --- Pass 2: blocks, text and destinations --------------------------------
  // A single pass: while the block is cut, the text is formatted, the
  // destination of the final jump is stored and, line by line, the destination
  // of any CALL (that is what a double click expands).
  first=g->nblock;
  for (ip=gobase; ip<end; ip++) {
    if (!Goflag(ip,GO_V_CODE) || !Goflag(ip,GO_V_LEADER))
      continue;
    if (g->nblock>=GO_MAXBLOCKS) {
      g->truncated=1;
      break;
    };
    i=g->nblock++;
    memset(&(g->block[i]),0,sizeof(t_goblock));
    g->block[i].start=ip;
    g->block[i].func=gobase;
    g->block[i].level=-1;
    g->block[i].term=GO_T_FALL;
    g->block[i].firstline=g->nline;
    // The block reaches its terminator, or the next leader.
    limit=ip;
    while (limit<end) {
      cmdsize=Disasm(code+(limit-gobase),end-limit,limit,
        (decode!=NULL && limit-gobase<decodesize ? decode+(limit-gobase) : NULL),
        &da,DISASM_CODE,0);
      if (cmdsize==0)
        break;
      if (g->block[i].nlines<GO_MAXLINES) {
        line=Goaddline(g);
        if (line==NULL) {              // Out of memory: keep what is there
          g->truncated=1;
          break;
        };
        line->addr=limit;
        line->calltarget=
          (((da.cmdtype & C_TYPEMASK)==C_CAL) ? da.jmpconst : 0);
        sprintf(line->text,"%08lX  %-.40s",limit,da.result);
        g->block[i].nlines++;
      }
      else
        g->truncated=1;                // Block longer than GO_MAXLINES lines
      if (Goflag(limit,GO_V_TERM)) {
        g->block[i].term=Gotermtype(&da);
        g->block[i].target=da.jmpconst;
        // An indirect jump whose cases Olly knows is not an unresolved exit:
        // it becomes a jump with several destinations (target 0, pass 3 adds
        // the edges).
        if (g->block[i].term==GO_T_UNRES) {
          for (k=0; k<nsw; k++) {
            if (goswfrom[k]==limit) {
              g->block[i].term=GO_T_JMP;
              break;
            };
          };
        };
        limit+=cmdsize;
        break;
      };
      limit+=cmdsize;
      if (Goflag(limit,GO_V_LEADER))
        break;                         // Falls into the next block
      if (!Goflag(limit,GO_V_CODE))
        break;                         // End of reachable code
    };
    g->block[i].end=limit;
  };

  // --- Pass 3: edges --------------------------------------------------------
  // The edges of this scan stay within this scan: a jump that lands in an
  // already expanded function is not a flow edge.
  for (i=first; i<g->nblock; i++) {
    target=g->block[i].target;
    switch (g->block[i].term) {
      case GO_T_FALL:
        index=Gofindblockin(g,first,g->nblock,g->block[i].end);
        Goaddedge(g,i,index,GO_E_JMP,1);
        break;
      case GO_T_JMP:
        if (target!=0) {
          Goaddedge(g,i,Gofindblockin(g,first,g->nblock,target),GO_E_JMP,0);
          break;
        };
        // Switch: one edge per case, no duplicates (several cases often go to
        // the same block). The block's only jump is its terminator, so it is
        // enough for the source to fall inside the block.
        e0=g->nedge;
        for (k=0; k<nsw; k++) {
          if (goswfrom[k]<g->block[i].start || goswfrom[k]>=g->block[i].end)
            continue;
          index=Gofindblockin(g,first,g->nblock,goswto[k]);
          if (index<0)
            continue;
          for (e=e0; e<g->nedge; e++) {
            if (g->edge[e].to==index)
              break;
          };
          if (e>=g->nedge)
            Goaddedge(g,i,index,GO_E_JMP,0);
        };
        break;
      case GO_T_COND:
        Goaddedge(g,i,Gofindblockin(g,first,g->nblock,target),GO_E_TRUE,0);
        Goaddedge(g,i,Gofindblockin(g,first,g->nblock,g->block[i].end),GO_E_FALSE,1);
        break;
      default:
        break;                         // RET and unresolved exits: no edge
    };
  };

  Gotrace("  CFG %08lX: blocks %d..%d, %d edges, window %08lX-%08lX%s%s",
    addr,first,g->nblock-1,g->nedge,gobase,end,
    (g->truncated ? " [truncated]" : ""),(g->noproc ? " [no procedure]" : ""));

  free(code);
  free(govisit);
  govisit=NULL;
  if (g->nfunc<GO_MAXFUNCS)
    g->func[g->nfunc++]=gobase;
  return (g->nblock>first ? first : -1);
};

// Builds the graph of the function containing addr. Returns 1 on success.
int Gobuildgraph(ulong addr,t_gograph *g) {
  ulong t0;
  char name[TEXTLEN];
  int first;

  t0=GetTickCount();
  memset(g,0,sizeof(t_gograph));       // g always arrives fresh from the caller

  name[0]='\0';
  if (Findname(addr,NM_LABEL,name)==0)
    Decodeaddress(addr,0,ADC_VALID|ADC_SYMBOL,name,sizeof(name),NULL);
  if (name[0]=='\0')
    sprintf(g->title,"%08lX",addr);
  else
    sprintf(g->title,"%.40s (%08lX)",name,addr);

  first=Goscan(g,addr);
  if (first<0)
    return 0;
  g->procstart=g->block[first].func;
  g->procend=g->procstart;
  Gotrace("  cost: build %lu ms, %d lines in the pool (%d KB), struct %d KB",
    GetTickCount()-t0,g->nline,(int)((g->maxline*sizeof(t_goline))/1024),
    (int)(sizeof(t_gograph)/1024));
  return (g->nblock>0);
};

// Expands into the graph the function starting at addr, with a dashed edge
// from the block that calls it. Returns the index of its first block, or -1.
// If it was already in, it still returns the index, so the caller can scroll
// to it.
int Goexpand(t_gograph *g,ulong addr,int fromblock) {
  int i,first;
  // Already expanded? It is not duplicated: return the block that starts it.
  // Only addr is checked: the destination of a CALL is an entry by definition.
  // Asking Olly where its procedure starts went wrong when Olly merges two of
  // them (a CALL exit that it thinks returns): the called function showed up
  // as "already in the graph" and the double click did nothing.
  for (i=0; i<g->nblock; i++) {
    if (g->block[i].start==addr) {
      Gotrace("  expand: %08lX already in the graph, block %d",addr,i);
      return i;
    };
  };
  if (g->nfunc>=GO_MAXFUNCS) {
    Flash("GraphOlly: already %d functions expanded in this graph",g->nfunc);
    return -1;
  };
  first=Goscan(g,addr);
  if (first<0)
    return -1;
  Goaddedge(g,fromblock,first,GO_E_CALL,0);
  Gotrace("  expand: %08lX expanded, block %d, now %d blocks",
    addr,first,g->nblock);
  return first;
};
