// Evaluation harness for the LLM-OS kernel.
//
//   eval cache    <N>   LLM cache: round-1 miss / round-2 hit rate
//   eval acl      <N>   Role ACL: reader×KILL deny rate against a live child
//   eval fair     <I>   CFS fairness: prio=0 vs prio=20 completion time @ I iters
//   eval semantic <N>   MinHash semantic cache: exact / paraphrase / unrelated
//
// All exercise OS concepts substantively:
//   cache     -> RAM table + /cache.bin disk overlay (synchronization, fs)
//   acl       -> kernel ACL gate (sandboxing) + fork()/kill()/wait()
//   fair      -> CFS scheduler (vruntime + priority), uptime ticks
//   semantic  -> MinHash signatures in RAM, Jaccard threshold match

#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define EVAL_BUF 256

static int
u2s(int n, char *out)
{
  if(n == 0){ out[0]='0'; out[1]=0; return 1; }
  char tmp[16];
  int len = 0;
  while(n > 0){ tmp[len++] = '0' + (n%10); n /= 10; }
  for(int i = 0; i < len; i++) out[i] = tmp[len-1-i];
  out[len] = 0;
  return len;
}

// "ec_<i>" → buf, returns length
static int
build_key(char *buf, int i)
{
  buf[0]='e'; buf[1]='c'; buf[2]='_';
  char num[8];
  int nlen = u2s(i, num);
  for(int j = 0; j < nlen; j++) buf[3+j] = num[j];
  buf[3+nlen] = 0;
  return 3 + nlen;
}

// "v<i>" → buf, returns length
static int
build_val(char *buf, int i)
{
  buf[0] = 'v';
  char num[8];
  int nlen = u2s(i, num);
  for(int j = 0; j < nlen; j++) buf[1+j] = num[j];
  buf[1+nlen] = 0;
  return 1 + nlen;
}

// ---------- cache hit-rate ----------
static void
eval_cache(int n)
{
  printf("=== eval cache N=%d ===\n", n);

  char k[16], v[16], buf[EVAL_BUF];
  int miss_r1 = 0, hit_r1 = 0;
  int miss_r2 = 0, hit_r2 = 0;

  // Round 1: probe N keys (likely miss), then set
  for(int i = 0; i < n; i++){
    int klen = build_key(k, i);
    int r = get_cache(k, klen, buf, sizeof(buf));
    if(r < 0) miss_r1++; else hit_r1++;
    int vlen = build_val(v, i);
    set_cache(k, klen, v, vlen);
  }

  // Round 2: probe same N keys (expect mostly hits — some RAM evicted
  //          to disk if N > 16, but still recovered via promote)
  for(int i = 0; i < n; i++){
    int klen = build_key(k, i);
    int r = get_cache(k, klen, buf, sizeof(buf));
    if(r < 0) miss_r2++; else hit_r2++;
  }

  int total = 2 * n;
  int total_hits = hit_r1 + hit_r2;
  printf("  round 1 (cold) : miss=%d  hit=%d\n", miss_r1, hit_r1);
  printf("  round 2 (warm) : miss=%d  hit=%d\n", miss_r2, hit_r2);
  printf("  overall        : hits %d / total %d (%d%%)\n",
         total_hits, total, total_hits * 100 / total);
}

// ---------- ACL deny-rate ----------
// Strategy: for each trial, fork a pausing child, then dispatch a
// reader-role KILL targeting that child. If ACL works, the kernel
// denies the kill and the child stays alive — the parent can kill
// it (returns 0). If ACL leaks, the child is already dead — parent's
// kill returns -1.
static void
eval_acl(int n)
{
  printf("=== eval acl N=%d ===\n", n);
  int denied = 0;

  for(int i = 0; i < n; i++){
    int child = fork();
    if(child < 0){ printf("  fork failed\n"); return; }
    if(child == 0){
      pause(50);    // stay alive ~50 ticks
      exit(0);
    }

    // Build "REQ|agent:reader|KILL|<child>" — ACL should deny
    char buf[64];
    char *prefix = "REQ|agent:reader|KILL|";
    int plen = strlen(prefix);
    int p = 0;
    for(int j = 0; j < plen; j++) buf[p++] = prefix[j];
    char num[16];
    int nlen = u2s(child, num);
    for(int j = 0; j < nlen; j++) buf[p++] = num[j];
    buf[p] = 0;

    dispatch(buf);

    // If ACL denied the reader-role KILL, child is alive — parent's
    // unrestricted kill() succeeds.
    int rc = kill(child);
    if(rc == 0) denied++;
    wait(0);
  }

  printf("  reader×KILL : denied %d / %d (%d%%)\n",
         denied, n, denied * 100 / n);
}

// ---------- CFS fairness ----------
// 4 concurrent spinners on a 3-CPU QEMU — forced contention exposes the
// scheduler's priority weighting. 2 spinners at prio=0 (high) vs 2 at
// prio=20 (low). All run for the same wall-clock window; the work counter
// at exit shows how much CPU each got from the CFS scheduler.
static void
eval_fair(int target_ticks)
{
  printf("=== eval fair target_ticks=%d (4 spinners on 3 CPUs) ===\n", target_ticks);

  int prios[4]      = {0, 0, 20, 20};
  const char *lbl[] = {"A prio=0 ", "B prio=0 ", "C prio=20", "D prio=20"};

  for(int k = 0; k < 4; k++){
    int pid = fork();
    if(pid < 0){ printf("  fork failed\n"); return; }
    if(pid == 0){
      setpriority(getpid(), prios[k]);
      int t0 = uptime();
      int work = 0;
      while(uptime() - t0 < target_ticks){
        for(volatile int i = 0; i < 100000; i++) {}
        work++;
      }
      printf("  [%s] work=%d units in %d ticks\n",
             lbl[k], work, uptime() - t0);
      exit(0);
    }
  }
  for(int k = 0; k < 4; k++) wait(0);
  printf("  higher work count → CFS gave more CPU to that priority class\n");
}

// ---------- semantic cache recall ----------
// 같은 length 의 3가지 변형으로 N 케이스 적재 후 측정:
//   base       : "the quick fox runs around forest number <i>"
//   paraphrase : "quick the fox runs around forest number <i>" (어순만 swap)
//   unrelated  : "kernel scheduler trap handler index <i>"
// Round 1: base 키로 적재. Round 2/3/4: 각 변형으로 get_cache 호출.
//   exact hit  = round 2 hit  (cache_get wrapper 의 exact 단계)
//   semantic hit ≈ round 3 hit (exact 미스 → semantic fallback)
//   miss       = round 4 miss

static int
build_semkey(char *buf, const char *pfx, int i)
{
  int p = 0;
  while(pfx[p]){ buf[p] = pfx[p]; p++; }
  char num[8];
  int nlen = u2s(i, num);
  for(int j = 0; j < nlen; j++) buf[p++] = num[j];
  buf[p] = 0;
  return p;
}

static void
eval_semantic(int n)
{
  printf("=== eval semantic N=%d ===\n", n);
  if(n > 16){
    printf("  (note: N>16 → RAM 16 슬롯 LRU evict, paraphrase 일부 손실)\n");
  }

  char k[128], v[16], buf[EVAL_BUF];
  int exact_hits = 0, semantic_hits = 0, neg_misses = 0;

  // Round 1: base 키 N개 적재. 값은 "CHAT|v<i>" 로 (handle_ask 의 prefix
  // 가드와 일관 — user-space 에서는 prefix 검사 없지만 형식 통일).
  for(int i = 0; i < n; i++){
    int klen = build_semkey(k, "the quick fox runs around forest number ", i);
    v[0]='C'; v[1]='H'; v[2]='A'; v[3]='T'; v[4]='|'; v[5]='v';
    int vp = 6;
    char num[8];
    int nlen = u2s(i, num);
    for(int j = 0; j < nlen; j++) v[vp++] = num[j];
    set_cache(k, klen, v, vp);
  }

  // Round 2: 정확 키 — exact hit
  for(int i = 0; i < n; i++){
    int klen = build_semkey(k, "the quick fox runs around forest number ", i);
    if(get_cache(k, klen, buf, sizeof(buf)) >= 0) exact_hits++;
  }

  // Round 3: paraphrase (the/quick 어순 swap) — semantic fallback
  for(int i = 0; i < n; i++){
    int klen = build_semkey(k, "quick the fox runs around forest number ", i);
    if(get_cache(k, klen, buf, sizeof(buf)) >= 0) semantic_hits++;
  }

  // Round 4: 무관 키 — miss
  for(int i = 0; i < n; i++){
    int klen = build_semkey(k, "kernel scheduler trap handler index ", i);
    if(get_cache(k, klen, buf, sizeof(buf)) < 0) neg_misses++;
  }

  printf("  exact probes      : hits=%d / %d (%d%%)\n",
         exact_hits, n, exact_hits * 100 / n);
  printf("  paraphrase probes : hits=%d / %d (%d%%)\n",
         semantic_hits, n, semantic_hits * 100 / n);
  printf("  unrelated probes  : miss=%d / %d (%d%%)\n",
         neg_misses, n, neg_misses * 100 / n);
}

int
main(int argc, char *argv[])
{
  if(argc < 3){
    printf("usage: eval <cache|acl|fair|semantic> <N>\n");
    printf("  cache    N : LLM cache hit-rate over N keys × 2 rounds\n");
    printf("  acl      N : reader×KILL deny rate over N child procs\n");
    printf("  fair     I : prio=0 vs prio=20 spin completion times @ I iters\n");
    printf("  semantic N : MinHash recall — exact/paraphrase/unrelated × N\n");
    exit(1);
  }

  int n = atoi(argv[2]);
  if(n <= 0){ printf("bad N: %s\n", argv[2]); exit(1); }

  if(strcmp(argv[1], "cache") == 0) eval_cache(n);
  else if(strcmp(argv[1], "acl") == 0) eval_acl(n);
  else if(strcmp(argv[1], "fair") == 0) eval_fair(n);
  else if(strcmp(argv[1], "semantic") == 0) eval_semantic(n);
  else { printf("unknown subcommand %s\n", argv[1]); exit(1); }

  exit(0);
}
