/*
 * cfs_share.c — Quantitative CFS fairness benchmark.
 *
 * Spawns N children with different priorities, lets them all race a
 * fixed wall-clock budget, and reports each child's loop count plus the
 * implied CPU-share ratio. This lets us verify F3/F4 numerically (not
 * just by eye-balling finish order in Test 3).
 *
 * Recommended invocation: smp=1 (so cores don't trivially absorb every
 * runnable child). Try `make qemu CPUS=1` and run `cfs_share`.
 *
 * Output example:
 *   prio=1 count=4123456 share=63%
 *   prio=10 count=1320120 share=20%
 *   prio=19 count=991111 share=15%
 */
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define DURATION_TICKS 200          // wall-clock window per run (10ms tick @ xv6)
#define NKIDS          3            // number of priority levels we race

/* Priorities we want to compare. Edit freely. Mixing kernel-class
 * (negative) values requires running this binary itself as kernel-class,
 * which user shells cannot do — so we stick to user range here. */
static const int prios[NKIDS] = { 1, 10, 19 };

int
main(int argc, char *argv[])
{
  int p[2];
  if(pipe(p) < 0){
    printf("cfs_share: pipe failed\n");
    exit(1);
  }

  printf("cfs_share: racing %d children for %d ticks (smp=1 recommended)\n",
         NKIDS, DURATION_TICKS);

  for(int i = 0; i < NKIDS; i++){
    int pid = fork();
    if(pid < 0){
      printf("cfs_share: fork failed\n");
      exit(1);
    }
    if(pid == 0){
      close(p[0]);
      setpriority(getpid(), prios[i]);

      /* Spin until our budget expires. Each child reads its own start
       * tick after fork — they're a few ticks apart at most, which is
       * negligible vs DURATION_TICKS. */
      uint start = uptime();
      uint deadline = start + DURATION_TICKS;
      volatile uint count = 0;
      while(uptime() < deadline){
        for(int k = 0; k < 1000; k++)
          count++;
      }

      /* Report back as "prio:count\n"; the parent slurps the whole pipe. */
      fprintf(p[1], "%d:%u\n", prios[i], count);
      exit(0);
    }
  }

  close(p[1]);

  /* Wait for all children before reading so we can size the report by
   * total iterations (for the share-% column). This is only safe while
   * NKIDS * max_line_bytes (~14) stays under PIPESIZE (512): otherwise a
   * writer would block on a full pipe while we block in wait() -> deadlock.
   * If you bump NKIDS past ~30, read concurrently with wait() instead. */
  for(int i = 0; i < NKIDS; i++){
    int st;
    wait(&st);
  }

  char all[512];
  int total = 0;
  int got = 0;
  int n;
  while((n = read(p[0], all + got, sizeof(all) - 1 - got)) > 0)
    got += n;
  close(p[0]);
  all[got] = 0;

  /* First pass: sum totals so we can compute %. */
  struct { int prio; uint count; } rows[NKIDS];
  int nrows = 0;
  char *s = all;
  while(*s && nrows < NKIDS){
    int neg = 0;
    if(*s == '-'){ neg = 1; s++; }
    int prio = 0;
    while(*s >= '0' && *s <= '9'){ prio = prio*10 + (*s - '0'); s++; }
    if(neg) prio = -prio;
    if(*s != ':') break;
    s++;
    uint count = 0;
    while(*s >= '0' && *s <= '9'){ count = count*10 + (*s - '0'); s++; }
    if(*s == '\n') s++;
    rows[nrows].prio = prio;
    rows[nrows].count = count;
    total += count;
    nrows++;
  }

  if(nrows != NKIDS)
    printf("cfs_share: warning: parsed %d of %d rows\n", nrows, NKIDS);

  printf("\ncfs_share results:\n");
  for(int i = 0; i < nrows; i++){
    int pct = total ? (int)((uint64)rows[i].count * 100 / total) : 0;
    printf("  prio=%d count=%u share=%d%%\n",
           rows[i].prio, rows[i].count, pct);
  }
  printf("  (total=%d iterations)\n", total);

  exit(0);
}
