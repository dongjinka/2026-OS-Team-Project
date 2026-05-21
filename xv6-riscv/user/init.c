// init: The initial user-level program

#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/spinlock.h"
#include "kernel/sleeplock.h"
#include "kernel/fs.h"
#include "kernel/file.h"
#include "user/user.h"
#include "kernel/fcntl.h"

char *argv[] = { "sh", 0 };

int
main(void)
{
  int pid, wpid;

  if(open("console", O_RDWR) < 0){
    mknod("console", CONSOLE, 0);
    open("console", O_RDWR);
  }
  dup(0);  // stdout
  dup(0);  // stderr

  // Apply any persisted F7 deny-list policy (/denylist.conf) to the kernel
  // before starting the agent runtime, so a saved configuration takes effect
  // at boot. denyctl load is a no-op if the file does not exist.
  pid = fork();
  if(pid == 0){
    char *dargv[] = { "denyctl", "load", 0 };
    exec("denyctl", dargv);
    printf("init: exec denyctl failed\n");
    exit(1);
  }
  wait((int *) 0);

  // Start the jailed LLM agent runtime. It confines itself via jail() and
  // then serves LLM-issued commands inside its sandbox for the system's
  // lifetime (it does not exit, so init's wait() loop never reaps it).
  pid = fork();
  if(pid == 0){
    char *aargv[] = { "agentd", 0 };
    exec("agentd", aargv);
    printf("init: exec agentd failed\n");
    exit(1);
  }

  for(;;){
    printf("init: starting sh\n");
    pid = fork();
    if(pid < 0){
      printf("init: fork failed\n");
      exit(1);
    }
    if(pid == 0){
      exec("sh", argv);
      printf("init: exec sh failed\n");
      exit(1);
    }

    for(;;){
      // this call to wait() returns if the shell exits,
      // or if a parentless process exits.
      wpid = wait((int *) 0);
      if(wpid == pid){
        // the shell exited; restart it.
        break;
      } else if(wpid < 0){
        printf("init: wait returned an error\n");
        exit(1);
      } else {
        // it was a parentless process; do nothing.
      }
    }
  }
}
