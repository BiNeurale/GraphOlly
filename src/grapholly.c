////////////////////////////////////////////////////////////////////////////////
// GraphOlly - OllyDbg 1.10 plugin: control-flow graph drawn inside Olly.
//
// Mandatory PDK 1.10 exports, exported BY NAME (never by ordinal).
// Build 32-bit, byte alignment (plugin.h requires it), unsigned char.
////////////////////////////////////////////////////////////////////////////////

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include "plugin_packed.h"
#include "grapholly.h"

HINSTANCE        hinst;                // DLL instance
HWND             hwmain;               // OllyDbg main window
int              gofolloweip=1;        // Graph follows EIP (ini: FollowEIP)
// Shortcut. It CANNOT be Ctrl+G: Olly uses it for "Go to expression" and never
// passes it to the plugin. Nor Ctrl+Shift+G - tested by hand: Olly ignores the
// Shift and opens its own dialog anyway. The letters 1.10 binds with Ctrl,
// read from the menus inside its executable, are A B C E F G J K L N O R S T;
// D H I M P Q U V W X Y Z are free.
static char *Goshortname(void);

int              gofollowfunc=1;       // The graph switches function by itself
                                       // when execution enters another one
int              golabels=0;           // T/F on edges: off, colour and the
                                       // fixed port are enough (ini: Labels)
int              gominimap=1;          // Minimap in the corner (ini: Minimap)
int              goshortkey='D';
int              goshortshift=0;

// Diagnostic trace. OFF by default: enable it with Trace=1 in the
// [Plugin GraphOlly] section of ollydbg.ini. Written next to Olly's executable.
// Olly does not say out loud why it rejects a plugin, and its Log cannot be
// read from outside: without this trace, diagnosis is guesswork.
static int gotraceon=-1;               // -1 = not read from the ini yet

void Gotrace(const char *format,...) {
  FILE *f;
  va_list arg;
  if (gotraceon<=0)
    return;                            // Off, or ini not read yet
  f=fopen("grapholly-trace.txt","at");
  if (f==NULL)
    return;
  va_start(arg,format);
  vfprintf(f,format,arg);
  va_end(arg);
  fputc('\n',f);
  fclose(f);
};

BOOL WINAPI DllEntryPoint(HINSTANCE hi,DWORD reason,LPVOID reserved) {
  if (reason==DLL_PROCESS_ATTACH)
    hinst=hi;
  return TRUE;
};

BOOL WINAPI DllMain(HINSTANCE hi,DWORD reason,LPVOID reserved) {
  if (reason==DLL_PROCESS_ATTACH) {
    hinst=hi;
    Gotrace("--- DllMain ATTACH hinst=%p",hi);
  }
  else if (reason==DLL_PROCESS_DETACH)
    Gotrace("--- DllMain DETACH");
  return TRUE;
};

////////////////////////////////////////////////////////////////////////////////
// Service port for automated QA.
//
// Under headless wine no synthetic input reaches OllyDbg (neither XTEST nor
// keybd_event), so an automated test cannot ask for the graph from the menu.
// This hidden window receives a registered message and calls THE SAME function
// as the menu item.
//
// WHAT IT DOES NOT TEST: that the menu item and the shortcut are wired
// correctly. Those two are checked by hand on Windows.
////////////////////////////////////////////////////////////////////////////////

#define GO_QACLASS  "GraphOllyQA"
#define GO_QAMSG    "GraphOlly.QA.Trigger"

static UINT      goqamsg;
static HWND      goqawnd;

// Diagnostics: what Olly knows about the module under the selection, and where
// its procedures are. The same function serves automated QA and the
// "Diagnostics" menu item: one path only, so what the trace shows is exactly
// what the user sees.
void Godiag(void) {
  t_dump *pd;
  t_module *pm;
  ulong a,base,size,st,en;
  int n,analysed;
  pd=(t_dump *)Plugingetvalue(VAL_CPUDASM);
  a=(pd!=NULL ? pd->sel0 : 0);
  pm=Findmodule(a);
  Gotrace("DIAG: sel=%08lX module=%p",a,pm);
  if (pm==NULL) {
    Flash("GraphOlly: no known module at %08lX (trace written)",a);
    return;
  };
  Gotrace("DIAG: code %08lX+%lu codedec=%p jddata=%p njddata=%d processed=%d",
    pm->codebase,pm->codesize,pm->codedec,pm->jddata,pm->njddata,pm->processed);
  analysed=(pm->codedec!=NULL);
  base=pm->codebase;
  size=pm->codesize;
  a=base;
  for (n=0; n<10; n++) {
    a=Findnextproc(a);
    if (a==0 || a<base || a>=base+size)
      break;
    st=en=0;
    Gotrace("DIAG: procedure %d at %08lX (Getproclimits=%d %08lX-%08lX, begin=%08lX end=%08lX)",
      n,a,Getproclimits(a,&st,&en),st,en,Findprocbegin(a),Findprocend(a));
  };
  Gotrace("DIAG: procedures found: %d",n);
  Addtolist(0,0,"GraphOlly diagnostics: module %s, analysed=%s, procedures seen=%d",
    pm->name,(analysed ? "YES" : "NO"),n);
  if (analysed==0)
    Flash("GraphOlly: module NOT analysed - press Ctrl+A in the Disassembler");
  else
    Flash("GraphOlly: module analysed, %d procedures (details in the Log)",n);
};

static LRESULT CALLBACK Goqaproc(HWND hw,UINT msg,WPARAM wp,LPARAM lp) {
  t_dump *pd;
  ulong addr;
  HWND hwg;
  int x,y;
  if (msg==goqamsg && goqamsg!=0 && wp==0xFFFFFFFF) {
    Godiag();
    return 0;
  };
  if (msg==goqamsg && goqamsg!=0 && wp==0xFFFFFFFD) {
    // Sets a breakpoint from OUTSIDE the graph, as the user would from the
    // Disassembler: the graph window receives no message and must notice on
    // its own through its timer. Without this port the test would not be
    // possible (under wine the keyboard does not reach Olly).
    Gotrace("QA: external breakpoint at %08lX -> %d",(ulong)lp,
      Setbreakpoint((ulong)lp,TY_ACTIVE|TY_KEEPCODE,0));
    return 1;
  };
  if (msg==goqamsg && goqamsg!=0 && wp==0xFFFFFFFC) {
    // Resumes the debuggee. Used to test function following: run to a
    // breakpoint in ANOTHER function and the graph must switch by itself.
    // "Run UNTIL" that address: this way the test knows where EIP will end
    // up, instead of hoping the program passes a breakpoint.
    Gotrace("QA: Go(until %08lX) -> %d",(ulong)lp,Go(Getcputhreadid(),(ulong)lp,STEP_RUN,0,0));
    return 1;
  };
  if (msg==goqamsg && goqamsg!=0 && (wp==0xFFFFFFFB || wp==0xFFFFFFF9)) {
    // Search (FB) and export (F9) with the text in a global ATOM: across two
    // processes a pointer is worthless, an atom is not. The caller creates and
    // deletes it.
    char s[TEXTLEN];
    s[0]='\0';
    if (GlobalGetAtomName((ATOM)lp,s,sizeof(s))==0)
      return (wp==0xFFFFFFFB ? -2 : 0);
    if (wp==0xFFFFFFFB)
      return Goqafind(s,1);
    return Goqaexport(s);
  };
  if (msg==goqamsg && goqamsg!=0 && wp==0xFFFFFFFA) {
    // F3 / Shift+F3: lp=0 next, lp=1 previous. Shift cannot be posted
    // together with the key, which is why this goes through here.
    return Goqafind(NULL,(lp==1 ? -1 : 1));
  };
  if (msg==goqamsg && goqamsg!=0 && wp==0xFFFFFFF8) {
    // Where the minimap is: lp=0 top-left corner, lp=1 bottom-right,
    // +1 on each coordinate so that 0 still means "hidden".
    RECT r;
    if (Goqamaprect(&r)==0)
      return 0;
    if (lp==1)
      return (LRESULT)MAKELONG(r.right+1,r.bottom+1);
    return (LRESULT)MAKELONG(r.left+1,r.top+1);
  };
  if (msg==goqamsg && goqamsg!=0 && wp==0xFFFFFFF7) {
    // Exact zoom: lp is the percentage. Returns the zoom and the line height
    // in pixels (0 = overview, no text).
    int cell,z;
    z=Goqazoom((int)lp,&cell);
    return (LRESULT)MAKELONG(z,cell);
  };
  if (msg==goqamsg && goqamsg!=0 && wp==0xFFFFFFFE) {
    // Where is the first CALL line? Returns the position, not the action: the
    // caller then does the double click, which goes through the real code.
    hwg=NULL; x=y=0;
    if (Gofindcallline(&hwg,&x,&y)==0)
      return 0;                        // 0 = no visible CALL
    return (LRESULT)MAKELONG(x+1,y+1); // +1: so that 0 still means "not found"
  };
  if (msg==goqamsg && goqamsg!=0) {
    pd=(t_dump *)Plugingetvalue(VAL_CPUDASM);
    addr=(wp!=0 ? (ulong)wp : (pd!=NULL ? pd->sel0 : 0));
    Gotrace("QA: graph requested at %08lX (wp=%08lX)",addr,(ulong)wp);
    if (addr!=0)
      Gotrace("QA: Gocreatewindow -> %d",Gocreatewindow(addr));
    return 0;
  };
  return DefWindowProc(hw,msg,wp,lp);
};

static void Gocreateqawindow(void) {
  WNDCLASS wc;
  memset(&wc,0,sizeof(wc));
  wc.lpfnWndProc=Goqaproc;
  wc.hInstance=hinst;
  wc.lpszClassName=GO_QACLASS;
  if (RegisterClass(&wc)==0 && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) {
    Gotrace("QA: RegisterClass failed (%lu)",GetLastError());
    return;
  };
  goqamsg=RegisterWindowMessage(GO_QAMSG);
  goqawnd=CreateWindow(GO_QACLASS,GO_QACLASS,0,0,0,0,0,
    HWND_MESSAGE,NULL,hinst,NULL);
  Gotrace("QA: service window hw=%p msg=%u",goqawnd,goqamsg);
};

extc int _export cdecl ODBG_Plugindata(char shortname[32]) {
  Gotrace("ODBG_Plugindata: returning %d",PLUGIN_VERSION);
  strcpy(shortname,GO_NAME);
  return PLUGIN_VERSION;
};

extc int _export cdecl ODBG_Plugininit(int ollydbgversion,HWND hw,ulong *features) {
  int rc;
  // The ini is read here and not in DllMain: before Plugininit the PDK
  // functions are not safe to rely on.
  gotraceon=Pluginreadintfromini(hinst,"Trace",0);
  gofolloweip=Pluginreadintfromini(hinst,"FollowEIP",1);
  golabels   =Pluginreadintfromini(hinst,"Labels",0);
  gofollowfunc=Pluginreadintfromini(hinst,"FollowFunction",1);
  gominimap  =Pluginreadintfromini(hinst,"Minimap",1);
  {
    char sk[TEXTLEN];
    sk[0]='\0';
    Pluginreadstringfromini(hinst,"Shortcut",sk,"Ctrl+D");
    goshortshift=(strstr(sk,"Shift")!=NULL || strstr(sk,"shift")!=NULL);
    if (sk[0]!='\0') {
      char *p=sk+strlen(sk)-1;         // The last character is the key
      goshortkey=(*p>='a' && *p<='z' ? *p-'a'+'A' : *p);
    };
  };
  if (gotraceon>0) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    Gotrace("=== %s v%s - %04d-%02d-%02d %02d:%02d:%02d ===",
      GO_NAME,GO_VERSION,st.wYear,st.wMonth,st.wDay,st.wHour,st.wMinute,st.wSecond);
  };
  Gotrace("ODBG_Plugininit: version=%d hw=%p hinst=%p",ollydbgversion,hw,hinst);
  if (ollydbgversion<PLUGIN_VERSION) {
    Gotrace("  REJECTED: version too old");
    return -1;
  };
  hwmain=hw;
  // The window class formally belongs to the OllyDbg instance.
  rc=Registerpluginclass(gographclass,NULL,hinst,Gowinproc);
  Gotrace("  Registerpluginclass -> %d, class=\"%s\"",rc,gographclass);
  if (rc<0)
    return -1;
  Addtolist(0,0,"%s v%s - control-flow graph in OllyDbg style - %s",
    GO_NAME,GO_VERSION,GO_AUTHOR);
  Addtolist(0,-1,"  Disassembler: context menu, or %s",Goshortname());
  Addtolist(0,-1,"  In the graph: Enter follows, * back to EIP, F2 breakpoint, double click expands");
  Gocreateqawindow();
  Gotrace("  init done, returning 0");
  return 0;
};

// Olly calls this on every debug event. It is the only hook the 1.10 PDK
// offers to learn that the debuggee has stopped: from here the open graph
// windows realign to EIP.
extc void _export cdecl ODBG_Pluginmainloop(DEBUG_EVENT *debugevent) {
  if (debugevent==NULL)
    return;                            // Idle call from Olly's loop
  Gofolloweip();
};

// How the shortcut is written in menus, so it never disagrees with the one
// read from the ini.
static char *Goshortname(void) {
  static char s[32];
  sprintf(s,"Ctrl+%s%c",(goshortshift ? "Shift+" : ""),goshortkey);
  return s;
};

extc int _export cdecl ODBG_Pluginmenu(int origin,char data[4096],void *item) {
  Gotrace("menu: origin=%d",origin);
  switch (origin) {
    case PM_MAIN:
      sprintf(data,"0 &Graph current function\t%s|"
                   "3 %c Follow &EIP in graph|"
                   "4 %c Switch &function when execution enters it|"
                   "5 %c &Minimap in graph|"
                   "6 &Search in graph...\tCtrl+F|"
                   "7 E&xport graph as image...|"
                   "2 &Diagnostics|1 &About",
              Goshortname(),(gofolloweip ? '+' : '-'),(gofollowfunc ? '+' : '-'),
              (gominimap ? '+' : '-'));
      return 1;
    case PM_DISASM:
      sprintf(data,"0 &Graph function\t%s|"
                   "3 GraphOlly: %c follow &EIP|2 GraphOlly: &diagnostics",
              Goshortname(),(gofolloweip ? '+' : '-'));
      return 1;
    default:
      break;
  };
  return 0;
};

extc void _export cdecl ODBG_Pluginaction(int origin,int action,void *item) {
  t_dump *pd;
  ulong addr;
  Gotrace("action: origin=%d action=%d item=%p",origin,action,item);
  addr=0;
  if (origin==PM_DISASM && item!=NULL) {
    pd=(t_dump *)item;
    addr=pd->sel0;
  }
  else {
    pd=(t_dump *)Plugingetvalue(VAL_CPUDASM);
    if (pd!=NULL)
      addr=pd->sel0;
  };
  switch (action) {
    case 0:
      Gotrace("  chosen address: %08lX",addr);
      if (addr==0)
        Flash("GraphOlly: no address selected in the Disassembler");
      else
        Gotrace("  Gocreatewindow -> %d",Gocreatewindow(addr));
      break;
    case 1:
      {
      char about[2048];
      sprintf(about,
        GO_NAME " v" GO_VERSION "\n"
        GO_AUTHOR "\n\n"
        "The control flow drawn as a graph inside OllyDbg, with OllyDbg's\n"
        "fonts and colours.\n\n"
        "IN THE DISASSEMBLER\n"
        "  %s        opens the graph of the function\n\n"
        "IN THE GRAPH\n"
        "  Enter           follows the jump or call under the cursor\n"
        "  Backspace       goes back to where you were\n"
        "  *  (or Space)   back to the current instruction (EIP)\n"
        "  F2              sets or clears the breakpoint\n"
        "  Up/Down arrows  move the cursor; with Ctrl they scroll the view\n"
        "  + -  0  End     zoom, one-to-one, fit to window\n"
        "  Ctrl+wheel      zoom on the point under the mouse\n"
        "  double click    on a CALL expands the function; elsewhere moves\n"
        "                  the CPU window to that line\n"
        "  Ctrl+F          search (address, instruction, operand)\n"
        "  F3, Shift+F3    next, previous (also Ctrl+L); Esc clears\n"
        "  M               minimap: click or drag to move around\n"
        "  Shift+wheel     scrolls sideways\n"
        "  right button    everything else, including BMP export\n\n"
        "Green = jump taken, red = not taken; the false branch always\n"
        "leaves on the left and the true one on the right. Blue =\n"
        "unconditional jump or fall-through, dashed grey = expanded call.\n\n"
        "Diagnostic trace: [Plugin GraphOlly] Trace=1 in ollydbg.ini.",
        Goshortname());
      MessageBox(hwmain,about,GO_NAME,MB_OK|MB_ICONINFORMATION);
      };
      break;
    case 2:
      Godiag();
      break;
    case 5:
      Gocmdminimap();
      Flash("GraphOlly: minimap %s",(gominimap ? "ON" : "off"));
      break;
    case 6:
      Gocmdfind();
      break;
    case 7:
      Gocmdexport();
      break;
    case 4:
      gofollowfunc=(gofollowfunc ? 0 : 1);
      Pluginwriteinttoini(hinst,"FollowFunction",gofollowfunc);
      Flash("GraphOlly: automatic function switch %s",
        (gofollowfunc ? "ON" : "off"));
      break;
    case 3:
      gofolloweip=(gofolloweip ? 0 : 1);
      Pluginwriteinttoini(hinst,"FollowEIP",gofolloweip);
      Flash("GraphOlly: EIP following %s",(gofolloweip ? "ON" : "off"));
      Gofolloweip();
      break;
  };
};

extc int _export cdecl ODBG_Pluginshortcut(
  int origin,int ctrl,int alt,int shift,int key,void *item) {
  t_dump *pd;
  Gotrace("shortcut: origin=%d ctrl=%d alt=%d shift=%d key=%d(%c) item=%p",
    origin,ctrl,alt,shift,key,(key>=32 && key<127 ? key : '.'),item);
  // The key is the one read from the ini (default Ctrl+D). Every shortcut that
  // arrives ends up in the trace: if a combination does not show up there,
  // Olly swallowed it first and another one must be chosen.
  if (origin==PM_DISASM && ctrl!=0 && alt==0 && key==goshortkey &&
      (goshortshift==0 || shift!=0)) {
    pd=(t_dump *)item;
    if (pd!=NULL) {
      Gocreatewindow(pd->sel0);
      return 1;
    };
  };
  return 0;
};

extc void _export cdecl ODBG_Pluginreset(void) {
};

extc int _export cdecl ODBG_Pluginclose(void) {
  return 0;
};

extc void _export cdecl ODBG_Plugindestroy(void) {
  Unregisterpluginclass(gographclass);
};
