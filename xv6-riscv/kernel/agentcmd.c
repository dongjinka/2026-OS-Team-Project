// Kernel side of the agent command path.
//
// LLM commands arrive over the QEMU serial port as lines of the form
//
//   REQ|<CMD>|<arg>\n
//   REQ|agent:<role>|<CMD>|<arg>\n     (role optional; kept for wire-compat
//                                       but ignored — jail is the boundary)
//
// Two-stage design:
//   1) console.c (uart interrupt context) calls agent_dispatch() — enqueue only.
//   2) consoleread() (process context with valid myproc()) calls agent_drain().
// This split is mandatory because cache_get/cache_set / fs handlers may
// trigger begin_op() which can sleep; sleeping in interrupt context faults.
//
// Routing inside agent_dispatch_now():
//   - meta commands (ASK / LLM_RESP / CACHE_GET / CACHE_SET) are handled here
//     because the cache lives in kernel memory (cache.c). The orchestration
//     layer needs cache_get*/cache_set, which are kernel APIs.
//   - every other wire command (PRINT / CHAT / READ / WRITE / LS / NICE / PS /
//     LIST / SETPRIO / KILL / ...) is forwarded into agentq, the ring buffer
//     consumed by the jailed agentd runtime (user/agentd.c) via the
//     agent_recv() system call. agentd executes them *inside its chroot
//     jail* with dangerous syscalls (exec/kill/mknod) refused by the kernel.
//
// Human shell input never travels this path — only LLM-issued commands do.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "stat.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"

// ───────────────── stage-1 queue: interrupt → process ─────────────────
// agent_drain() pulls from this queue. Stage 1 may carry any wire line
// (meta or otherwise); routing happens in agent_dispatch_now().

#define AGENT_LINE_MAX 1280
#define AGENT_Q_LEN    8

static char  agent_q[AGENT_Q_LEN][AGENT_LINE_MAX];
static int   agent_q_head, agent_q_tail;
static struct spinlock agent_q_lock;

// ───────────────── stage-2 queue: kernel → jailed agentd ─────────────────
// Only non-meta wire commands land here. The jailed agentd pulls them via
// sys_agent_recv() and executes them in user space.

#define AGENTQ_N    16    // ring capacity
#define AGENTQ_LEN  256   // max line length

struct {
  struct spinlock lock;
  char buf[AGENTQ_N][AGENTQ_LEN];
  int r;       // next slot to read
  int w;       // next slot to write
  int count;
} agentq;

// pending LLM request — set by handle_ask on cache miss, consumed by
// handle_llm_resp so the response can be written back to cache_set with
// the right key. Single-flight (agent.py REPL is serial).
static char  pending_prompt[AGENT_LINE_MAX];
static int   pending_len;

// ───────────────── init ─────────────────

void
agentinit(void)
{
  initlock(&agent_q_lock, "agent_q");
  initlock(&agentq.lock, "agentq");
  agentq.r = agentq.w = agentq.count = 0;
}

// alias: legacy name used by main.c boot sequence on the main branch
void
agentcmd_init(void)
{
  agentinit();
}

// ───────────────── small utilities ─────────────────

static int
str_eq(const char *a, const char *b)
{
  while(*a && *b){ if(*a != *b) return 0; a++; b++; }
  return *a == *b;
}

static int
str_starts(const char *s, const char *prefix)
{
  while(*prefix){ if(*s++ != *prefix++) return 0; }
  return 1;
}

static void
copy_line(char *dst, const char *src, int max)
{
  int i = 0;
  while(i < max-1 && src[i]){ dst[i] = src[i]; i++; }
  dst[i] = 0;
}

// ───────────────── stage-2 enqueue (kernel → agentd) ─────────────────

// Forward a complete wire line ("REQ|<CMD>|<arg>") to the agentd queue.
// Drops silently if the queue is full.
static void
forward_to_agentd(const char *line)
{
  acquire(&agentq.lock);
  if(agentq.count == AGENTQ_N){
    release(&agentq.lock);
    printf("[agent] queue full, dropping line\n");
    return;
  }
  int i = 0;
  while(line[i] && i < AGENTQ_LEN - 1){
    agentq.buf[agentq.w][i] = line[i];
    i++;
  }
  agentq.buf[agentq.w][i] = 0;
  agentq.w = (agentq.w + 1) % AGENTQ_N;
  agentq.count++;
  release(&agentq.lock);
  wakeup(&agentq);
}

// Wrap a bare wire payload ("PRINT|hi", "WRITE|/x|hello") in "REQ|" and
// enqueue for agentd. Used by cache-hit and LLM_RESP paths, which carry
// the raw payload without the REQ| prefix.
static void
forward_wire_to_agentd(const char *wire)
{
  char buf[AGENTQ_LEN];
  int i = 0;
  buf[i++] = 'R'; buf[i++] = 'E'; buf[i++] = 'Q'; buf[i++] = '|';
  for(int j = 0; wire[j] && i < AGENTQ_LEN - 1; j++)
    buf[i++] = wire[j];
  buf[i] = 0;
  forward_to_agentd(buf);
}

// Blocking dequeue, called from sys_agent_recv() in process context.
// Copies the next command line into out (>= AGENTQ_LEN bytes). Returns
// the string length, or -1 if the caller was killed while waiting.
int
agentq_get(char *out)
{
  acquire(&agentq.lock);
  while(agentq.count == 0){
    if(killed(myproc())){
      release(&agentq.lock);
      return -1;
    }
    sleep(&agentq, &agentq.lock);
  }
  int i = 0;
  char *src = agentq.buf[agentq.r];
  while(src[i] && i < AGENTQ_LEN - 1){
    out[i] = src[i];
    i++;
  }
  out[i] = 0;
  agentq.r = (agentq.r + 1) % AGENTQ_N;
  agentq.count--;
  release(&agentq.lock);
  return i;
}

// ───────────────── meta-command handlers (in-kernel) ─────────────────
// ASK / LLM_RESP / CACHE_GET / CACHE_SET — orchestration only. They never
// touch the file system or kill processes; they only manipulate the
// kernel cache (cache.c) and forward results to agentd.

static void
handle_ask(char *arg)
{
  int plen = 0; while(arg[plen]) plen++;
  if(plen == 0) return;

  // CACHE_VAL = 1024 (cache.c) + null terminator.
  char cached[1025];

  // 1) exact cache hit — forward the cached wire to agentd.
  int clen = cache_get_exact(arg, plen, cached, sizeof(cached) - 1);
  if(clen >= 0){
    if(clen >= (int)sizeof(cached)) clen = sizeof(cached) - 1;
    cached[clen] = 0;
    printf("[cache] HIT\n");
    forward_wire_to_agentd(cached);
    return;
  }

  // 2) semantic hit — only for CHAT/PRINT prefixes (no side-effect commands)
  //    so paraphrase matching never re-runs a WRITE/KILL/NICE.
  int score = 0;
  clen = cache_get_semantic(arg, plen, cached, sizeof(cached) - 1, &score);
  if(clen >= 0){
    if(clen >= (int)sizeof(cached)) clen = sizeof(cached) - 1;
    cached[clen] = 0;
    if(str_starts(cached, "CHAT|") || str_starts(cached, "PRINT|")){
      printf("[cache] SEMANTIC HIT score=%d/64\n", score);
      forward_wire_to_agentd(cached);
      return;
    }
    // side-effect action — drop the semantic hit, fall through to LLM
  }

  // 3) miss — record pending and ask the host to call the LLM.
  copy_line(pending_prompt, arg, AGENT_LINE_MAX);
  pending_len = plen;
  printf("LLM_REQ|%s\n", arg);
}

static void
handle_llm_resp(char *arg)
{
  int alen = 0; while(arg[alen]) alen++;
  if(pending_len > 0){
    int rc = cache_set(pending_prompt, pending_len, arg, alen);
    if(rc < 0) printf("[cache] DROP (oversized %d B)\n", alen);
    pending_len = 0;
  }
  forward_wire_to_agentd(arg);
}

static void
handle_cache_get(char *arg)
{
  int klen = 0; while(arg[klen]) klen++;
  if(klen == 0){ printf("RESP|MISS\n"); return; }
  char valbuf[1024];   // CACHE_VAL
  int vlen = cache_get(arg, klen, valbuf, sizeof(valbuf));
  if(vlen < 0){
    printf("RESP|MISS\n");
  } else {
    printf("RESP|HIT|");
    if(vlen > (int)sizeof(valbuf)) vlen = sizeof(valbuf);
    for(int i = 0; i < vlen; i++) consputc(valbuf[i]);
    consputc('\n');
  }
}

static void
handle_cache_set(char *arg)
{
  char *p = arg;
  int klen = 0;
  while(*p >= '0' && *p <= '9'){ klen = klen*10 + (*p - '0'); p++; }
  if(*p != ':' || klen <= 0){ printf("RESP|ERR\n"); return; }
  p++;
  int rem = 0; while(p[rem]) rem++;
  if(rem < klen){ printf("RESP|ERR\n"); return; }
  int rc = cache_set(p, klen, p + klen, rem - klen);
  printf("%s\n", (rc == 0) ? "RESP|OK" : "RESP|ERR");
}

// ───────────────── interrupt-safe enqueue ─────────────────
// console.c calls this for each "REQ|" line it sniffs from the serial port.
// Interrupt context — only spinlock-protected enqueue, no sleep.

void
agent_dispatch(char *line)
{
  acquire(&agent_q_lock);
  int next = (agent_q_tail + 1) % AGENT_Q_LEN;
  if(next != agent_q_head){
    copy_line(agent_q[agent_q_tail], line, AGENT_LINE_MAX);
    agent_q_tail = next;
  }
  // queue full: silently drop
  release(&agent_q_lock);
}

// ───────────────── process-context drain + route ─────────────────

void
agent_drain(void)
{
  for(;;){
    char local[AGENT_LINE_MAX];
    acquire(&agent_q_lock);
    if(agent_q_head == agent_q_tail){
      release(&agent_q_lock);
      return;
    }
    copy_line(local, agent_q[agent_q_head], AGENT_LINE_MAX);
    agent_q_head = (agent_q_head + 1) % AGENT_Q_LEN;
    release(&agent_q_lock);

    agent_dispatch_now(local);
  }
}

// Parse "REQ|[agent:<role>|]<CMD>|<arg>" and route:
//   - meta cmds run in-kernel (orchestration + cache)
//   - everything else is forwarded to the jailed agentd
void
agent_dispatch_now(char *line)
{
  if(!(line[0]=='R' && line[1]=='E' && line[2]=='Q' && line[3]=='|'))
    return;

  // Skip optional "agent:<role>|" prefix. role is currently ignored — jail
  // (not ACL) is the sandboxing boundary. The prefix is kept on the wire
  // for diagnostic logs and possible future per-role policy.
  char *cmd_start = line + 4;
  if(str_starts(cmd_start, "agent:")){
    char *bar = cmd_start + 6;
    while(*bar && *bar != '|') bar++;
    if(*bar != '|'){ printf("[agent] malformed role prefix\n"); return; }
    cmd_start = bar + 1;
  }

  // Split CMD / arg (arg may be empty for no-arg cmds like PS / LIST).
  char *bar = cmd_start;
  while(*bar && *bar != '|') bar++;
  char *cmd = cmd_start;
  char *arg;
  if(*bar == '|'){
    *bar = 0;
    arg = bar + 1;
  } else {
    arg = bar;
  }

  // Meta commands — in-kernel, never reach agentd.
  if(str_eq(cmd, "ASK"))       { handle_ask(arg);       return; }
  if(str_eq(cmd, "LLM_RESP"))  { handle_llm_resp(arg);  return; }
  if(str_eq(cmd, "CACHE_GET")) { handle_cache_get(arg); return; }
  if(str_eq(cmd, "CACHE_SET")) { handle_cache_set(arg); return; }

  // Everything else — restore original separator and forward verbatim to
  // the jailed agentd. agentd parses its own wire ("REQ|<CMD>|<arg>") with
  // the format the main branch already uses.
  if(arg != bar) *bar = '|';
  forward_to_agentd(line);
}
