// Kernel-side dispatcher for agent commands sent over the QEMU serial port
// (TCP 4444 in the qemu-agent target). The line protocol is:
//
//   REQ|<CMD>|<arg>\n
//
// Lines are sniffed and accumulated in console.c; complete lines are passed
// here. This file does the parsing and the call into kernel functions.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

extern struct proc proc[NPROC];

static int
str_eq(const char *a, const char *b)
{
  while(*a && *b){ if(*a != *b) return 0; a++; b++; }
  return *a == *b;
}

static int
parse_uint(const char *s, int *out)
{
  int v = 0, any = 0;
  while(*s >= '0' && *s <= '9'){ v = v*10 + (*s - '0'); any = 1; s++; }
  if(!any) return -1;
  *out = v;
  return (*s == 0) ? 0 : -1;
}

void
agent_dispatch(char *line)
{
  // line is null-terminated, no trailing newline. Verify REQ| prefix.
  if(!(line[0]=='R' && line[1]=='E' && line[2]=='Q' && line[3]=='|')) return;
  char *cmd = line + 4;

  // split at the first '|' after cmd
  char *bar = cmd;
  while(*bar && *bar != '|') bar++;
  if(*bar != '|'){ printf("[agent] malformed: %s\n", line); return; }
  *bar = 0;
  char *arg = bar + 1;

  if(str_eq(cmd, "PRINT")){
    printf("[agent] %s\n", arg);
    return;
  }

  if(str_eq(cmd, "KILL")){
    int pid;
    if(parse_uint(arg, &pid) < 0){ printf("[agent] bad KILL pid: %s\n", arg); return; }
    if(pid <= 2){
      printf("[agent] DENY kill pid=%d (init/sh protected)\n", pid);
      return;
    }
    if(kkill(pid) == 0) printf("[agent] killed pid=%d\n", pid);
    else                printf("[agent] no such pid=%d\n", pid);
    return;
  }

  if(str_eq(cmd, "NICE")){
    // arg = "<pid>:<prio>"
    char *colon = arg;
    while(*colon && *colon != ':') colon++;
    if(*colon != ':'){ printf("[agent] bad NICE arg: %s\n", arg); return; }
    *colon = 0;
    int pid, prio;
    if(parse_uint(arg, &pid) < 0 || parse_uint(colon+1, &prio) < 0){
      printf("[agent] bad NICE numbers\n"); return;
    }
    if(prio < 0 || prio > 20){ printf("[agent] bad prio %d\n", prio); return; }

    struct proc *p;
    for(p = proc; p < &proc[NPROC]; p++){
      acquire(&p->lock);
      if(p->pid == pid){
        p->priority = prio;
        release(&p->lock);
        printf("[agent] pid=%d prio=%d\n", pid, prio);
        return;
      }
      release(&p->lock);
    }
    printf("[agent] no such pid=%d\n", pid);
    return;
  }

  printf("[agent] unknown cmd '%s'\n", cmd);
}
