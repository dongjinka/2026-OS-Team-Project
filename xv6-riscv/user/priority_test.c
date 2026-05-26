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
 * Test 3: High-priority processes finish earlier on average than low ones.
 *
 * SMP-친화적 디자인: 자식 수(KIDS) 를 CPU 수(3) 보다 많게 두어 *강제 경쟁* 을
 * 만든다. 자식 3 명이면 3-CPU 환경에서 각자 CPU 한 개씩 차지해 경쟁이 사라져
 * 우선순위 효과가 측정 불가. 6 자식 (HIGH×2 / MED×2 / LOW×2) 으로 늘리면
 * 항상 일부 자식이 runqueue 대기 → CFS 의 vruntime 가중치가 종료 순서로 드러남.
 *
 * 검증: 종료 순서를 6 byte 의 pipe 로 받아, 각 priority 그룹의 평균 종료
 * 인덱스를 계산. avg(H) < avg(M) < avg(L) 이면 CFS 가 우선순위를 존중한 것.
 * 정수 비교를 위해 곱셈으로 회피 (avg = sum/n → sum1*n2 < sum2*n1).
 */
#define KIDS 6
static void
test_scheduling_order(void)
{
  printf("--- Test 3: High-priority processes finish earlier on avg ---\n");

  int p[2];
  if (pipe(p) < 0) {
    printf("FAIL: pipe() failed\n");
    exit(1);
  }

  const int WORK = 30000000;

  struct { const char *label; int prio; char tag; } kids[KIDS] = {
    { "LOW0",  19, 'L' }, { "LOW1",  19, 'L' },
    { "MED0",  10, 'M' }, { "MED1",  10, 'M' },
    { "HI0 ",   1, 'H' }, { "HI1 ",   1, 'H' },
  };

  /* LOW 부터 spawn — HIGH 가 "추월"해야 통과되도록. */
  for (int i = 0; i < KIDS; i++) {
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
      exit(0);
    }
  }

  close(p[1]);
  for (int i = 0; i < KIDS; i++) {
    int status;
    wait(&status);
  }

  char order[KIDS] = {0};
  int got = 0;
  while (got < KIDS) {
    int n = read(p[0], order + got, KIDS - got);
    if (n <= 0) break;
    got += n;
  }
  close(p[0]);

  if (got != KIDS) {
    printf("FAIL: pipe returned %d bytes (expected %d)\n", got, KIDS);
    exit(1);
  }

  printf("Finish order: ");
  for (int i = 0; i < KIDS; i++) printf("%c ", order[i]);
  printf("(expected: H 들이 앞쪽, L 들이 뒤쪽)\n");

  /* 그룹별 평균 종료 인덱스 (정수로 sum/count). */
  int h_sum = 0, h_n = 0, m_sum = 0, m_n = 0, l_sum = 0, l_n = 0;
  for (int i = 0; i < KIDS; i++) {
    if      (order[i] == 'H') { h_sum += i; h_n++; }
    else if (order[i] == 'M') { m_sum += i; m_n++; }
    else if (order[i] == 'L') { l_sum += i; l_n++; }
  }
  if (h_n == 0 || m_n == 0 || l_n == 0) {
    printf("FAIL: 그룹별 종료 카운트 누락 (H=%d M=%d L=%d)\n", h_n, m_n, l_n);
    exit(1);
  }
  /* avg_H < avg_M < avg_L  ⇔  h_sum*m_n < m_sum*h_n  &&  m_sum*l_n < l_sum*m_n */
  if (!(h_sum * m_n < m_sum * h_n && m_sum * l_n < l_sum * m_n)) {
    printf("FAIL: CFS did not respect priority "
           "(avg idx — H=%d/%d, M=%d/%d, L=%d/%d)\n",
           h_sum, h_n, m_sum, m_n, l_sum, l_n);
    exit(1);
  }

  printf("Test 3 PASSED (avg idx H<M<L)\n\n");
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
