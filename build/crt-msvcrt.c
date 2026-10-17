// Minimal startup for the test executables, linked against msvcrt.dll.
//
// Current MinGW builds against the UCRT (api-ms-win-crt-*.dll), which an
// unpatched Windows 7 or XP does not have: the executable does not even start.
// msvcrt.dll has been there since Windows 95. The toolchain's startup files
// are made for the UCRT, so we link with -nostartfiles and enter here.
#include <stdlib.h>

typedef struct { int newmode; } _startupinfo;
int __cdecl __getmainargs(int *argc,char ***argv,char ***env,int glob,
  _startupinfo *si);
int main(int argc,char *argv[]);

// libgcc (__main) asks for it to register global destructors. C has none,
// and the _onexit in MinGW's msvcrt library is itself a wrapper around
// atexit: forwarding this call to it loops until the stack overflows.
int __cdecl atexit(void (__cdecl *f)(void)) {
  (void)f;
  return 0;
}

void mainCRTStartup(void) {
  int argc;
  char **argv,**env;
  _startupinfo si={0};
  __getmainargs(&argc,&argv,&env,0,&si);
  exit(main(argc,argv));
}
