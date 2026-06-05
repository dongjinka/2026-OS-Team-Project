/*
 * cfs_bench.c — CFS priority -> CPU-share sweep (report benchmark).
 *
 * Generalizes cfs_share.c (which races a fixed 3 priorities) to a full
 * user-range sweep, emitting machine-parseable rows that tools/bench_report.py
 * turns into a "CPU share vs priority" table/curve for the Technical Report.
 * It also speaks to the documented open issue (Project_Guide §11.8): whether
 * mid-range priorities 1..19 separate cleanly under enough load.
 *
 * Each child pins itself to one priority and spins for the same wall-clock
 * window (uptime() ticks); its loop count is the CPU it won. Output:
 *
 *   CFSBENCH start nkids=6 ticks=150
 *   CFSBENCH prio=0 count=12345678
 *   ...
 *   CFSBENCH DONE
 *
 * Run with smp=1 so a multi-core machine doesn't trivially absorb every
 * runnable child (tools/bench_report.py does this).
 *
 * NOTE: only non-negative (user-class) priorities — a user-class process may
 * not grant itself kernel-class (negative) priority.
 */
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define DURATION_TICKS 150          // ~15s wall-clock @ ~100ms/tick
#define NKIDS          6

// Sweep across the whole user range so the curve is visible end to end.
static const int prios[NKIDS] = { 0, 4, 8, 12, 16, 19 };

int
main(int argc, char *argv[])
{
  int p[2];
  if(pipe(p) < 0){
    printf("cfs_bench: pipe failed\n");
    exit(1);
  }

  printf("CFSBENCH start nkids=%d ticks=%d\n", NKIDS, DURATION_TICKS);

  for(int i = 0; i < NKIDS; i++){
    int pid = fork();
    if(pid < 0){
      printf("cfs_bench: fork failed\n");
      exit(1);
    }
    if(pid == 0){
      close(p[0]);
      setpriority(getpid(), prios[i]);

      uint start = uptime();
      uint deadline = start + DURATION_TICKS;
      volatile uint count = 0;
      while(uptime() < deadline){
        for(int k = 0; k < 1000; k++)
          count++;
      }

      fprintf(p[1], "%d:%u\n", prios[i], count);
      exit(0);
    }
  }

  close(p[1]);
  for(int i = 0; i < NKIDS; i++){
    int st;
    wait(&st);
  }

  char all[512];
  int got = 0, n;
  while((n = read(p[0], all + got, sizeof(all) - 1 - got)) > 0)
    got += n;
  close(p[0]);
  all[got] = 0;

  // Re-emit each row on its own CFSBENCH line so the host parser is trivial
  // and order-independent (children may report in any order).
  char *s = all;
  while(*s){
    int prio = 0;
    while(*s >= '0' && *s <= '9'){ prio = prio*10 + (*s - '0'); s++; }
    if(*s != ':') break;
    s++;
    uint count = 0;
    while(*s >= '0' && *s <= '9'){ count = count*10 + (*s - '0'); s++; }
    if(*s == '\n') s++;
    printf("CFSBENCH prio=%d count=%u\n", prio, count);
  }
  printf("CFSBENCH DONE\n");
  exit(0);
}
