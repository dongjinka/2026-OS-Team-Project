// Kernel side of the agent command path.
//
// LLM commands arrive over the QEMU serial port as lines of the form
//
//   REQ|<CMD>|<arg>\n
//   REQ|agent:<role>|<CMD>|<arg>\n   (role optional; stripped + ignored here —
//                                     the chroot jail, not ACL, is the boundary)
//
// Two-stage design (ported from commit 76b2737, Sejoong branch, then merged
// with SeungBeom's configurable deny list):
//   1) console.c / sys_dispatch enqueue raw lines via agent_dispatch() — this
//      runs in *interrupt context* and must not sleep, so it only copies the
//      line into a small intake ring (agent_q).
//   2) agent_drain() runs in *process context* (usertrap + consoleread) and
//      routes each line through agent_dispatch_now(). The split is mandatory
//      because the cache handlers call cache_*() which may begin_op()/sleep —
//      illegal in interrupt context.
//
// Routing inside agent_dispatch_now():
//   - meta commands (ASK / LLM_RESP / CACHE_GET / CACHE_SET) are handled in the
//     kernel because the response cache (cache.c, F9) lives in kernel memory.
//     ASK consults the cache and only emits LLM_REQ| to the host on a miss —
//     this is what makes the cache actually skip Solar API round-trips.
//   - every other wire command is checked against the deny list (F7) and, if
//     allowed, forwarded into agentq, the ring the jailed agentd consumes via
//     agent_recv(). Human shell input never travels this path.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "deny.h"

// ───────────────── stage-1 intake queue: interrupt → process ─────────────────
// Holds any raw wire line (meta or not); routing happens in dispatch_now.
// AGENT_LINE_MAX is large because ASK carries a full natural-language prompt.
#define AGENT_LINE_MAX 1280
#define AGENT_Q_LEN    8

static char  agent_q[AGENT_Q_LEN][AGENT_LINE_MAX];
static int   agent_q_head, agent_q_tail;
static struct spinlock agent_q_lock;

// ───────────────── stage-2 queue: kernel → jailed agentd ─────────────────
#define AGENTQ_N    16     // ring buffer capacity (commands)
#define AGENTQ_LEN  256    // max length of one command line

struct {
  struct spinlock lock;
  char buf[AGENTQ_N][AGENTQ_LEN];
  int r;          // next slot to read
  int w;          // next slot to write
  int count;      // queued commands
} agentq;

// pending LLM request — set by handle_ask on a cache miss, consumed by
// handle_llm_resp so the host's response is cache_set under the right key.
// agent_drain() can run on several CPUs at once, so pending_prompt/pending_len
// are guarded by pending_lock (a later ASK must not clobber the key an
// in-flight LLM_RESP will cache under).
static char  pending_prompt[AGENT_LINE_MAX];
static int   pending_len;
static struct spinlock pending_lock;

// Commands the kernel refuses outright: they never reach the agent runtime
// (F7 — the hard sandbox boundary). Managed at runtime via set_deny()/get_deny()
// and the `denyctl` shell tool. Guarded by a spinlock.
struct {
  struct spinlock lock;
  char names[DENY_MAX][DENY_NAMELEN];
  int count;
} denylist;

static const char *deny_default[] = { "KILL", "EXEC" };
#define NDENY_DEFAULT ((int)(sizeof(deny_default) / sizeof(deny_default[0])))

// Restore the built-in default deny list. Also used at boot.
void
deny_reset(void)
{
  acquire(&denylist.lock);
  denylist.count = 0;
  for(int i = 0; i < NDENY_DEFAULT && i < DENY_MAX; i++)
    safestrcpy(denylist.names[denylist.count++], deny_default[i], DENY_NAMELEN);
  release(&denylist.lock);
}

// Empty the deny list (nothing blocked at the kernel boundary).
void
deny_clear(void)
{
  acquire(&denylist.lock);
  denylist.count = 0;
  release(&denylist.lock);
}

// Add cmd to the deny list. Idempotent. Returns 0 on success, -1 if full.
int
deny_add(const char *cmd)
{
  acquire(&denylist.lock);
  for(int i = 0; i < denylist.count; i++){
    if(strncmp(denylist.names[i], cmd, DENY_NAMELEN) == 0){
      release(&denylist.lock);
      return 0;
    }
  }
  if(denylist.count >= DENY_MAX){
    release(&denylist.lock);
    return -1;
  }
  safestrcpy(denylist.names[denylist.count++], cmd, DENY_NAMELEN);
  release(&denylist.lock);
  return 0;
}

// Remove cmd from the deny list. Returns 0 if removed, -1 if not present.
int
deny_remove(const char *cmd)
{
  acquire(&denylist.lock);
  for(int i = 0; i < denylist.count; i++){
    if(strncmp(denylist.names[i], cmd, DENY_NAMELEN) == 0){
      for(int j = i; j < denylist.count - 1; j++)
        safestrcpy(denylist.names[j], denylist.names[j + 1], DENY_NAMELEN);
      denylist.count--;
      release(&denylist.lock);
      return 0;
    }
  }
  release(&denylist.lock);
  return -1;
}

// Copy the deny list into buf as newline-separated names. Returns byte length.
int
deny_snapshot(char *buf, int max)
{
  int n = 0;
  acquire(&denylist.lock);
  for(int i = 0; i < denylist.count; i++){
    const char *s = denylist.names[i];
    while(*s && n < max - 2)
      buf[n++] = *s++;
    if(n < max - 1)
      buf[n++] = '\n';
  }
  release(&denylist.lock);
  if(n < max)
    buf[n] = 0;
  return n;
}

void
agentcmd_init(void)
{
  initlock(&agent_q_lock, "agent_q");
  agent_q_head = agent_q_tail = 0;
  initlock(&agentq.lock, "agentq");
  agentq.r = agentq.w = agentq.count = 0;
  initlock(&pending_lock, "agent_pending");
  pending_len = 0;
  initlock(&denylist.lock, "denylist");
  deny_reset();
}

// True if cmd is on the deny list. Called from dispatch_now (process context);
// the spinlock is fine either way.
static int
deny_listed(const char *cmd)
{
  int found = 0;
  acquire(&denylist.lock);
  for(int i = 0; i < denylist.count; i++){
    if(strncmp(denylist.names[i], cmd, DENY_NAMELEN) == 0){
      found = 1;
      break;
    }
  }
  release(&denylist.lock);
  return found;
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

// ───────────────── stage-2 enqueue (kernel → agentd) ─────────────────

// Forward a complete wire line ("REQ|<CMD>|<arg>") to the agentd queue.
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

// Wrap a bare wire payload ("PRINT|hi", "CHAT|...") in "REQ|" and enqueue for
// agentd. Used by the cache-hit and LLM_RESP paths, and by the forward path
// after the optional "agent:<role>|" prefix has been stripped.
static void
forward_wire_to_agentd(const char *wire)
{
  // F7 single chokepoint. EVERY path that hands a wire to agentd funnels
  // through here: the normal command path AND the ASK / LLM_RESP / semantic
  // cache-hit paths. Enforce the deny list here (not only in
  // agent_dispatch_now) so a denied command (e.g. an admin-added WRITE/SPAWN)
  // cannot be laundered past F7 via the meta-command forwards — handle_ask's
  // exact-hit and handle_llm_resp previously skipped the deny check entirely.
  char cmd[DENY_NAMELEN];
  int n = 0;
  while(wire[n] && wire[n] != '|' && n < DENY_NAMELEN - 1){ cmd[n] = wire[n]; n++; }
  cmd[n] = 0;
  if(deny_listed(cmd)){
    printf("[agent] DENY '%s' (sandboxed: never reaches the agent)\n", cmd);
    return;
  }

  char buf[AGENTQ_LEN];
  int i = 0;
  buf[i++] = 'R'; buf[i++] = 'E'; buf[i++] = 'Q'; buf[i++] = '|';
  // Stop at a control byte: a value planted in the user-writable /cache.bin and
  // replayed via ASK could embed a newline that would otherwise forge a second
  // queued command line. Wire fields are single-line by construction.
  for(int j = 0; wire[j] && wire[j] != '\n' && wire[j] != '\r' && i < AGENTQ_LEN - 1; j++)
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

// ───────────────── meta-command handlers (in-kernel, process context) ─────────
// ASK / LLM_RESP / CACHE_GET / CACHE_SET — orchestration only. They manipulate
// the kernel cache (cache.c) and forward results to agentd; they never touch
// the deny list (F7 only gates commands that reach the jailed runtime).

static void
handle_ask(char *arg)
{
  int plen = 0; while(arg[plen]) plen++;
  if(plen == 0) return;

  char cached[CACHE_VAL + 1];   // value cap + room for the NUL terminator

  // 1) exact cache hit — forward the cached wire to agentd, skip the LLM.
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
  acquire(&pending_lock);
  safestrcpy(pending_prompt, arg, AGENT_LINE_MAX);
  pending_len = plen;
  release(&pending_lock);
  printf("LLM_REQ|%s\n", arg);
}

static void
handle_llm_resp(char *arg)
{
  int alen = 0; while(arg[alen]) alen++;

  // Snapshot the pending key under the lock, then cache_set outside it
  // (cache_set may sleep on begin_op(), illegal while holding a spinlock).
  // key[] is deliberately small: this runs on the agent_drain() call chain,
  // which already spends ~1280 B (drain local) + ~1KB (cache_set) of the
  // one-page kernel stack — a full AGENT_LINE_MAX buffer here overflows it.
  // Prompts longer than this simply aren't cached (best-effort cache).
  char key[256];
  int klen;
  acquire(&pending_lock);
  klen = pending_len;
  pending_len = 0;
  if(klen > 0 && klen < (int)sizeof(key))
    safestrcpy(key, pending_prompt, sizeof(key));
  else
    klen = 0;
  release(&pending_lock);

  if(klen > 0){
    int rc = cache_set(key, klen, arg, alen);
    if(rc < 0) printf("[cache] DROP (oversized %d B)\n", alen);
  }
  forward_wire_to_agentd(arg);
}

static void
handle_cache_get(char *arg)
{
  int klen = 0; while(arg[klen]) klen++;
  if(klen == 0){ printf("RESP|MISS\n"); return; }
  char valbuf[CACHE_VAL + 1];   // value cap + room for the NUL terminator
  int vlen = cache_get(arg, klen, valbuf, sizeof(valbuf) - 1);
  if(vlen < 0){
    printf("RESP|MISS\n");
  } else {
    // Emit the whole RESP line in ONE printf() so it is atomic under the printf
    // lock. The old code wrote the value via bare consputc() (UNLOCKED), so at
    // smp>1 another CPU's console output interleaved into "RESP|HIT|<value>",
    // and the host parsed the garbled line as a cache MISS — the F9 cache
    // silently failed at the default `make qemu-agent` (smp=3). %s does not
    // interpret '%' in the value, and cache values are nul-free text.
    // The buffer is CACHE_VAL+1 so a full 1024-byte value keeps all its bytes
    // (the NUL lands at valbuf[1024]) instead of dropping the last one.
    if(vlen > (int)sizeof(valbuf) - 1) vlen = sizeof(valbuf) - 1;
    valbuf[vlen] = 0;
    printf("RESP|HIT|%s\n", valbuf);
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

// CONFIRM_RES|<pid>|y    또는   CONFIRM_RES|<pid>|n
// 호스트 사용자의 jail confirm-escape 응답. confirm.c 의 pending 요청을 깨움.
static void
handle_confirm_res(char *arg)
{
  // pid 파싱
  char *p = arg;
  int pid = 0;
  while(*p >= '0' && *p <= '9'){ pid = pid*10 + (*p - '0'); p++; }
  if(*p != '|' || pid <= 0) return;
  p++;
  int allow = (*p == 'y' || *p == 'Y') ? 1 : 0;
  confirm_resolve(pid, allow);
}

// CONFIRM_RES 와이어 inline 처리 — agent_dispatch (interrupt context) 에서
// 큐 우회. 이유: 큐 drain 은 usertrap 끝에서만 호출되는데, confirm-escape
// 경로에서는 모든 user proc 이 SLEEPING (자식은 confirm_wait_chan, 부모는
// wait()) 이라 user trap 이 발생 안 함 → 큐 정체. CONFIRM_RES 처리는
// sleep/FS 가 없어 (spinlock + wakeup 만) interrupt-safe 하므로 인라인 OK.
static int
try_inline_confirm_res(const char *line)
{
  // expected: "REQ|[agent:<role>|]CONFIRM_RES|<pid>|<y|n>"
  if(!(line[0]=='R' && line[1]=='E' && line[2]=='Q' && line[3]=='|')) return 0;
  const char *cmd = line + 4;
  // optional role prefix
  if(cmd[0]=='a' && cmd[1]=='g' && cmd[2]=='e' && cmd[3]=='n' &&
     cmd[4]=='t' && cmd[5]==':'){
    const char *bar = cmd + 6;
    while(*bar && *bar != '|') bar++;
    if(*bar != '|') return 0;
    cmd = bar + 1;
  }
  // exact "CONFIRM_RES|"
  const char *want = "CONFIRM_RES|";
  for(int i = 0; want[i]; i++) if(cmd[i] != want[i]) return 0;
  const char *arg = cmd + 12;
  int pid = 0;
  while(*arg >= '0' && *arg <= '9'){ pid = pid*10 + (*arg - '0'); arg++; }
  if(*arg != '|' || pid <= 0) return 1;   // consumed (malformed → drop)
  arg++;
  int allow = (*arg == 'y' || *arg == 'Y') ? 1 : 0;
  confirm_resolve(pid, allow);
  return 1;
}

// ───────────────── stage-1 enqueue (interrupt-safe) ─────────────────
// console.c calls this for each "REQ|" line it sniffs from the serial port.
// Interrupt context — only spinlock-protected enqueue, never sleeps.
void
agent_dispatch(char *line)
{
  if(try_inline_confirm_res(line)) return;

  acquire(&agent_q_lock);
  int next = (agent_q_tail + 1) % AGENT_Q_LEN;
  if(next != agent_q_head){
    safestrcpy(agent_q[agent_q_tail], line, AGENT_LINE_MAX);
    agent_q_tail = next;
  }
  // queue full: silently drop
  release(&agent_q_lock);
}

// ───────────────── process-context drain + route ─────────────────
// Pulls every queued line and routes it. Separated from agent_drain() so the
// 1280-byte local frame is only reserved when there is actually work to do.
static void
agent_drain_locked(void)
{
  for(;;){
    char local[AGENT_LINE_MAX];
    acquire(&agent_q_lock);
    if(agent_q_head == agent_q_tail){
      release(&agent_q_lock);
      return;
    }
    safestrcpy(local, agent_q[agent_q_head], AGENT_LINE_MAX);
    agent_q_head = (agent_q_head + 1) % AGENT_Q_LEN;
    release(&agent_q_lock);

    agent_dispatch_now(local);
  }
}

// Called from usertrap() (every trap) and consoleread(), in process context.
// The unlocked head==tail check keeps the common empty case off the spinlock;
// a stale read at worst defers one line to the next trap, where the locked
// re-check in agent_drain_locked() is authoritative.
void
agent_drain(void)
{
  if(agent_q_head == agent_q_tail)
    return;
  agent_drain_locked();
}

// Parse "REQ|[agent:<role>|]<CMD>|<arg>" and route:
//   - meta cmds run in-kernel (cache orchestration)
//   - everything else is deny-checked (F7) then forwarded to the jailed agentd
// Also callable directly from sys_dispatch() (already in process context).
void
agent_dispatch_now(char *line)
{
  if(!(line[0]=='R' && line[1]=='E' && line[2]=='Q' && line[3]=='|'))
    return;

  // Skip optional "agent:<role>|" prefix. role is ignored — jail (not ACL) is
  // the boundary; the prefix is kept on the wire for diagnostic logs.
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
  int had_bar = (*bar == '|');
  if(had_bar){
    *bar = 0;
    arg = bar + 1;
  } else {
    arg = bar;
  }

  // Meta commands — in-kernel, never reach agentd or the deny list.
  if(str_eq(cmd, "ASK"))         { handle_ask(arg);         return; }
  if(str_eq(cmd, "LLM_RESP"))    { handle_llm_resp(arg);    return; }
  if(str_eq(cmd, "CACHE_GET"))   { handle_cache_get(arg);   return; }
  if(str_eq(cmd, "CACHE_SET"))   { handle_cache_set(arg);   return; }
  if(str_eq(cmd, "CONFIRM_RES")) { handle_confirm_res(arg); return; }

  // F7: hard sandbox boundary — denied commands never reach the agent runtime.
  if(deny_listed(cmd)){
    printf("[agent] DENY '%s' (sandboxed: never reaches the agent)\n", cmd);
    return;
  }

  // Forward "CMD|arg" (role prefix dropped) wrapped as "REQ|CMD|arg" so the
  // jailed agentd sees the plain wire it already parses.
  if(had_bar) *bar = '|';
  forward_wire_to_agentd(cmd_start);
}
