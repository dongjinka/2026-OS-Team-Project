// Kernel side of the agent command path.
//
// LLM commands arrive over the QEMU serial port as lines of the form
//
//   REQ|<CMD>|<arg>\n
//
// console.c sniffs these lines and hands each one to agent_dispatch().
// IMPORTANT: agent_dispatch() runs in *interrupt context* and must not
// sleep, fork, or touch the file system. So the kernel does NOT execute
// the command here. Instead it:
//
//   1. drops outright-denied commands (F7 hard sandbox boundary), and
//   2. enqueues every other line into a small ring buffer.
//
// The jailed agent runtime process `agentd` (user/agentd.c) pulls lines
// from that ring buffer via the agent_recv() system call and executes
// them *inside its chroot jail* with dangerous syscalls blocked. Human
// shell input never travels this path — only LLM-issued commands do.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "deny.h"

#define AGENTQ_N    16     // ring buffer capacity (commands)
#define AGENTQ_LEN  256    // max length of one command line

struct {
  struct spinlock lock;
  char buf[AGENTQ_N][AGENTQ_LEN];
  int r;          // next slot to read
  int w;          // next slot to write
  int count;      // queued commands
} agentq;

// Commands the kernel refuses outright: they never reach the agent runtime
// (F7 — the hard sandbox boundary). No longer hardcoded — managed at runtime
// via set_deny()/get_deny() and the `denyctl` shell tool. Guarded by a
// spinlock so deny_listed() can read it from interrupt context (agent_dispatch)
// while denyctl mutates it from process context.
struct {
  struct spinlock lock;
  char names[DENY_MAX][DENY_NAMELEN];
  int count;
} denylist;

static const char *deny_default[] = { "KILL", "EXEC" };
#define NDENY_DEFAULT ((int)(sizeof(deny_default) / sizeof(deny_default[0])))

// Restore the built-in default deny list. Also used at boot.
void
deny_reset(void)
{
  acquire(&denylist.lock);
  denylist.count = 0;
  for(int i = 0; i < NDENY_DEFAULT && i < DENY_MAX; i++)
    safestrcpy(denylist.names[denylist.count++], deny_default[i], DENY_NAMELEN);
  release(&denylist.lock);
}

// Empty the deny list (nothing blocked at the kernel boundary).
void
deny_clear(void)
{
  acquire(&denylist.lock);
  denylist.count = 0;
  release(&denylist.lock);
}

// Add cmd to the deny list. Idempotent. Returns 0 on success, -1 if full.
int
deny_add(const char *cmd)
{
  acquire(&denylist.lock);
  for(int i = 0; i < denylist.count; i++){
    if(strncmp(denylist.names[i], cmd, DENY_NAMELEN) == 0){
      release(&denylist.lock);
      return 0;
    }
  }
  if(denylist.count >= DENY_MAX){
    release(&denylist.lock);
    return -1;
  }
  safestrcpy(denylist.names[denylist.count++], cmd, DENY_NAMELEN);
  release(&denylist.lock);
  return 0;
}

// Remove cmd from the deny list. Returns 0 if removed, -1 if not present.
int
deny_remove(const char *cmd)
{
  acquire(&denylist.lock);
  for(int i = 0; i < denylist.count; i++){
    if(strncmp(denylist.names[i], cmd, DENY_NAMELEN) == 0){
      for(int j = i; j < denylist.count - 1; j++)
        safestrcpy(denylist.names[j], denylist.names[j + 1], DENY_NAMELEN);
      denylist.count--;
      release(&denylist.lock);
      return 0;
    }
  }
  release(&denylist.lock);
  return -1;
}

// Copy the deny list into buf as newline-separated names. Returns byte length.
int
deny_snapshot(char *buf, int max)
{
  int n = 0;
  acquire(&denylist.lock);
  for(int i = 0; i < denylist.count; i++){
    const char *s = denylist.names[i];
    while(*s && n < max - 2)
      buf[n++] = *s++;
    if(n < max - 1)
      buf[n++] = '\n';
  }
  release(&denylist.lock);
  if(n < max)
    buf[n] = 0;
  return n;
}

void
agentcmd_init(void)
{
  initlock(&agentq.lock, "agentq");
  agentq.r = agentq.w = agentq.count = 0;
  initlock(&denylist.lock, "denylist");
  deny_reset();
}

// True if cmd is on the deny list. Runs in interrupt context (agent_dispatch);
// a spinlock is interrupt-safe (no sleep).
static int
deny_listed(const char *cmd)
{
  int found = 0;
  acquire(&denylist.lock);
  for(int i = 0; i < denylist.count; i++){
    if(strncmp(denylist.names[i], cmd, DENY_NAMELEN) == 0){
      found = 1;
      break;
    }
  }
  release(&denylist.lock);
  return found;
}

// Called from consoleintr() for each complete "REQ|" line. Interrupt
// context: spinlock + wakeup only, never sleeps.
void
agent_dispatch(char *line)
{
  if(!(line[0]=='R' && line[1]=='E' && line[2]=='Q' && line[3]=='|'))
    return;

  // extract the command name (between "REQ|" and the next '|')
  char cmd[24];
  int n = 0;
  char *p = line + 4;
  while(*p && *p != '|' && n < (int)sizeof(cmd) - 1)
    cmd[n++] = *p++;
  cmd[n] = 0;
  if(*p != '|'){
    printf("[agent] malformed: %s\n", line);
    return;
  }

  if(deny_listed(cmd)){
    printf("[agent] DENY '%s' (sandboxed: never reaches the agent)\n", cmd);
    return;
  }

  // enqueue the raw line for the jailed agentd to execute
  acquire(&agentq.lock);
  if(agentq.count == AGENTQ_N){
    release(&agentq.lock);
    printf("[agent] queue full, dropping '%s'\n", cmd);
    return;
  }
  int i = 0;
  while(line[i] && i < AGENTQ_LEN - 1){
    agentq.buf[agentq.w][i] = line[i];
    i++;
  }
  agentq.buf[agentq.w][i] = 0;
  agentq.w = (agentq.w + 1) % AGENTQ_N;
  agentq.count++;
  release(&agentq.lock);
  wakeup(&agentq);
}

// Blocking dequeue, called from sys_agent_recv() in process context.
// Copies the next command line into out (>= AGENTQ_LEN bytes). Returns
// the string length, or -1 if the caller was killed while waiting.
int
agentq_get(char *out)
{
  acquire(&agentq.lock);
  while(agentq.count == 0){
    if(killed(myproc())){
      release(&agentq.lock);
      return -1;
    }
    sleep(&agentq, &agentq.lock);
  }
  int i = 0;
  char *src = agentq.buf[agentq.r];
  while(src[i] && i < AGENTQ_LEN - 1){
    out[i] = src[i];
    i++;
  }
  out[i] = 0;
  agentq.r = (agentq.r + 1) % AGENTQ_N;
  agentq.count--;
  release(&agentq.lock);
  return i;
}
