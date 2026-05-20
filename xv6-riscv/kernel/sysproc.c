#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "vm.h"

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

  if(priority < 0 || priority > 20)
    return -1;

  struct proc *p;
  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
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

#define MAX_KEY_LEN 1024
#define MAX_VAL_LEN 256

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

  int n = (vlen < vbuflen) ? vlen : vbuflen;
  if(n > 0 && copyout(p->pagetable, vbuf_addr, val_buf, n) < 0) return -1;
  return vlen;
}

// Allow userspace to feed the agent dispatcher directly — same wire format
// the QEMU serial port accepts ("REQ|...\n" or "REQ|agent:<role>|...\n").
// Used by eval / agent_multi for in-kernel ACL & cache demos.
#define MAX_DISPATCH_LEN 1024

uint64
sys_dispatch(void)
{
  uint64 addr;
  argaddr(0, &addr);
  char buf[MAX_DISPATCH_LEN];
  struct proc *p = myproc();
  if(copyinstr(p->pagetable, buf, addr, MAX_DISPATCH_LEN) < 0) return -1;
  agent_dispatch_now(buf);
  return 0;
}
