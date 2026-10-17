// Minimal target for the jump-arrow test: the entry point IS a handful of
// hand-written short jumps, so Olly stops exactly there and every jump has its
// destination on screen (without a visible destination Olly does not draw the
// connecting line). In C the compiler would use CMOV and there would be no
// jumps at all.
#include <windows.h>
void __attribute__((naked)) _start(void) {
  __asm__ __volatile__(
    "  xor  %eax, %eax        \n"
    "  cmp  $1, %eax          \n"
    "  je   2f                \n"   // short forward jump
    "  inc  %eax              \n"
    "  cmp  $5, %eax          \n"
    "  jne  1f                \n"   // another forward jump
    "  nop                    \n"
    "1:                       \n"
    "  add  $2, %eax          \n"
    "  cmp  $9, %eax          \n"
    "  jb   1b                \n"   // BACKWARD jump
    "2:                       \n"
    "  push %eax              \n"
    "  call *__imp__ExitProcess@4 \n"
  ); }
