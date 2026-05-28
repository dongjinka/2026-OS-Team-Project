// confirm_mknod.c — confirm-escape coverage for SYS_mknod.
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
  printf("[cm] jailed; attempting mknod\n");
  int rc = mknod("/dev_test", 1, 1);
  if(rc < 0)
    printf("[cm] OK   mknod blocked (rc=-1)\n");
  else
    printf("[cm] mknod returned %d\n", rc);
  printf("=== cm done ===\n");
  exit(0);
}
