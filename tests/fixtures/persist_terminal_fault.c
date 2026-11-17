/* Signed QEMU fixture only. Reuse the production Persist loop and dispatcher. */
#include "libsf.h"
#include "persist_server.h"
#include "../../services/persist_terminal.h"

#ifndef TERM_FAULT_MODE
#define TERM_FAULT_MODE 1
#endif
#if TERM_FAULT_MODE < 1 || TERM_FAULT_MODE > 3
#error TERM_FAULT_MODE must be 1 (drop ACK), 2 (delay exit), or 3 (force busy)
#endif

static long terminal_fault_send(uint32_t,SfMessage*);

static int terminal_fault_quiescent(void){
#if TERM_FAULT_MODE == 3
 static int reported;
 if(!reported){sf_log("[PERSIST-TERMINAL-FAULT] forced busy cache\n");reported=1;}
 return SF_BUSY;
#else
 return persist_quiescent();
#endif
}

/* Build a private copy of the production terminal parser/loop with only the
 * cached quiescence result redirected for mode 3. Keep the production copy,
 * linked by the regular source list, under its original symbol names. */
#define persist_terminal_dispatch terminal_fault_dispatch
#define persist_serve terminal_fault_serve
#define persist_serve_with_idle terminal_fault_serve_with_idle
#define persist_quiescent terminal_fault_quiescent
#include "../../services/persist_terminal.c"
#undef persist_quiescent
#undef persist_serve
#undef persist_serve_with_idle
#undef persist_terminal_dispatch

/* Include the production Persist service entry point, routing all sends
 * through a narrow test callback while retaining its ordinary dispatcher. */
#define persist_serve terminal_fault_serve
#define persist_serve_with_idle terminal_fault_serve_with_idle
#define sf_send terminal_fault_send
#include "../../services/persist.c"
#undef sf_send
#undef persist_serve
#undef persist_serve_with_idle

static long terminal_fault_send(uint32_t ep,SfMessage*m){
 static int ack_fault_done;
 if(m->h.opcode==OP_PERSIST_STOP&&m->status==0&&m->mode==1&&!ack_fault_done){
#if TERM_FAULT_MODE == 1
  ack_fault_done=1;
  sf_log("[PERSIST-TERMINAL-FAULT] accepted ACK dropped\n");
  return SF_TIMEOUT;
#elif TERM_FAULT_MODE == 2
  ack_fault_done=1;
  long r=sf_send(ep,m);
  if(!r){
   sf_log("[PERSIST-TERMINAL-FAULT] accepted ACK delayed exit\n");
   for(unsigned i=0;i<8;i++)sf_yield();
  }
  return r;
#endif
 }
 return sf_send(ep,m);
}
