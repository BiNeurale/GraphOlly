////////////////////////////////////////////////////////////////////////////////
// ollyqa.exe - drives and inspects OllyDbg from inside wine (or a Windows VM).
//
// Why it exists: under headless gamescope, XTEST events never reach wine
// windows and every GDI screen capture comes back black. So this tool uses the
// Windows input queue (keybd_event, posted messages) and reads the real state
// of the UI (menus, child windows) instead of guessing it from a screenshot.
// The graph-specific commands talk to the plugin's hidden QA window
// ("GraphOllyQA", message "GraphOlly.QA.Trigger"), see src/grapholly.c.
//
// Olly itself:
//   alive                    exit code 0 if the main window is there
//   menu                     items of the Plugins menu
//   menuids                  whole menu tree with ids (Olly's menu is
//                            owner-drawn: the texts are empty, the ids are not)
//   cmd <id>                 post WM_COMMAND with that id (runs a menu item)
//   windows                  MDI child windows (class + title)
//   top                      top-level windows (popups included)
//   modules                  non-system DLLs loaded in the OllyDbg process
//   geom                     main window and client geometry
//   key <combo>              send a key combo through the input queue (ctrl+g)
//   postkey <vk-hex>         post a virtual key to the main window (78 = F9);
//                            needs no foreground, works on real Windows too
//   topmost 0|1              make Olly topmost (1) or normal again (0)
//   maximize                 maximize the graph window inside Olly
//   click <class>            real click in the middle of the child of that class
//   clickat x y              real click at client x,y of the main window
//   clickdeep x y            posted click to the innermost window at screen x,y
//   clickcls <class> x y     posted click to the pane of that class, its coords
//   post x y                 posted click at main-window client x,y (forwarded
//                            to the child underneath)
//   postclient x y           posted click to the main window itself (toolbar)
//   postw x y                posted click, x,y relative to the window frame
//                            (the same coordinates as a screenshot)
//
// The graph window:
//   graph [addr]             open the graph at addr (hex), or at the selection
//   graphs                   how many graph windows are open
//   expand                   double-click the first visible CALL line (inline)
//   followcall               put the cursor on a CALL line and press Enter
//   graphkey <name>          key to the graph: end home star space enter
//                            backspace down right f2 f3 esc, or a letter/digit
//   graphclick x y           click in the graph (selects block and line)
//   graphhover x y           mouse move over the graph (hover highlight)
//   graphwheel in|out <n>    n steps of Ctrl+wheel
//   zoom <percent>           exact zoom; reports text line height or overview
//   redraw <n>               force 2n scrolls (the plugin traces paint cost)
//   find <text>              search like Ctrl+F (the text travels in an atom)
//   findnext [back]          F3 / Shift+F3
//   export <file.bmp>        export the whole graph, no save dialog
//   minimap                  where the minimap is (graph client coordinates)
//   minimapclick <px> <py>   posted click in the minimap, in percent of its
//                            width and height (goes through the real handler)
//   extbp <addr>             set a breakpoint from outside the graph window
//   run <addr>               run the debuggee until addr
////////////////////////////////////////////////////////////////////////////////

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <psapi.h>
#include <stdlib.h>

static HWND ollymain;
static HWND found;
static char wanted[256];

static BOOL CALLBACK Findolly(HWND hw,LPARAM lp) {
  char title[512];
  if (!IsWindowVisible(hw))
    return TRUE;
  GetWindowText(hw,title,sizeof(title));
  if (strncmp(title,"OllyDbg",7)==0) {
    ollymain=hw;
    return FALSE;
  };
  return TRUE;
};

static BOOL CALLBACK Findbyclass(HWND hw,LPARAM lp) {
  char cls[256];
  GetClassName(hw,cls,sizeof(cls));
  if (strcmp(cls,wanted)==0) {
    found=hw;
    return FALSE;
  };
  return TRUE;
};

static int  ngraphs;
static HWND graphwnd;

// A graph window is recognised by its title: "Graph: ...".
static BOOL CALLBACK Countgraph(HWND hw,LPARAM lp) {
  char title[512];
  GetWindowText(hw,title,sizeof(title));
  if (strncmp(title,"Graph:",6)==0) {
    ngraphs++;
    graphwnd=hw;
  };
  return TRUE;
};

// The last graph window found, or NULL (and a message).
static HWND Findgraph(void) {
  ngraphs=0; graphwnd=NULL;
  EnumChildWindows(ollymain,Countgraph,0);
  if (graphwnd==NULL)
    printf("NO GRAPH WINDOW\n");
  return graphwnd;
};

// The plugin's QA window, or NULL (and a message).
static HWND Findqa(UINT *msg) {
  HWND qa;
  qa=FindWindowEx(HWND_MESSAGE,NULL,"GraphOllyQA",NULL);
  if (qa==NULL)
    qa=FindWindow("GraphOllyQA",NULL);
  if (qa==NULL) {
    printf("QA WINDOW NOT FOUND: the plugin is not loaded\n");
    return NULL;
  };
  *msg=RegisterWindowMessage("GraphOlly.QA.Trigger");
  return qa;
};

static BOOL CALLBACK Dumpchild(HWND hw,LPARAM lp) {
  char title[512],cls[256];
  RECT rc;
  GetWindowText(hw,title,sizeof(title));
  GetClassName(hw,cls,sizeof(cls));
  GetWindowRect(hw,&rc);
  printf("  window: class=%-24s title=\"%s\" %ldx%ld visible=%d\n",
    cls,title,rc.right-rc.left,rc.bottom-rc.top,IsWindowVisible(hw));
  return TRUE;
};

static void Dumpmenu(HMENU menu,int depth) {
  int i,n;
  char text[256];
  HMENU sub;
  n=GetMenuItemCount(menu);
  for (i=0; i<n; i++) {
    text[0]='\0';
    GetMenuString(menu,i,text,sizeof(text),MF_BYPOSITION);
    printf("%*s[%d] id=%d \"%s\"\n",depth*2+2,"",i,
      GetMenuItemID(menu,i),text);
    sub=GetSubMenu(menu,i);
    if (sub!=NULL && depth<2)
      Dumpmenu(sub,depth+1);
  };
};

// "ctrl+g" -> press ctrl, press g, release. Only the modifiers named.
static void Sendkey(const char *combo) {
  int ctrl,alt,shift;
  char key;
  const char *p;
  ctrl=(strstr(combo,"ctrl")!=NULL);
  alt=(strstr(combo,"alt")!=NULL);
  shift=(strstr(combo,"shift")!=NULL);
  p=strrchr(combo,'+');
  key=(p!=NULL ? p[1] : combo[0]);
  if (key>='a' && key<='z')
    key=key-'a'+'A';
  if (ctrl)  keybd_event(VK_CONTROL,0,0,0);
  if (alt)   keybd_event(VK_MENU,0,0,0);
  if (shift) keybd_event(VK_SHIFT,0,0,0);
  keybd_event(key,0,0,0);
  Sleep(60);
  keybd_event(key,0,KEYEVENTF_KEYUP,0);
  if (shift) keybd_event(VK_SHIFT,0,KEYEVENTF_KEYUP,0);
  if (alt)   keybd_event(VK_MENU,0,KEYEVENTF_KEYUP,0);
  if (ctrl)  keybd_event(VK_CONTROL,0,KEYEVENTF_KEYUP,0);
};

static void Postclick(HWND hw,int x,int y) {
  PostMessage(hw,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(x,y));
  Sleep(80);
  PostMessage(hw,WM_LBUTTONUP,0,MAKELPARAM(x,y));
};

int main(int argc,char *argv[]) {
  HMENU menu,plugins;
  int i,n;
  char text[256];

  if (argc<2) {
    printf("usage: ollyqa.exe <command> [args] - the command list is at the top of build/ollyqa.c\n");
    return 2;
  };

  ollymain=NULL;
  EnumWindows(Findolly,0);
  if (ollymain==NULL) {
    printf("OLLYDBG NOT FOUND\n");
    return 3;
  };
  GetWindowText(ollymain,text,sizeof(text));
  printf("main window: \"%s\"\n",text);

  if (strcmp(argv[1],"alive")==0)
    return 0;

  if (strcmp(argv[1],"menu")==0) {
    menu=GetMenu(ollymain);
    if (menu==NULL) {
      printf("NO MENU\n");
      return 4;
    };
    plugins=NULL;
    n=GetMenuItemCount(menu);
    for (i=0; i<n; i++) {
      text[0]='\0';
      GetMenuString(menu,i,text,sizeof(text),MF_BYPOSITION);
      if (strstr(text,"Plugin")!=NULL || strstr(text,"plugin")!=NULL)
        plugins=GetSubMenu(menu,i);
    };
    if (plugins==NULL) {
      printf("NO PLUGINS MENU\n");
      return 5;
    };
    printf("Plugins menu:\n");
    Dumpmenu(plugins,0);
    return 0;
  };

  if (strcmp(argv[1],"clickat")==0 && argc>=4) {
    POINT pt;
    pt.x=atoi(argv[2]);
    pt.y=atoi(argv[3]);
    ClientToScreen(ollymain,&pt);
    SetForegroundWindow(ollymain);
    Sleep(200);
    SetCursorPos(pt.x,pt.y);
    Sleep(120);
    mouse_event(MOUSEEVENTF_LEFTDOWN,0,0,0,0);
    Sleep(80);
    mouse_event(MOUSEEVENTF_LEFTUP,0,0,0,0);
    printf("clicked at screen %ld,%ld\n",pt.x,pt.y);
    return 0;
  };

  if (strcmp(argv[1],"clickdeep")==0 && argc>=4) {
    // Click posted to the INNERMOST window under that screen point.
    // ChildWindowFromPoint stops at the first level (the MDI client), where
    // the message selects nothing: the CPU panes are its grandchildren.
    POINT pt;
    HWND target;
    char cls[128];
    pt.x=atoi(argv[2]);
    pt.y=atoi(argv[3]);
    target=WindowFromPoint(pt);
    if (target==NULL) { printf("no window at %ld,%ld\n",pt.x,pt.y); return 9; };
    GetClassName(target,cls,sizeof(cls));
    ScreenToClient(target,&pt);
    Postclick(target,pt.x,pt.y);
    printf("click posted at %ld,%ld of \"%s\" (%p)\n",pt.x,pt.y,cls,target);
    return 0;
  };

  if (strcmp(argv[1],"clickcls")==0 && argc>=5) {
    // Click posted to the Olly pane with that class, in ITS coordinates.
    // Screen coordinates are no good: they change when the window is
    // maximised, and WindowFromPoint can pick the wine console in front.
    // The class is stable (ACPUASM = the Disassembler).
    HWND target;
    int x,y;
    strncpy(wanted,argv[2],sizeof(wanted)-1);
    found=NULL;
    EnumChildWindows(ollymain,Findbyclass,0);
    target=found;
    if (target==NULL) { printf("no pane of class \"%s\"\n",argv[2]); return 9; };
    x=atoi(argv[3]); y=atoi(argv[4]);
    Postclick(target,x,y);
    printf("click posted at %d,%d of pane \"%s\" (%p)\n",x,y,argv[2],target);
    return 0;
  };

  if (strcmp(argv[1],"graph")==0) {
    HWND qa;
    UINT msg;
    unsigned long addr;
    if ((qa=Findqa(&msg))==NULL) return 9;
    addr=(argc>=3 ? strtoul(argv[2],NULL,16) : 0);
    SendMessageTimeout(qa,msg,(WPARAM)addr,0,SMTO_NORMAL,20000,NULL);
    printf("graph requested (addr=%08lX) from QA window %p, msg=%u\n",addr,qa,msg);
    return 0;
  };

  if (strcmp(argv[1],"expand")==0) {
    // Inline the first called function visible in the graph. The QA window
    // only says WHERE the CALL line is: the double-click is posted from here,
    // so the test goes through the real Gohittest and Goexpand.
    HWND qa;
    UINT msg;
    DWORD_PTR res=0;
    int x,y;
    if ((qa=Findqa(&msg))==NULL) return 9;
    SendMessageTimeout(qa,msg,0xFFFFFFFE,0,SMTO_NORMAL,20000,&res);
    if (res==0) { printf("NO CALL LINE VISIBLE IN THE GRAPH\n"); return 9; };
    x=LOWORD(res)-1; y=HIWORD(res)-1;
    if (Findgraph()==NULL) return 9;
    PostMessage(graphwnd,WM_LBUTTONDBLCLK,0,MAKELPARAM(x,y));
    Sleep(1500);
    printf("double-click posted at client %d,%d of graph window %p\n",x,y,graphwnd);
    return 0;
  };

  if ((strcmp(argv[1],"find")==0 || strcmp(argv[1],"export")==0) && argc>=3) {
    // Text cannot cross processes as a pointer: it goes into a global atom,
    // which the plugin reads and we delete.
    HWND qa;
    UINT msg;
    DWORD_PTR res=0;
    ATOM at;
    int find;
    find=(strcmp(argv[1],"find")==0);
    if ((qa=Findqa(&msg))==NULL) return 9;
    at=GlobalAddAtom(argv[2]);
    if (at==0) { printf("ATOM NOT CREATED\n"); return 9; };
    SendMessageTimeout(qa,msg,(find ? 0xFFFFFFFB : 0xFFFFFFF9),(LPARAM)at,
      SMTO_NORMAL,30000,&res);
    GlobalDeleteAtom(at);
    if (find)
      printf("find \"%s\": %d lines found\n",argv[2],(int)res);
    else
      printf("export \"%s\": %s (zoom %d%%)\n",argv[2],
        ((int)res>0 ? "done" : "FAILED"),(int)res);
    return ((int)res>0 ? 0 : 1);
  };

  if (strcmp(argv[1],"findnext")==0) {
    HWND qa;
    UINT msg;
    DWORD_PTR res=0;
    int back;
    if ((qa=Findqa(&msg))==NULL) return 9;
    back=(argc>=3 && strcmp(argv[2],"back")==0);
    SendMessageTimeout(qa,msg,0xFFFFFFFA,(back ? 1 : 0),SMTO_NORMAL,20000,&res);
    printf("find next (%s): %d lines found\n",(back ? "back" : "forward"),(int)res);
    return 0;
  };

  if (strcmp(argv[1],"minimap")==0 || (strcmp(argv[1],"minimapclick")==0 && argc>=4)) {
    // Where the minimap is. The click is posted from here to the graph
    // window, so it goes through the real WM_LBUTTONDOWN.
    HWND qa;
    UINT msg;
    DWORD_PTR a=0,b=0;
    int l,t,r,bb,x,y;
    if ((qa=Findqa(&msg))==NULL) return 9;
    SendMessageTimeout(qa,msg,0xFFFFFFF8,0,SMTO_NORMAL,20000,&a);
    SendMessageTimeout(qa,msg,0xFFFFFFF8,1,SMTO_NORMAL,20000,&b);
    if (a==0 || b==0) { printf("MINIMAP HIDDEN\n"); return 1; };
    l=LOWORD(a)-1; t=HIWORD(a)-1; r=LOWORD(b)-1; bb=HIWORD(b)-1;
    printf("minimap: %d,%d - %d,%d\n",l,t,r,bb);
    if (strcmp(argv[1],"minimap")==0)
      return 0;
    if (Findgraph()==NULL) return 9;
    x=l+(r-l-1)*atoi(argv[2])/100;
    y=t+(bb-t-1)*atoi(argv[3])/100;
    Postclick(graphwnd,x,y);
    Sleep(400);
    printf("click in the minimap at client %d,%d\n",x,y);
    return 0;
  };

  if (strcmp(argv[1],"zoom")==0 && argc>=3) {
    // Exact zoom, in percent. Reports whether text is drawn and how tall a
    // line is: checking the readability limit needs exact zoom levels, not
    // wheel steps.
    HWND qa;
    UINT msg;
    DWORD_PTR res=0;
    if ((qa=Findqa(&msg))==NULL) return 9;
    SendMessageTimeout(qa,msg,0xFFFFFFF7,(LPARAM)atoi(argv[2]),SMTO_NORMAL,20000,&res);
    if (LOWORD(res)==0) { printf("NO GRAPH WINDOW\n"); return 9; };
    if (HIWORD(res)==0)
      printf("zoom %d%%: overview (addresses only)\n",(int)LOWORD(res));
    else
      printf("zoom %d%%: text, line height %d px\n",(int)LOWORD(res),(int)HIWORD(res));
    return 0;
  };

  if (strcmp(argv[1],"graphwheel")==0 && argc>=4) {
    // Ctrl+wheel on the graph window: that is the zoom. The delta is in
    // multiples of 120, as Windows sends it. The direction is a word, not a
    // sign: "-6" as an argument is a minefield between shell, wine and atoi,
    // and a run that does not zoom won't tell whether the plugin or the
    // probe got it wrong.
    int k,steps,out;
    POINT pt;
    if (Findgraph()==NULL) return 9;
    out=(strcmp(argv[2],"out")==0);
    steps=atoi(argv[3]);
    if (steps<0) steps=-steps;
    pt.x=200; pt.y=150;
    ClientToScreen(graphwnd,&pt);
    for (k=0; k<steps; k++) {
      PostMessage(graphwnd,WM_MOUSEWHEEL,
        MAKEWPARAM(MK_CONTROL,(out ? (WORD)-120 : 120)),MAKELPARAM(pt.x,pt.y));
      Sleep(60);
    };
    Sleep(600);
    printf("%d Ctrl+wheel steps (%s) to graph window %p\n",
      steps,(out ? "out" : "in"),graphwnd);
    return 0;
  };

  if (strcmp(argv[1],"graphkey")==0 && argc>=3) {
    // One key to the graph window.
    int vk;
    if (Findgraph()==NULL) return 9;
    if      (strcmp(argv[2],"end")==0)       vk=VK_END;
    else if (strcmp(argv[2],"home")==0)      vk=VK_HOME;
    else if (strcmp(argv[2],"star")==0)      vk=VK_MULTIPLY;
    else if (strcmp(argv[2],"space")==0)     vk=VK_SPACE;
    else if (strcmp(argv[2],"enter")==0)     vk=VK_RETURN;
    else if (strcmp(argv[2],"backspace")==0) vk=VK_BACK;
    else if (strcmp(argv[2],"down")==0)      vk=VK_DOWN;
    else if (strcmp(argv[2],"right")==0)     vk=VK_RIGHT;
    else if (strcmp(argv[2],"f2")==0)        vk=VK_F2;
    else if (strcmp(argv[2],"f3")==0)        vk=VK_F3;
    else if (strcmp(argv[2],"esc")==0)       vk=VK_ESCAPE;
    // A letter is its UPPERCASE code: lowercase "m" (109) would be
    // VK_SUBTRACT, i.e. zoom out.
    else if (argv[2][0]>='a' && argv[2][0]<='z') vk=argv[2][0]-'a'+'A';
    else vk=argv[2][0];
    PostMessage(graphwnd,WM_KEYDOWN,vk,1);
    Sleep(60);
    PostMessage(graphwnd,WM_KEYUP,vk,0xC0000001);
    Sleep(600);
    printf("key %s (vk=%d) to graph window %p\n",argv[2],vk,graphwnd);
    return 0;
  };

  if (strcmp(argv[1],"graphclick")==0 && argc>=4) {
    // Click on the graph window: selects the block AND the line.
    int x,y;
    if (Findgraph()==NULL) return 9;
    x=atoi(argv[2]); y=atoi(argv[3]);
    Postclick(graphwnd,x,y);
    Sleep(600);
    printf("click at %d,%d on graph window %p\n",x,y,graphwnd);
    return 0;
  };

  if (strcmp(argv[1],"graphhover")==0 && argc>=4) {
    // Mouse resting over a point of the graph window: tests the highlight
    // of the block under the cursor.
    if (Findgraph()==NULL) return 9;
    PostMessage(graphwnd,WM_MOUSEMOVE,0,
      MAKELPARAM(atoi(argv[2]),atoi(argv[3])));
    Sleep(600);
    printf("mouse at %s,%s on graph window %p\n",argv[2],argv[3],graphwnd);
    return 0;
  };

  if (strcmp(argv[1],"extbp")==0 && argc>=3) {
    // Breakpoint set from OUTSIDE the graph window: the window gets no
    // message at all and has to notice through its timer.
    HWND qa; UINT msg; DWORD_PTR res=0;
    if ((qa=Findqa(&msg))==NULL) return 9;
    SendMessageTimeout(qa,msg,0xFFFFFFFD,
      (LPARAM)strtoul(argv[2],NULL,16),SMTO_NORMAL,20000,&res);
    printf("external breakpoint at %s requested (result %d)\n",argv[2],(int)res);
    return 0;
  };

  if (strcmp(argv[1],"run")==0 && argc>=3) {
    HWND qa; UINT msg; DWORD_PTR res=0;
    if ((qa=Findqa(&msg))==NULL) return 9;
    SendMessageTimeout(qa,msg,0xFFFFFFFC,
      (LPARAM)strtoul(argv[2],NULL,16),SMTO_NORMAL,20000,&res);
    printf("run until %s requested (result %d)\n",argv[2],(int)res);
    return 0;
  };

  if (strcmp(argv[1],"followcall")==0) {
    // Put the cursor on a CALL line (the QA window says where it is), then
    // press Enter: "follow" gets tested on a line that really has something
    // to follow, not on a random MOV.
    HWND qa;
    UINT msg;
    DWORD_PTR res=0;
    int x,y;
    if ((qa=Findqa(&msg))==NULL) return 9;
    SendMessageTimeout(qa,msg,0xFFFFFFFE,0,SMTO_NORMAL,20000,&res);
    if (res==0) { printf("NO CALL LINE VISIBLE\n"); return 9; };
    x=LOWORD(res)-1; y=HIWORD(res)-1;
    if (Findgraph()==NULL) return 9;
    Postclick(graphwnd,x,y);
    Sleep(400);
    printf("cursor on the CALL line at client %d,%d\n",x,y);
    PostMessage(graphwnd,WM_KEYDOWN,VK_RETURN,1);
    Sleep(80);
    PostMessage(graphwnd,WM_KEYUP,VK_RETURN,0xC0000001);
    Sleep(1500);
    printf("Enter posted to graph window %p\n",graphwnd);
    return 0;
  };

  if (strcmp(argv[1],"graphs")==0) {
    // How many graph windows are open. Tests REUSE: asking twice for the
    // graph of the same function must leave only one.
    ngraphs=0;
    EnumChildWindows(ollymain,Countgraph,0);
    printf("graph windows open: %d\n",ngraphs);
    return 0;
  };

  if (strcmp(argv[1],"redraw")==0 && argc>=3) {
    // Force N redraws by scrolling back and forth: the plugin measures its
    // own paint cost and writes it to the trace every 50 paints.
    int k,times;
    if (Findgraph()==NULL) return 9;
    times=atoi(argv[2]);
    for (k=0; k<times; k++) {
      PostMessage(graphwnd,WM_VSCROLL,(k&1 ? SB_LINEDOWN : SB_LINEUP),0);
      PostMessage(graphwnd,WM_HSCROLL,(k&1 ? SB_LINEDOWN : SB_LINEUP),0);
      Sleep(15);                       // Without a pause the WM_PAINTs merge
    };                                 // and the paint count is wrong
    Sleep(2000);
    printf("requested %d scrolls from graph window %p\n",times*2,graphwnd);
    return 0;
  };

  if (strcmp(argv[1],"geom")==0) {
    RECT wr,cr;
    POINT org;
    GetWindowRect(ollymain,&wr);
    GetClientRect(ollymain,&cr);
    org.x=0; org.y=0;
    ClientToScreen(ollymain,&org);
    printf("window on screen: %ld,%ld - %ld,%ld\n",wr.left,wr.top,wr.right,wr.bottom);
    printf("client: %ldx%ld, origin on screen %ld,%ld\n",
      cr.right,cr.bottom,org.x,org.y);
    printf("window->client offset: %ld,%ld\n",org.x-wr.left,org.y-wr.top);
    return 0;
  };

  if (strcmp(argv[1],"postclient")==0 && argc>=4) {
    // Click posted TO THE MAIN WINDOW in its client coordinates: that is
    // where Olly draws and handles the toolbar (it is not a child).
    int px,py;
    px=atoi(argv[2]);
    py=atoi(argv[3]);
    Postclick(ollymain,px,py);
    printf("click posted at client %d,%d of the main window\n",px,py);
    return 0;
  };

  if (strcmp(argv[1],"postw")==0 && argc>=4) {
    // Coordinates as in the screenshot (relative to the window FRAME):
    // converted to client coordinates of the child underneath.
    RECT wr;
    POINT pt,org;
    HWND target;
    char cls[128];
    GetWindowRect(ollymain,&wr);
    org.x=0; org.y=0;
    ClientToScreen(ollymain,&org);
    pt.x=atoi(argv[2])+wr.left-org.x;  // frame -> client of the main window
    pt.y=atoi(argv[3])+wr.top -org.y;
    target=ChildWindowFromPoint(ollymain,pt);
    if (target==NULL)
      target=ollymain;
    if (target!=ollymain) {
      POINT sp=pt;
      ClientToScreen(ollymain,&sp);
      ScreenToClient(target,&sp);
      pt=sp;
    };
    GetClassName(target,cls,sizeof(cls));
    Postclick(target,pt.x,pt.y);
    printf("click posted at client %ld,%ld of %s\n",pt.x,pt.y,cls);
    return 0;
  };

  if (strcmp(argv[1],"post")==0 && argc>=4) {
    int px,py;
    HWND target;
    POINT pt;
    char cls[128];
    px=atoi(argv[2]);
    py=atoi(argv[3]);
    pt.x=px; pt.y=py;
    // If there is a child window under that point, the message goes to it.
    target=ChildWindowFromPoint(ollymain,pt);
    if (target==NULL || target==ollymain)
      target=ollymain;
    else {
      POINT sp;
      sp.x=px; sp.y=py;
      ClientToScreen(ollymain,&sp);
      ScreenToClient(target,&sp);
      px=sp.x; py=sp.y;
    };
    Postclick(target,px,py);
    GetClassName(target,cls,sizeof(cls));
    printf("click posted at %d,%d of class %s\n",px,py,cls);
    return 0;
  };

  if (strcmp(argv[1],"top")==0) {
    printf("top-level windows:\n");
    EnumWindows(Dumpchild,0);
    return 0;
  };

  if (strcmp(argv[1],"menuids")==0) {
    menu=GetMenu(ollymain);
    if (menu==NULL) {
      printf("NO MENU\n");
      return 4;
    };
    printf("menu tree:\n");
    Dumpmenu(menu,0);
    return 0;
  };

  if (strcmp(argv[1],"cmd")==0 && argc>=3) {
    int id=atoi(argv[2]);
    PostMessage(ollymain,WM_COMMAND,(WPARAM)id,0);
    printf("WM_COMMAND id=%d sent\n",id);
    return 0;
  };

  if (strcmp(argv[1],"modules")==0) {
    // A check that depends neither on pixels nor on menus: the plugin DLL
    // must be mapped into the OllyDbg process.
    DWORD pid,needed,k;
    HANDLE hproc;
    HMODULE mods[512];
    char path[MAX_PATH];
    GetWindowThreadProcessId(ollymain,&pid);
    hproc=OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ,FALSE,pid);
    if (hproc==NULL) {
      printf("OPENPROCESS FAILED (pid %lu)\n",pid);
      return 6;
    };
    if (EnumProcessModules(hproc,mods,sizeof(mods),&needed)==0) {
      printf("ENUMPROCESSMODULES FAILED\n");
      return 7;
    };
    printf("modules loaded in OllyDbg (pid %lu):\n",pid);
    for (k=0; k<needed/sizeof(HMODULE); k++) {
      if (GetModuleFileNameEx(hproc,mods[k],path,sizeof(path))>0) {
        if (strstr(path,"\\windows\\")!=NULL || strstr(path,"\\Windows\\")!=NULL)
          continue;                    // Skip system DLLs
        printf("  %s\n",path);
      };
    };
    CloseHandle(hproc);
    return 0;
  };

  if (strcmp(argv[1],"windows")==0) {
    printf("child windows:\n");
    EnumChildWindows(ollymain,Dumpchild,0);
    return 0;
  };

  if (strcmp(argv[1],"click")==0 && argc>=3) {
    // A real click: Olly's shortcuts depend on WHO has the focus.
    RECT rc;
    strncpy(wanted,argv[2],sizeof(wanted)-1);
    found=NULL;
    EnumChildWindows(ollymain,Findbyclass,0);
    if (found==NULL) {
      printf("CLASS NOT FOUND: %s\n",argv[2]);
      return 8;
    };
    GetWindowRect(found,&rc);
    SetForegroundWindow(ollymain);
    Sleep(200);
    SetCursorPos((rc.left+rc.right)/2,(rc.top+rc.bottom)/2);
    Sleep(100);
    mouse_event(MOUSEEVENTF_LEFTDOWN,0,0,0,0);
    Sleep(60);
    mouse_event(MOUSEEVENTF_LEFTUP,0,0,0,0);
    printf("clicked on %s at %ld,%ld\n",argv[2],
      (rc.left+rc.right)/2,(rc.top+rc.bottom)/2);
    return 0;
  };

  if (strcmp(argv[1],"maximize")==0) {
    HWND gr=Findgraph();
    if (gr==NULL) return 4;
    SendMessage(GetParent(gr),WM_MDIMAXIMIZE,(WPARAM)gr,0);
    printf("graph window maximized\n");
    return 0;
  };

  if (strcmp(argv[1],"topmost")==0 && argc>=3) {
    // Olly above every other window (1) or normal again (0). Windows does not
    // let another process bring it to the foreground; TOPMOST it allows.
    SetWindowPos(ollymain,atoi(argv[2]) ? HWND_TOPMOST : HWND_NOTOPMOST,
      0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
    printf("topmost: %s\n",argv[2]);
    return 0;
  };

  if (strcmp(argv[1],"postkey")==0 && argc>=3) {
    // Virtual key (hex) posted to the main window: it goes through Olly's
    // message loop and so through its accelerators, without needing the
    // foreground. On real Windows this is how F9 (78) reaches a window that
    // cannot be brought to the front.
    UINT vk=(UINT)strtoul(argv[2],NULL,16);
    PostMessage(ollymain,WM_KEYDOWN,vk,1);
    Sleep(60);
    PostMessage(ollymain,WM_KEYUP,vk,0xC0000001);
    printf("key %02X posted\n",vk);
    return 0;
  };

  if (strcmp(argv[1],"key")==0 && argc>=3) {
    SetForegroundWindow(ollymain);
    Sleep(300);
    Sendkey(argv[2]);
    printf("sent: %s\n",argv[2]);
    return 0;
  };

  printf("unknown command\n");
  return 2;
};
