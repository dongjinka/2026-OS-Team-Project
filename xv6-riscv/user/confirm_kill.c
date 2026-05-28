// confirm_kill.c — confirm-escape coverage for SYS_kill.
// jail(); kill(1); — should trigger CONFIRM_REQ for syscall 6 (SYS_kill).
// Host driver decides allow / deny; on deny the kill returns -1.
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(void)
{
  mkdir("/agentbox");
  if(jail("/agentbox") < 0){
    printf("FAIL: jail() failed\n");
    exit(1);
  }
  printf("[ck] jailed; attempting kill(99999)\n");
  // pid 99999 won't exist — but the syscall must still pass through
  // the confirm-escape gate first. We test the *gate*, not the kill itself.
  int rc = kill(99999);
  if(rc < 0)
    printf("[ck] OK   kill blocked or no-such-pid (rc=-1)\n");
  else
    printf("[ck] kill returned %d\n", rc);
  printf("=== ck done ===\n");
  exit(0);
}
