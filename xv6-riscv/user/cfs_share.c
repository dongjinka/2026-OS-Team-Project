/*
 * cfs_share.c — CFS priority -> CPU-share benchmark.
 *
 * Spawns N children at different priorities, races them for a fixed
 * wall-clock window, and reports each child's loop count (the CPU it won)
 * as BOTH a human "share %" table and machine-parseable CFSBENCH lines that
 * tools/bench_report.py turns into the report table/curve. This unifies the
 * former cfs_share (fixed 3-priority race) and cfs_bench (full user-range
 * sweep) into one program — they were otherwise the same fork/spin/pipe race.
 *
 * Usage:
 *   cfs_share                       # default: 3 kids {1,10,19}, 200 ticks
 *   cfs_share <ticks> <prio> ...    # sweep arbitrary user-class priorities
 *     e.g. cfs_share 150 0 4 8 12 16 19
 *
 * Recommended invocation: smp=1 (so cores don't trivially absorb every
 * runnable child). Try `make qemu CPUS=1` and run `cfs_share`, or let
 * tools/bench_report.py drive it head-less.
 *
 * Output:
 *   CFSBENCH start nkids=6 ticks=150
 *   CFSBENCH prio=0 count=12345678
 *   ...
 *   CFSBENCH DONE
 *   cfs_share results:
 *     prio=0 count=12345678 share=42%
 *   (total=... iterations)
 *
 * NOTE: only non-negative (user-class) priorities — a user-class process may
 * not grant itself kernel-class (negative) priority, so negatives are rejected.
 */
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define MAXKIDS        16           // pipe-deadlock-safe ceiling (see wait note)
#define DEF_TICKS      200          // wall-clock window per run (10ms tick @ xv6)

// Default race when invoked with no args (preserves the original cfs_share).
static const int default_prios[] = { 1, 10, 19 };
#define DEF_NKIDS ((int)(sizeof(default_prios)/sizeof(default_prios[0])))

int
main(int argc, char *argv[])
{
  int prios[MAXKIDS];
  int nkids, ticks;

  if(argc <= 1){
    nkids = DEF_NKIDS;
    ticks = DEF_TICKS;
    for(int i = 0; i < nkids; i++)
      prios[i] = default_prios[i];
  } else {
    ticks = atoi(argv[1]);
    if(ticks <= 0)
      ticks = DEF_TICKS;
    nkids = 0;
    for(int i = 2; i < argc && nkids < MAXKIDS; i++){
      int pr = atoi(argv[i]);
      if(pr < 0){                   // user-class only; negatives would be denied
        printf("cfs_share: skipping kernel-class priority %d\n", pr);
        continue;
      }
      prios[nkids++] = pr;
    }
    if(nkids == 0){
      printf("cfs_share: usage: cfs_share <ticks> <prio>...\n");
      exit(1);
    }
  }

  int p[2];
  if(pipe(p) < 0){
    printf("cfs_share: pipe failed\n");
    exit(1);
  }

  printf("cfs_share: racing %d children for %d ticks (smp=1 recommended)\n",
         nkids, ticks);
  printf("CFSBENCH start nkids=%d ticks=%d\n", nkids, ticks);

  for(int i = 0; i < nkids; i++){
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
       * negligible vs the window. */
      uint start = uptime();
      uint deadline = start + ticks;
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
   * nkids * max_line_bytes (~14) stays under PIPESIZE (512): MAXKIDS=16
   * keeps us well under. If you raise MAXKIDS past ~30, read concurrently
   * with wait() instead, or a writer blocks on a full pipe -> deadlock. */
  for(int i = 0; i < nkids; i++){
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

  /* First pass: parse "prio:count\n" rows and sum totals so we can compute %.
   * Order-independent — children may report in any order. */
  struct { int prio; uint count; } rows[MAXKIDS];
  int nrows = 0;
  char *s = all;
  while(*s && nrows < nkids){
    // Children only ever report non-negative (user-class) priorities, so the
    // readback is a plain "prio:count" scan — no sign to parse.
    int prio = 0;
    while(*s >= '0' && *s <= '9'){ prio = prio*10 + (*s - '0'); s++; }
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

  if(nrows != nkids)
    printf("cfs_share: warning: parsed %d of %d rows\n", nrows, nkids);

  /* Machine-parseable rows for tools/bench_report.py (one CFSBENCH line each). */
  for(int i = 0; i < nrows; i++)
    printf("CFSBENCH prio=%d count=%u\n", rows[i].prio, rows[i].count);
  printf("CFSBENCH DONE\n");

  /* Human-readable share table (also keeps the "cfs_share" token the
   * ralph_battery S4 check looks for). */
  printf("\ncfs_share results:\n");
  for(int i = 0; i < nrows; i++){
    int pct = total ? (int)((uint64)rows[i].count * 100 / total) : 0;
    printf("  prio=%d count=%u share=%d%%\n",
           rows[i].prio, rows[i].count, pct);
  }
  printf("  (total=%d iterations)\n", total);

  exit(0);
}
