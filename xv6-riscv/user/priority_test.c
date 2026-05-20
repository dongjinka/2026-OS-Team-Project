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
 * Fork 3 children with priorities HIGH(1), MED(10), LOW(19). Each child
 * burns identical CPU work then writes its label to a shared pipe just
 * before exit. The parent reads the pipe to recover the *actual* finish
 * order and asserts HIGH → MED → LOW.
 *
 * Why a pipe (not wait()): wait() returns "any exited child" and its
 * ordering is implementation-defined when multiple children have already
 * exited. A pipe gives a hard happens-before edge: the byte arrives in
 * the order the children's write() syscalls were serialized.
 */
static void
test_scheduling_order(void)
{
  printf("--- Test 3: High-priority process runs first ---\n");

  int p[2];
  if (pipe(p) < 0) {
    printf("FAIL: pipe() failed\n");
    exit(1);
  }

  /* Make burn work big enough that scheduling effects dominate startup
   * noise. Tuned by trial on smp=1; raise if Test 3 ever flakes. */
  const int WORK = 30000000;

  struct { const char *label; int prio; char tag; } kids[] = {
    { "LOW ",  19, 'L' },
    { "MED ",  10, 'M' },
    { "HIGH",   1, 'H' },
  };

  /* Spawn LOW first so HIGH genuinely has to "overtake". */
  for (int i = 0; i < 3; i++) {
    int pid = fork();
    if (pid < 0) {
      printf("FAIL: fork failed\n");
      exit(1);
    }
    if (pid == 0) {
      close(p[0]);
      setpriority(getpid(), kids[i].prio);
      burn(WORK);
      write(p[1], &kids[i].tag, 1);
      printf("[%s prio=%d] finished\n", kids[i].label, kids[i].prio);
      exit(0);
    }
  }

  close(p[1]);
  for (int i = 0; i < 3; i++) {
    int status;
    wait(&status);
  }

  char order[3] = {0};
  int got = 0;
  while (got < 3) {
    int n = read(p[0], order + got, 3 - got);
    if (n <= 0) break;
    got += n;
  }
  close(p[0]);

  if (got != 3) {
    printf("FAIL: pipe returned %d bytes (expected 3)\n", got);
    exit(1);
  }

  printf("Finish order: %c %c %c (expected: H M L)\n",
         order[0], order[1], order[2]);

  if (order[0] != 'H' || order[1] != 'M' || order[2] != 'L') {
    printf("FAIL: CFS did not respect priority\n");
    exit(1);
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
