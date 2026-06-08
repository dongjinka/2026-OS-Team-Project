// secnice.c — red-team reproducer for SECURITY_AUDIT finding #3.
//
// Finding: sys_setpriority() (kernel/sysproc.c) guards the kernel-class
// boundary but never restricts a jailed agent (is_agent=1, user-class) to
// reniceing only ITSELF. A sandboxed agent can therefore change the priority
// of ANY user-class process — the human shell, init's children, or a peer
// agent — i.e. a scheduling-level denial of service from inside the jail.
//
// This program does NOT modify any kernel code. It only exercises existing
// syscalls and reports what the kernel allows:
//   setpriority(victim, X) == 0  -> RESULT=VULNERABLE (current main)
//   setpriority(victim, X) == -1 -> RESULT=SAFE        (after fix #3)
//
// Driven head-less by tools/sec_audit.py; also runnable by hand: `secnice`.
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

// Keep the victim alive (no sleep() syscall in this xv6 variant) long enough
// for the parent's quick attack. The victim only needs to EXIST in the proc
// table at attack time; a fork'd child is RUNNABLE immediately, so even a
// short spin is sufficient.
static void
stay_alive(void)
{
  volatile long i;
  for(i = 0; i < 40000000L; i++)
    ;
}

int
main(void)
{
  mkdir("/agentbox");                 // jail target (no-op if it already exists)

  int victim = fork();
  if(victim < 0){
    printf("SECNICE: fork failed\n");
    exit(1);
  }
  if(victim == 0){
    stay_alive();                     // child: remain a live, targetable pid
    exit(0);
  }

  // Parent = the would-be attacker. Become a sandboxed agent, then try to
  // renice a process that is NOT us.
  int before = getpriority(victim);
  if(jail("/agentbox") < 0){
    printf("SECNICE: jail() failed\n");
    exit(1);
  }
  int rc    = setpriority(victim, 19);   // attack: starve a non-self process
  int after = getpriority(victim);

  printf("SECNICE: jailed setpriority(victim=%d, 19) rc=%d  prio %d -> %d\n",
         victim, rc, before, after);
  // Disambiguate the two reasons setpriority can return -1: the F7 guard
  // (victim still live, priority unchanged) vs. the victim having already
  // exited (getpriority now -1). Only treat an unchanged, *still-live* victim
  // as SAFE, and only a confirmed reprioritization as VULNERABLE; a vanished
  // victim is a harness race, not a security verdict — report it as such so
  // the run is re-tried rather than scored as a false SAFE.
  if(rc == 0 && after == 19)
    printf("SECNICE: RESULT=VULNERABLE (jailed agent reniced a non-self process)\n");
  else if(rc < 0 && after >= 0)
    printf("SECNICE: RESULT=SAFE (jailed agent restricted to self)\n");
  else
    printf("SECNICE: RESULT=INCONCLUSIVE (victim pid=%d not live at attack time; re-run)\n",
           victim);

  wait(0);                            // reap the victim (wait is not gated)
  exit(0);
}
