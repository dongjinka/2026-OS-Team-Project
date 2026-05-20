// Kernel-side dispatcher for agent commands sent over the QEMU serial port
// (TCP 4444 in the qemu-agent target). The line protocol is:
//
//   REQ|<CMD>|<arg>\n
//   REQ|agent:<role>|<CMD>|<arg>\n     (role optional; defaults to "reader")
//
// Two-stage design:
//   1) console.c (uart interrupt context) calls agent_dispatch() — enqueue only.
//   2) consoleread() (process context with valid myproc()) calls agent_drain().
// This split is mandatory because cache_get/cache_set / fs handlers may
// trigger begin_op() which can sleep; sleeping in interrupt context faults.
//
// Phase 5: role-based ACL sandboxing. Every wire command is gated by the
// caller's role (reader / writer / admin). Denied commands print a [deny]
// line and produce no side effect.

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

extern struct proc proc[NPROC];

// agent_drain 이 이 크기의 stack-local 버퍼를 하나 잡으므로 4 KB 커널 스택
// 예산을 고려해야 한다. 최대 와이어: REQ|agent:<role>|LLM_RESP|CHAT|<CACHE_VAL>
// ≈ 1050 B + 여유. 1280 으로 둠.
#define AGENT_LINE_MAX 1280
#define AGENT_Q_LEN    8

static char  agent_q[AGENT_Q_LEN][AGENT_LINE_MAX];
static int   agent_q_head, agent_q_tail;
static struct spinlock agent_q_lock;

// ---------- ACL ----------
#define ACL_PRINT  0x001
#define ACL_CHAT   0x002
#define ACL_READ   0x004
#define ACL_LS     0x008
#define ACL_PS     0x010
#define ACL_WRITE  0x020
#define ACL_KILL   0x040
#define ACL_NICE   0x080

enum agent_role { ROLE_READER=0, ROLE_WRITER=1, ROLE_ADMIN=2, ROLE_MAX=3 };

static const ushort acl_mask[ROLE_MAX] = {
  /* reader */ ACL_PRINT|ACL_CHAT|ACL_READ|ACL_LS|ACL_PS,
  /* writer */ ACL_PRINT|ACL_CHAT|ACL_READ|ACL_LS|ACL_PS|ACL_WRITE,
  /* admin  */ 0xFFFF,
};

static const char *role_name[ROLE_MAX] = {"reader","writer","admin"};

// 진행 중인 LLM 요청 — LLM_RESP 도착 시 캐시 키 + 적용 role 로 쓰인다.
// 한 번에 하나의 요청만 진행된다고 가정 (agent.py REPL이 직렬이므로 안전).
static char  pending_prompt[AGENT_LINE_MAX];
static int   pending_len;
static int   pending_role = ROLE_READER;

void
agentinit(void)
{
  initlock(&agent_q_lock, "agent_q");
}

static int
str_eq(const char *a, const char *b)
{
  while(*a && *b){ if(*a != *b) return 0; a++; b++; }
  return *a == *b;
}

static int
parse_uint(const char *s, int *out)
{
  int v = 0, any = 0;
  while(*s >= '0' && *s <= '9'){ v = v*10 + (*s - '0'); any = 1; s++; }
  if(!any) return -1;
  *out = v;
  return (*s == 0) ? 0 : -1;
}

static void
copy_line(char *dst, const char *src, int max)
{
  int i = 0;
  while(i < max-1 && src[i]){ dst[i] = src[i]; i++; }
  dst[i] = 0;
}

static int
parse_role(const char *s)
{
  if(str_eq(s, "reader")) return ROLE_READER;
  if(str_eq(s, "writer")) return ROLE_WRITER;
  if(str_eq(s, "admin"))  return ROLE_ADMIN;
  return -1;
}

static ushort
cmd_to_acl(const char *cmd)
{
  if(str_eq(cmd, "PRINT")) return ACL_PRINT;
  if(str_eq(cmd, "CHAT"))  return ACL_CHAT;
  if(str_eq(cmd, "READ"))  return ACL_READ;
  if(str_eq(cmd, "LS"))    return ACL_LS;
  if(str_eq(cmd, "PS"))    return ACL_PS;
  if(str_eq(cmd, "WRITE")) return ACL_WRITE;
  if(str_eq(cmd, "KILL"))  return ACL_KILL;
  if(str_eq(cmd, "NICE"))  return ACL_NICE;
  return 0;   // unknown cmd — no ACL applied (printed as unknown by exec_cmd)
}

// ---------- fs / proc 보조 핸들러 ----------
// 호출자가 ACL 검사를 통과시킨 뒤에만 도달한다. process 컨텍스트에서만 호출.

static void
do_write(char *arg)
{
  // arg = "<path>|<content>"
  char *bar = arg;
  while(*bar && *bar != '|') bar++;
  if(*bar != '|'){ printf("[agent] WRITE missing content separator\n"); return; }
  *bar = 0;
  char *path = arg;
  char *content = bar + 1;
  int clen = 0;
  while(content[clen]) clen++;

  begin_op();
  struct inode *ip = namei(path);
  if(ip == 0){
    ip = create(path, T_FILE, 0, 0);   // returns with ilock held
    if(ip == 0){ end_op(); printf("[agent] WRITE: cannot create %s\n", path); return; }
  } else {
    ilock(ip);
    if(ip->type != T_FILE){
      iunlockput(ip); end_op();
      printf("[agent] WRITE: not a file %s\n", path);
      return;
    }
    itrunc(ip);
  }
  int n = writei(ip, 0, (uint64)content, 0, clen);
  iunlockput(ip);
  end_op();
  printf("[agent] wrote %d bytes to %s\n", n, path);
}

static void
do_read(char *arg)
{
  begin_op();
  struct inode *ip = namei(arg);
  if(ip == 0){ end_op(); printf("[agent] READ: not found %s\n", arg); return; }
  ilock(ip);
  if(ip->type != T_FILE){
    iunlockput(ip); end_op();
    printf("[agent] READ: not a file %s\n", arg);
    return;
  }
  char buf[256];
  int n = readi(ip, 0, (uint64)buf, 0, sizeof(buf)-1);
  uint sz = ip->size;
  iunlockput(ip);
  end_op();

  if(n < 0) n = 0;
  buf[n] = 0;
  printf("[agent] %s\n", buf);
  if(sz > (uint)n) printf("[agent] ... (truncated; size=%d)\n", sz);
}

static void
do_ls(char *arg)
{
  static char root_path[] = "/";
  char *path = (*arg == 0) ? root_path : arg;
  begin_op();
  struct inode *ip = namei(path);
  if(ip == 0){ end_op(); printf("[agent] LS: not found %s\n", path); return; }
  ilock(ip);
  if(ip->type != T_DIR){
    iunlockput(ip); end_op();
    printf("[agent] LS: not a dir %s\n", path);
    return;
  }
  struct dirent de;
  for(uint off = 0; off < ip->size; off += sizeof(de)){
    if(readi(ip, 0, (uint64)&de, off, sizeof(de)) != sizeof(de)) break;
    if(de.inum == 0) continue;
    char name[DIRSIZ+1];
    memmove(name, de.name, DIRSIZ);
    name[DIRSIZ] = 0;
    printf("[agent] %s (inum=%d)\n", name, de.inum);
  }
  iunlockput(ip);
  end_op();
}

static void
do_ps(void)
{
  static const char *states[] = {"UNUSED","USED","SLEEP","RUNBLE","RUNNING","ZOMBIE"};
  struct proc *p;
  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->state != UNUSED){
      const char *st = (p->state >= 0 && p->state < 6) ? states[p->state] : "?";
      printf("[agent] pid=%d %s %s prio=%d\n", p->pid, p->name, st, p->priority);
    }
    release(&p->lock);
  }
}

// ---------- exec_cmd : ACL gate + cmd routing ----------
// arg 는 mutable (NICE/WRITE 가 내부에서 '\0' 삽입). 빈 인자라면 빈 문자열 포인터.
static void
exec_cmd(int role, const char *cmd, char *arg)
{
  ushort bit = cmd_to_acl(cmd);
  if(bit && (acl_mask[role] & bit) == 0){
    printf("[deny] role=%s cmd=%s\n", role_name[role], cmd);
    return;
  }

  if(str_eq(cmd, "PRINT")){
    printf("[agent] %s\n", arg);
    return;
  }
  if(str_eq(cmd, "CHAT")){
    printf("[chat] %s\n", arg);
    return;
  }
  if(str_eq(cmd, "READ")) { do_read(arg);  return; }
  if(str_eq(cmd, "LS"))   { do_ls(arg);    return; }
  if(str_eq(cmd, "PS"))   { do_ps();       return; }
  if(str_eq(cmd, "WRITE")){ do_write(arg); return; }

  if(str_eq(cmd, "KILL")){
    int pid;
    if(parse_uint(arg, &pid) < 0){ printf("[agent] bad KILL pid: %s\n", arg); return; }
    if(pid <= 2){
      printf("[agent] DENY kill pid=%d (init/sh protected)\n", pid);
      return;
    }
    if(kkill(pid) == 0) printf("[agent] killed pid=%d\n", pid);
    else                printf("[agent] no such pid=%d\n", pid);
    return;
  }

  if(str_eq(cmd, "NICE")){
    char *colon = arg;
    while(*colon && *colon != ':') colon++;
    if(*colon != ':'){ printf("[agent] bad NICE arg: %s\n", arg); return; }
    *colon = 0;
    int pid, prio;
    if(parse_uint(arg, &pid) < 0 || parse_uint(colon+1, &prio) < 0){
      printf("[agent] bad NICE numbers\n"); return;
    }
    if(prio < 0 || prio > 20){ printf("[agent] bad prio %d\n", prio); return; }

    struct proc *p;
    for(p = proc; p < &proc[NPROC]; p++){
      acquire(&p->lock);
      if(p->pid == pid){
        p->priority = prio;
        release(&p->lock);
        printf("[agent] pid=%d prio=%d\n", pid, prio);
        return;
      }
      release(&p->lock);
    }
    printf("[agent] no such pid=%d\n", pid);
    return;
  }

  printf("[agent] unknown cmd '%s'\n", cmd);
}

// 와이어 명령("CMD|arg" 또는 "CMD")을 분리해 곧장 실행. 새 버퍼·재귀 없음.
static void
exec_wire(int role, char *cmdarg)
{
  char *bar = cmdarg;
  while(*bar && *bar != '|') bar++;
  char *arg;
  if(*bar == '|'){
    *bar = 0;
    arg = bar + 1;
  } else {
    arg = bar;   // points to '\0' — empty arg
  }
  exec_cmd(role, cmdarg, arg);
}

// ---------- 오케스트레이션 메타-명령 핸들러 ----------
// 각각 별도 함수로 둠으로써 agent_dispatch_now 의 단일 프레임에 1KB+ 버퍼들이
// 누적되는 것을 막는다 (4 KB 커널 스택 보호).

static void
handle_ask(int role, char *arg)
{
  int plen = 0; while(arg[plen]) plen++;
  if(plen == 0) return;

  // CACHE_VAL = 1024 (cache.c). +1 for null-terminator that exec_wire needs.
  char cached[1025];

  int clen = cache_get_exact(arg, plen, cached, sizeof(cached) - 1);
  if(clen >= 0){
    if(clen >= (int)sizeof(cached)) clen = sizeof(cached) - 1;
    cached[clen] = 0;
    printf("[cache] HIT\n");
    exec_wire(role, cached);
    return;
  }

  // 의미 매치는 CHAT|/PRINT| 응답에만 적용 — WRITE/KILL/NICE 가 paraphrase 로
  // 우연히 hit 되어 부작용을 재실행하는 사고를 막는다.
  int score = 0;
  clen = cache_get_semantic(arg, plen, cached, sizeof(cached) - 1, &score);
  if(clen >= 0){
    if(clen >= (int)sizeof(cached)) clen = sizeof(cached) - 1;
    cached[clen] = 0;
    if(strncmp(cached, "CHAT|", 5) == 0 ||
       strncmp(cached, "PRINT|", 6) == 0){
      printf("[cache] SEMANTIC HIT score=%d/64\n", score);
      exec_wire(role, cached);
      return;
    }
    // 부작용 action — 무시하고 LLM 새로 호출
  }

  copy_line(pending_prompt, arg, AGENT_LINE_MAX);
  pending_len = plen;
  pending_role = role;
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
  exec_wire(pending_role, arg);
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

// ---------- 인터럽트-안전 enqueue ----------
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

// ---------- 프로세스 컨텍스트 dequeue + dispatch ----------
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

void
agent_dispatch_now(char *line)
{
  if(!(line[0]=='R' && line[1]=='E' && line[2]=='Q' && line[3]=='|')) return;
  char *cmd = line + 4;

  // Optional "agent:<role>|" prefix
  int role = ROLE_READER;
  if(cmd[0]=='a' && cmd[1]=='g' && cmd[2]=='e' && cmd[3]=='n' && cmd[4]=='t' && cmd[5]==':'){
    char *role_str = cmd + 6;
    char *rbar = role_str;
    while(*rbar && *rbar != '|') rbar++;
    if(*rbar != '|'){ printf("[agent] malformed role prefix\n"); return; }
    *rbar = 0;
    int r = parse_role(role_str);
    if(r < 0){ printf("[agent] unknown role: %s\n", role_str); return; }
    role = r;
    cmd = rbar + 1;
  }

  // split at first '|'
  char *bar = cmd;
  while(*bar && *bar != '|') bar++;
  char *arg;
  if(*bar == '|'){
    *bar = 0;
    arg = bar + 1;
  } else {
    arg = bar;   // '\0' — no-arg cmds like PS
  }

  // ASK / LLM_RESP / CACHE_GET / CACHE_SET 는 오케스트레이션 메타-명령 —
  // ACL을 우회한다 (자체 ACL은 LLM_RESP 가 unwrap 한 진짜 cmd 에 적용).
  // 각각 별도 함수로 분리 — agent_dispatch_now 의 단일 스택 프레임에 1KB+ 버퍼를
  // 모아두면 agent_drain 의 2KB local 과 합산되어 4 KB 커널 스택을 넘는다
  // (Phase 3 bug 4 패턴 재발 — 핸들러 추출로 회피).
  if(str_eq(cmd, "ASK"))      { handle_ask(role, arg);   return; }
  if(str_eq(cmd, "LLM_RESP")) { handle_llm_resp(arg);    return; }
  if(str_eq(cmd, "CACHE_GET")){ handle_cache_get(arg);   return; }
  if(str_eq(cmd, "CACHE_SET")){ handle_cache_set(arg);   return; }

  // 일반 명령 — ACL 적용
  exec_cmd(role, cmd, arg);
}
