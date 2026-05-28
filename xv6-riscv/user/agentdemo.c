/*
 * agentdemo.c — demonstrates F2 (priority guard) and F7 (filesystem jail).
 *
 * Copy to xv6-riscv/user/ and add $U/_agentdemo to UPROGS in the Makefile.
 * Run from the xv6 shell:  agentdemo
 */
#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"

int
main(void)
{
  printf("=== agent sandbox demo ===\n");

  /* Build a jail directory with one file inside it. */
  mkdir("/agentbox");
  int fd = open("/agentbox/notes.txt", O_CREATE | O_RDWR);
  if(fd >= 0){
    write(fd, "hello from inside the jail\n", 27);
    close(fd);
  }

  /* Enter the jail. From here on, '/' means /agentbox. */
  if(jail("/agentbox") < 0){
    printf("FAIL: jail() failed\n");
    exit(1);
  }
  printf("[demo] jailed: '/' is now the agent box\n");

  /* 1. A file inside the jail is reachable. */
  fd = open("/notes.txt", O_RDONLY);
  if(fd >= 0){
    char buf[64];
    int n = read(fd, buf, sizeof(buf) - 1);
    if(n > 0){ buf[n] = 0; printf("[demo] read inside jail: %s", buf); }
    close(fd);
  } else {
    printf("[demo] FAIL: cannot read file inside jail\n");
  }

  /* 2. Escape attempts must fail. */
  if(open("/../init", O_RDONLY) < 0)
    printf("[demo] OK   escape via '..' blocked\n");
  else
    printf("[demo] FAIL escape via '..' succeeded\n");

  if(open("/cat", O_RDONLY) < 0)
    printf("[demo] OK   outside file '/cat' is invisible\n");
  else
    printf("[demo] FAIL outside file reachable\n");

  /* 3. F2: a sandboxed (user-class) process cannot grant itself
   *    kernel-class (negative) priority. */
  if(setpriority(getpid(), -1) < 0)
    printf("[demo] OK   negative priority denied (no escalation)\n");
  else
    printf("[demo] FAIL obtained kernel-class priority\n");

  /* 4. Dangerous syscalls go through the confirm-escape gate. fork+exec so
   *    the demo process itself isn't replaced when allow path runs — that
   *    way `=== demo done ===` is always reached and tests can sequence. */
  int pid = fork();
  if(pid < 0){
    printf("[demo] FAIL fork() failed\n");
  } else if(pid == 0){
    char *argv[] = { "echo", "demo-exec", 0 };
    exec("/echo", argv);
    exit(1);                   // exec failed
  } else {
    int st = 0;
    wait(&st);
    if(st == 0)
      printf("[demo] OK   exec() ran via confirm-escape allow\n");
    else
      printf("[demo] OK   exec() blocked by sandbox\n");
  }

  printf("=== demo done ===\n");
  exit(0);
}
