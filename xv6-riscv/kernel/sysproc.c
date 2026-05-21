#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "vm.h"
#include "deny.h"
#include "procinfo.h"

extern struct proc proc[NPROC];

uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  kexit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return kfork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return kwait(p);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int t;
  int n;

  argint(0, &n);
  argint(1, &t);
  addr = myproc()->sz;

  if(t == SBRK_EAGER || n < 0) {
    if(growproc(n) < 0) {
      return -1;
    }
  } else {
    // Lazily allocate memory for this process: increase its memory
    // size but don't allocate memory. If the processes uses the
    // memory, vmfault() will allocate it.
    if(addr + n < addr)
      return -1;
    if(addr + n > TRAPFRAME)
      return -1;
    myproc()->sz += n;
  }
  return addr;
}

uint64
sys_pause(void)
{
  int n;
  uint ticks0;

  argint(0, &n);
  if(n < 0)
    n = 0;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(killed(myproc())){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kkill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

uint64
sys_setpriority(void)
{
  int pid, priority;
  argint(0, &pid);
  argint(1, &priority);

  // F2: valid nice range is -20..20; negative values are kernel-class.
  if(priority < -20 || priority > 20)
    return -1;
  // F2/F7: a user-class process (priority >= 0) may not grant kernel-class
  // (negative) priority to anyone — that would be a privilege escalation.
  if(priority < 0 && myproc()->priority >= 0)
    return -1;

  // Snapshot the caller's class once, before taking any p->lock — so the
  // per-target check below never reads myproc()->priority while holding an
  // unrelated process's lock.
  int caller_kernel = (myproc()->priority < 0);

  struct proc *p;
  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      // F2/F7: a user-class caller may not modify a kernel-class process
      // (e.g. init). Otherwise the jailed agent could demote the supervisor
      // out of kernel-class via NICE. Only kernel-class callers may.
      if(p->priority < 0 && !caller_kernel){
        release(&p->lock);
        return -1;
      }
      p->priority = priority;
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

uint64
sys_getpriority(void)
{
  int pid;
  argint(0, &pid);

  struct proc *p;
  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      int prio = p->priority;
      release(&p->lock);
      return prio;
    }
    release(&p->lock);
  }
  return -1;
}

// agent_recv(buf): block until an LLM command line is available, then copy
// it into buf (caller must provide >= 256 bytes). Only the jailed agent
// runtime may pull commands. Returns the line length, -1 on error.
uint64
sys_agent_recv(void)
{
  uint64 uaddr;
  argaddr(0, &uaddr);

  struct proc *p = myproc();
  if(!p->is_agent)          // only a sandboxed agent process may receive
    return -1;

  char kbuf[256];           // == AGENTQ_LEN in agentcmd.c
  int n = agentq_get(kbuf);
  if(n < 0)
    return -1;
  if(copyout(p->pagetable, uaddr, kbuf, n + 1) < 0)
    return -1;
  return n;
}

// set_deny(op, cmd): mutate the F7 command deny list. Only non-agent (human-
// side) processes may call it, so a sandboxed agent cannot weaken its own
// boundary. op is DENY_ADD/DENY_REMOVE/DENY_RESET (kernel/deny.h).
uint64
sys_set_deny(void)
{
  int op;
  char cmd[DENY_NAMELEN];

  if(myproc()->is_agent)    // a jailed agent must not edit the deny list
    return -1;

  argint(0, &op);
  if(op == DENY_RESET){
    deny_reset();
    return 0;
  }
  if(op == DENY_CLEAR){
    deny_clear();
    return 0;
  }
  if(argstr(1, cmd, DENY_NAMELEN) < 0)
    return -1;
  if(op == DENY_ADD)
    return deny_add(cmd);
  if(op == DENY_REMOVE)
    return deny_remove(cmd);
  return -1;
}

// get_deny(buf, max): copy the deny list into buf as newline-separated names.
// Readable by anyone (no mutation), so agentd can render effective policy.
uint64
sys_get_deny(void)
{
  uint64 uaddr;
  int max;
  char kbuf[DENY_MAX * DENY_NAMELEN];

  argaddr(0, &uaddr);
  argint(1, &max);
  if(max <= 0)
    return -1;
  if(max > (int)sizeof(kbuf))
    max = sizeof(kbuf);

  int n = deny_snapshot(kbuf, max);
  if(copyout(myproc()->pagetable, uaddr, kbuf, n + 1) < 0)
    return -1;
  return n;
}

// procinfo(buf, max): copy up to `max` non-UNUSED process entries into the
// user array `buf`. Returns the count. Read-only observability: lets a user
// program (and the LLM agent via agentd PS) see pid/state/priority/name so it
// can reason about the system. Each proc's lock is held only long enough to
// snapshot one entry; copyout happens after release.
uint64
sys_procinfo(void)
{
  uint64 uaddr;
  int max;
  argaddr(0, &uaddr);
  argint(1, &max);
  if(max <= 0)
    return -1;

  struct proc *self = myproc();
  int n = 0;
  for(struct proc *p = proc; p < &proc[NPROC] && n < max; p++){
    struct procinfo pi;
    memset(&pi, 0, sizeof(pi));   // don't leak kernel stack via name[]'s tail
    acquire(&p->lock);
    if(p->state == UNUSED){
      release(&p->lock);
      continue;
    }
    pi.pid = p->pid;
    pi.state = p->state;
    pi.priority = p->priority;
    pi.is_agent = p->is_agent;
    safestrcpy(pi.name, p->name, sizeof(pi.name));
    release(&p->lock);

    if(copyout(self->pagetable, uaddr + (uint64)n * sizeof(pi),
               (char *)&pi, sizeof(pi)) < 0)
      return -1;
    n++;
  }
  return n;
}

// F9 — LLM response cache, ported from commit 76b2737 (Sejoong branch).
// Exposed here as syscalls SYS_set_cache (29) / SYS_get_cache (30); the cache
// body lives in kernel/cache.c.
#define MAX_KEY_LEN 1024
#define MAX_VAL_LEN 256   // userspace value cap; must stay <= CACHE_VAL in cache.c

uint64
sys_set_cache(void)
{
  uint64 key_addr, val_addr;
  int klen, vlen;
  argaddr(0, &key_addr);
  argint(1, &klen);
  argaddr(2, &val_addr);
  argint(3, &vlen);
  if(klen <= 0 || klen > MAX_KEY_LEN) return -1;
  if(vlen < 0 || vlen > MAX_VAL_LEN) return -1;

  char key_buf[MAX_KEY_LEN];
  char val_buf[MAX_VAL_LEN];
  struct proc *p = myproc();

  if(copyin(p->pagetable, key_buf, key_addr, klen) < 0) return -1;
  if(vlen > 0 && copyin(p->pagetable, val_buf, val_addr, vlen) < 0) return -1;

  return cache_set(key_buf, klen, val_buf, vlen);
}

uint64
sys_get_cache(void)
{
  uint64 key_addr, vbuf_addr;
  int klen, vbuflen;
  argaddr(0, &key_addr);
  argint(1, &klen);
  argaddr(2, &vbuf_addr);
  argint(3, &vbuflen);
  if(klen <= 0 || klen > MAX_KEY_LEN) return -1;
  if(vbuflen <= 0) return -1;

  char key_buf[MAX_KEY_LEN];
  char val_buf[MAX_VAL_LEN];
  struct proc *p = myproc();

  if(copyin(p->pagetable, key_buf, key_addr, klen) < 0) return -1;

  int vlen = cache_get(key_buf, klen, val_buf, MAX_VAL_LEN);
  if(vlen < 0) return -1;

  // cache_get filled at most MAX_VAL_LEN bytes of val_buf; never copy out past
  // that, regardless of the returned vlen or the caller-supplied vbuflen.
  int avail = (vlen < MAX_VAL_LEN) ? vlen : MAX_VAL_LEN;
  int n = (avail < vbuflen) ? avail : vbuflen;
  if(n > 0 && copyout(p->pagetable, vbuf_addr, val_buf, n) < 0) return -1;
  return vlen;
}
