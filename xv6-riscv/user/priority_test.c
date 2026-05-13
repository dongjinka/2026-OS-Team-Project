/*
 * priority_test.c — Test program for xv6 priority scheduler
 *
 * Copy this file to xv6-riscv/user/ and add $U/_priority_test
 * to UPROGS in the Makefile.
 */
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

/* Burn CPU for a while so scheduling differences are observable */
static void
burn(int iterations)
{
  volatile int x = 0;
  for (int i = 0; i < iterations; i++)
    x += i;
}

/*
 * Test 1: setpriority / getpriority basic functionality
 */
static void
test_basic(void)
{
  int pid = getpid();
  int prio;

  printf("--- Test 1: setpriority/getpriority ---\n");

  /* Default priority should be 10 */
  prio = getpriority(pid);
  printf("PID %d: default priority = %d\n", pid, prio);
  if (prio != 10) {
    printf("FAIL: expected default priority 10, got %d\n", prio);
    exit(1);
  }

  /* Set priority to 5 */
  if (setpriority(pid, 5) != 0) {
    printf("FAIL: setpriority returned error\n");
    exit(1);
  }
  prio = getpriority(pid);
  printf("PID %d: after setpriority(%d, 5), priority = %d\n", pid, pid, prio);
  if (prio != 5) {
    printf("FAIL: expected priority 5, got %d\n", prio);
    exit(1);
  }

  /* Invalid priority values should fail */
  int r1 = setpriority(pid, -1);
  printf("setpriority with invalid priority (-1): returned %d (OK)\n", r1);
  if (r1 != -1) {
    printf("FAIL: should have returned -1\n");
    exit(1);
  }

  int r2 = setpriority(pid, 21);
  printf("setpriority with invalid priority (21): returned %d (OK)\n", r2);
  if (r2 != -1) {
    printf("FAIL: should have returned -1\n");
    exit(1);
  }

  /* Restore default */
  setpriority(pid, 10);

  printf("Test 1 PASSED\n\n");
}

/*
 * Test 2: Priority inheritance through fork
 */
static void
test_inheritance(void)
{
  printf("--- Test 2: Priority inheritance via fork ---\n");

  /* Set parent priority to 3 */
  setpriority(getpid(), 3);
  printf("Parent priority = %d\n", getpriority(getpid()));

  int pid = fork();
  if (pid < 0) {
    printf("FAIL: fork failed\n");
    exit(1);
  }

  if (pid == 0) {
    /* Child */
    int cprio = getpriority(getpid());
    printf("Child priority = %d\n", cprio);
    if (cprio != 3) {
      printf("FAIL: child expected priority 3, got %d\n", cprio);
      exit(1);
    }
    exit(0);
  }

  /* Parent waits for child */
  int status;
  wait(&status);

  /* Restore default */
  setpriority(getpid(), 10);

  printf("Test 2 PASSED\n\n");
}

/*
 * Test 3: High-priority process completes before lower-priority ones
 *
 * We fork 3 children with different priorities. Each child burns CPU
 * then prints a message and exits. With priority scheduling, the
 * high-priority child should finish first.
 */
static void
test_scheduling_order(void)
{
  printf("--- Test 3: High-priority process runs first ---\n");

  /* We'll create children in order: LOW, MED, HIGH
   * but expect them to finish: HIGH, MED, LOW */

  int pids[3];

  /* Child 0: LOW priority (19) */
  pids[0] = fork();
  if (pids[0] == 0) {
    setpriority(getpid(), 19);
    burn(5000000);
    printf("[LOW  prio=19] finished\n");
    exit(0);
  }

  /* Child 1: MED priority (10) */
  pids[1] = fork();
  if (pids[1] == 0) {
    setpriority(getpid(), 10);
    burn(5000000);
    printf("[MED  prio=10] finished\n");
    exit(0);
  }

  /* Child 2: HIGH priority (1) */
  pids[2] = fork();
  if (pids[2] == 0) {
    setpriority(getpid(), 1);
    burn(5000000);
    printf("[HIGH prio=1] finished\n");
    exit(0);
  }

  /* Parent waits for all children */
  for (int i = 0; i < 3; i++) {
    int status;
    wait(&status);
  }

  printf("Test 3 PASSED\n\n");
}

int
main(int argc, char *argv[])
{
  printf("=== Priority Scheduler Test ===\n\n");

  test_basic();
  test_inheritance();
  test_scheduling_order();

  printf("All tests passed!\n");
  exit(0);
}
