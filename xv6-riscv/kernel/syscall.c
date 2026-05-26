#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "syscall.h"
#include "defs.h"

// Fetch the uint64 at addr from the current process.
int
fetchaddr(uint64 addr, uint64 *ip)
{
  struct proc *p = myproc();
  if(addr >= p->sz || addr+sizeof(uint64) > p->sz) // both tests needed, in case of overflow
    return -1;
  if(copyin(p->pagetable, (char *)ip, addr, sizeof(*ip)) != 0)
    return -1;
  return 0;
}

// Fetch the nul-terminated string at addr from the current process.
// Returns length of string, not including nul, or -1 for error.
int
fetchstr(uint64 addr, char *buf, int max)
{
  struct proc *p = myproc();
  if(copyinstr(p->pagetable, buf, addr, max) < 0)
    return -1;
  return strlen(buf);
}

static uint64
argraw(int n)
{
  struct proc *p = myproc();
  switch (n) {
  case 0:
    return p->trapframe->a0;
  case 1:
    return p->trapframe->a1;
  case 2:
    return p->trapframe->a2;
  case 3:
    return p->trapframe->a3;
  case 4:
    return p->trapframe->a4;
  case 5:
    return p->trapframe->a5;
  }
  panic("argraw");
  return -1;
}

// Fetch the nth 32-bit system call argument.
void
argint(int n, int *ip)
{
  *ip = argraw(n);
}

// Retrieve an argument as a pointer.
// Doesn't check for legality, since
// copyin/copyout will do that.
void
argaddr(int n, uint64 *ip)
{
  *ip = argraw(n);
}

// Fetch the nth word-sized system call argument as a null-terminated string.
// Copies into buf, at most max.
// Returns string length if OK (including nul), -1 if error.
int
argstr(int n, char *buf, int max)
{
  uint64 addr;
  argaddr(n, &addr);
  return fetchstr(addr, buf, max);
}

// Prototypes for the functions that handle system calls.
extern uint64 sys_fork(void);
extern uint64 sys_exit(void);
extern uint64 sys_wait(void);
extern uint64 sys_pipe(void);
extern uint64 sys_read(void);
extern uint64 sys_kill(void);
extern uint64 sys_exec(void);
extern uint64 sys_fstat(void);
extern uint64 sys_chdir(void);
extern uint64 sys_dup(void);
extern uint64 sys_getpid(void);
extern uint64 sys_sbrk(void);
extern uint64 sys_pause(void);
extern uint64 sys_uptime(void);
extern uint64 sys_open(void);
extern uint64 sys_write(void);
extern uint64 sys_mknod(void);
extern uint64 sys_unlink(void);
extern uint64 sys_link(void);
extern uint64 sys_mkdir(void);
extern uint64 sys_close(void);
extern uint64 sys_setpriority(void);
extern uint64 sys_getpriority(void);
extern uint64 sys_jail(void);
extern uint64 sys_agent_recv(void);
extern uint64 sys_set_deny(void);
extern uint64 sys_get_deny(void);
extern uint64 sys_procinfo(void);
extern uint64 sys_set_cache(void);
extern uint64 sys_get_cache(void);
extern uint64 sys_dispatch(void);

// An array mapping syscall numbers from syscall.h
// to the function that handles the system call.
static uint64 (*syscalls[])(void) = {
[SYS_fork]    sys_fork,
[SYS_exit]    sys_exit,
[SYS_wait]    sys_wait,
[SYS_pipe]    sys_pipe,
[SYS_read]    sys_read,
[SYS_kill]    sys_kill,
[SYS_exec]    sys_exec,
[SYS_fstat]   sys_fstat,
[SYS_chdir]   sys_chdir,
[SYS_dup]     sys_dup,
[SYS_getpid]  sys_getpid,
[SYS_sbrk]    sys_sbrk,
[SYS_pause]   sys_pause,
[SYS_uptime]  sys_uptime,
[SYS_open]    sys_open,
[SYS_write]   sys_write,
[SYS_mknod]   sys_mknod,
[SYS_unlink]  sys_unlink,
[SYS_link]    sys_link,
[SYS_mkdir]   sys_mkdir,
[SYS_close]   sys_close,
[SYS_setpriority] sys_setpriority,
[SYS_getpriority] sys_getpriority,
[SYS_jail]    sys_jail,
[SYS_agent_recv] sys_agent_recv,
[SYS_set_deny] sys_set_deny,
[SYS_get_deny] sys_get_deny,
[SYS_procinfo] sys_procinfo,
[SYS_set_cache] sys_set_cache,
[SYS_get_cache] sys_get_cache,
[SYS_dispatch]  sys_dispatch,
};

// F7: system calls a sandboxed agent process may not invoke.
static int
agent_blocked(int num)
{
  return num == SYS_exec || num == SYS_kill || num == SYS_mknod;
}

// 차단된 syscall 의 호스트-가시 한 줄 요약.
// 보안 검토 단계에서 사람이 *무엇을* 허용하는지 알 수 있도록 인자 핵심만.
static void
agent_call_summary(int num, char *buf, int buflen)
{
  const char *name = "unknown";
  switch(num){
    case SYS_exec:  name = "exec";  break;
    case SYS_kill:  name = "kill";  break;
    case SYS_mknod: name = "mknod"; break;
  }
  // 인자 자세히는 syscall arg 디코딩이 필요 — v1 은 syscall 이름만.
  // (a0 인자 디코드는 argaddr/argstr 가 호출자 컨텍스트 종속이라 생략.)
  int i = 0;
  while(name[i] && i < buflen - 1){ buf[i] = name[i]; i++; }
  buf[i] = 0;
}

void
syscall(void)
{
  int num;
  struct proc *p = myproc();

  num = p->trapframe->a7;
  if(num > 0 && num < NELEM(syscalls) && syscalls[num]) {
    // F7 sandbox: jail 안 agent 가 위험 syscall 호출 시 무조건 거부.
    // (TODO: confirm-escape 메커니즘은 kerneltrap panic 의심으로 임시 비활성화.
    //  confirm.c / agentcmd CONFIRM_RES handler / agent.py _handle_confirm_req
    //  는 모듈로 남아 있으니 panic 원인 파악 후 다시 활성화 예정.)
    if(p->is_agent && agent_blocked(num)){
      printf("[sandbox] pid %d (%s): syscall %d blocked\n",
             p->pid, p->name, num);
      p->trapframe->a0 = -1;
      return;
    }
    (void)agent_call_summary;     // 위 분기에서 더 이상 안 부르지만 prototype 보존
    // Use num to lookup the system call function for num, call it,
    // and store its return value in p->trapframe->a0
    p->trapframe->a0 = syscalls[num]();
  } else {
    printf("%d %s: unknown sys call %d\n",
            p->pid, p->name, num);
    p->trapframe->a0 = -1;
  }
}
