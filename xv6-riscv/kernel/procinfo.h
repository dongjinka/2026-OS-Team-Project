#ifndef PROCINFO_H
#define PROCINFO_H

// One process's observable state, copied out by the procinfo() system call.
// Lets a user program (and through agentd, the LLM agent) see the process
// table — pid/state/priority/name — so it can reason about the system (e.g.
// pick a pid for NICE). `state` holds an `enum procstate` value (proc.h):
// 0=UNUSED 1=USED 2=SLEEPING 3=RUNNABLE 4=RUNNING 5=ZOMBIE.
struct procinfo {
  int  pid;
  int  state;
  int  priority;
  int  is_agent;     // 1 if a sandboxed agent process
  char name[16];     // matches struct proc.name
};

#endif
