/*
 * agentd.c — the jailed LLM agent runtime.
 *
 * Started automatically by init at boot. It immediately confines itself to
 * a jail directory via jail(), then loops receiving LLM-issued commands
 * (forwarded by the kernel from the "REQ|" serial channel) and executing
 * them *inside* that jail. Because the process is sandboxed:
 *   - every file path it touches is chroot'd to the jail directory;
 *   - exec/kill/mknod system calls are refused by the kernel.
 *
 * Human shell input never reaches here — only commands the LLM produced.
 *
 * F7: the function table below is the agent's whitelist.
 * F8: each function carries a priority the LLM may retune (SETPRIO); the
 *     agent runs each command at its function's priority.
 */
#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"
#include "kernel/param.h"
#include "user/user.h"
#include "kernel/deny.h"
#include "kernel/procinfo.h"

#define JAIL "/agentbox"

struct fn {
  char *name;
  int   allowed;
  int   priority;
  char *usage;       // how the LLM calls it (shown by HELP)
};

static struct fn table[] = {
  { "PRINT",   1, 10, "PRINT|<msg>" },
  { "CHAT",    1, 10, "CHAT|<msg>" },
  { "READ",    1,  8, "READ|<file>" },
  { "WRITE",   1, 12, "WRITE|<file>:<text>" },
  { "LS",      1,  8, "LS|" },
  { "NICE",    1,  5, "NICE|<pid>:<prio 0..20>" },
  { "LIST",    1,  0, "LIST|" },
  { "SETPRIO", 1,  5, "SETPRIO|<FN>:<prio 0..20>" },
  { "PS",      1,  8, "PS|" },
  { "HELP",    1,  0, "HELP|" },
  { "SPAWN",   1, 10, "SPAWN|<bin>|<argv space-separated>" },
};
#define NFN ((int)(sizeof(table) / sizeof(table[0])))

static int
streq(const char *a, const char *b)
{
  while(*a && *b){ if(*a != *b) return 0; a++; b++; }
  return *a == *b;
}

// parse a decimal int; whole string must be consumed
static int
parsei(const char *s, int *out)
{
  int v = 0, any = 0, neg = 0;
  if(*s == '-'){ neg = 1; s++; }
  while(*s >= '0' && *s <= '9'){ v = v*10 + (*s - '0'); any = 1; s++; }
  if(!any || *s)
    return -1;
  *out = neg ? -v : v;
  return 0;
}

// Wire payload unescape — agent.py's `_wire_escape` swaps embedded newlines
// for the literal two-char sequence "\n" so the wire stays single-line.
// Walk the buffer in-place: every '\' + 'n' collapses into a real '\n'; every
// other '\' is passed through unchanged. Output length ≤ input length.
static void
unescape_inplace(char *s)
{
  char *r = s;
  char *w = s;
  while(*r){
    if(*r == '\\' && *(r+1) == 'n'){
      *w++ = '\n';
      r += 2;
    } else {
      *w++ = *r++;
    }
  }
  *w = 0;
}

// ───────────────────────── command handlers ─────────────────────────

static void
do_print(char *arg)
{
  unescape_inplace(arg);
  printf("[agentd] %s\n", arg);
}

// CHAT <text> — natural-language reply. The kernel forwards cache hits of
// "CHAT|..." and the LLM's final answer (via LLM_RESP) through this path.
static void
do_chat(char *arg)
{
  unescape_inplace(arg);
  printf("[chat] %s\n", arg);
}

// READ <path> — print a file's contents (path is chroot'd to the jail)
static void
do_read(char *arg)
{
  int fd = open(arg, O_RDONLY);
  if(fd < 0){
    printf("[agentd] READ: '%s' not reachable inside jail\n", arg);
    return;
  }
  char buf[128];
  int n;
  printf("[agentd] READ %s:\n", arg);
  while((n = read(fd, buf, sizeof(buf))) > 0)
    write(1, buf, n);
  close(fd);
}

// WRITE <path>:<data> — create/overwrite a file inside the jail
static void
do_write(char *arg)
{
  char *c = arg;
  while(*c && *c != ':') c++;
  if(*c != ':'){ printf("[agentd] WRITE: expected <file>:<data>\n"); return; }
  *c = 0;
  char *data = c + 1;
  unescape_inplace(data);

  int fd = open(arg, O_CREATE | O_RDWR);
  if(fd < 0){
    printf("[agentd] WRITE: '%s' not writable inside jail\n", arg);
    return;
  }
  int len = 0;
  while(data[len]) len++;
  write(fd, data, len);
  close(fd);
  printf("[agentd] WROTE %d bytes to %s\n", len, arg);
}

// LS — list the files in the sandbox directory, with sizes
static void
do_ls(char *arg)
{
  int fd = open(".", O_RDONLY);
  if(fd < 0){
    printf("[agentd] LS: cannot open sandbox directory\n");
    return;
  }
  struct dirent de;
  struct stat st;
  char name[DIRSIZ + 1];
  int count = 0;

  printf("[agentd] sandbox file list:\n");
  while(read(fd, &de, sizeof(de)) == sizeof(de)){
    if(de.inum == 0)
      continue;
    memmove(name, de.name, DIRSIZ);
    name[DIRSIZ] = 0;
    if(streq(name, ".") || streq(name, ".."))
      continue;
    if(stat(name, &st) < 0)
      printf("[agentd]   %s\n", name);
    else
      printf("[agentd]   %s (%d bytes)\n", name, (int)st.size);
    count++;
  }
  close(fd);
  if(count == 0)
    printf("[agentd]   (sandbox is empty)\n");
}

// NICE <pid>:<prio> — adjust a process's CFS priority
static void
do_nice(char *arg)
{
  char *c = arg;
  while(*c && *c != ':') c++;
  if(*c != ':'){ printf("[agentd] NICE: expected <pid>:<prio>\n"); return; }
  *c = 0;
  int pid, prio;
  if(parsei(arg, &pid) < 0 || parsei(c+1, &prio) < 0){
    printf("[agentd] NICE: bad numbers\n");
    return;
  }
  if(setpriority(pid, prio) < 0)
    printf("[agentd] NICE: denied (pid=%d prio=%d)\n", pid, prio);
  else
    printf("[agentd] pid=%d prio=%d\n", pid, prio);
}

// True if name appears in the kernel deny-list snapshot (newline-separated).
static int
in_snapshot(const char *snap, const char *name)
{
  for(const char *s = snap; *s; ){
    const char *n = name, *t = s;
    while(*n && *t != '\n' && *t && *n == *t){ n++; t++; }
    if(*n == 0 && (*t == '\n' || *t == 0))
      return 1;
    while(*s && *s != '\n') s++;     // advance to the next line
    if(*s == '\n') s++;
  }
  return 0;
}

// LIST — report the agent's whitelist (F7) and per-function priority (F8).
// Access reflects EFFECTIVE policy: a tool the human added to the kernel deny
// list (via denyctl) shows DENY(kernel) — it never reaches this process.
static void
do_list(char *arg)
{
  char snap[DENY_MAX * DENY_NAMELEN];
  int n = get_deny(snap, sizeof(snap));
  if(n < 0) n = 0;
  snap[n] = 0;

  printf("[agentd] agent functions (name | access | priority):\n");
  for(int i = 0; i < NFN; i++){
    const char *acc = in_snapshot(snap, table[i].name) ? "DENY(kernel)"
                      : (table[i].allowed ? "ALLOW" : "DENY");
    printf("[agentd]   %s\t%s\tprio=%d\n", table[i].name, acc, table[i].priority);
  }
}

// SETPRIO <FN>:<prio> — F8: the LLM retunes a function's priority
static void
do_setprio(char *arg)
{
  char *c = arg;
  while(*c && *c != ':') c++;
  if(*c != ':'){ printf("[agentd] SETPRIO: expected <FN>:<prio>\n"); return; }
  *c = 0;
  int prio;
  if(parsei(c+1, &prio) < 0 || prio < 0 || prio > 20){
    printf("[agentd] SETPRIO: priority must be 0..20\n");
    return;
  }
  for(int i = 0; i < NFN; i++){
    if(streq(table[i].name, arg)){
      table[i].priority = prio;
      printf("[agentd] %s priority set to %d\n", arg, prio);
      return;
    }
  }
  printf("[agentd] SETPRIO: no such function '%s'\n", arg);
}

// PS — report the live process table so the agent can pick a pid for NICE.
static void
do_ps(char *arg)
{
  static const char *st[] = {
    "unused", "used", "sleep", "runble", "run", "zombie"
  };
  struct procinfo pi[NPROC];
  int n = procinfo(pi, NPROC);
  if(n < 0){
    printf("[agentd] PS: procinfo failed\n");
    return;
  }
  printf("[agentd] processes (pid | state | prio | name):\n");
  for(int i = 0; i < n; i++){
    const char *s = (pi[i].state >= 0 && pi[i].state < 6) ? st[pi[i].state] : "?";
    printf("[agentd]   %d\t%s\t%d\t%s%s%s\n",
           pi[i].pid, s, pi[i].priority, pi[i].name,
           pi[i].priority < 0 ? " [K]" : "", pi[i].is_agent ? " [A]" : "");
  }
}

// HELP — self-describing command catalog: how to call each tool (usage),
// whether it's allowed, and the priority it runs at. Lets the LLM discover
// the command set and argument formats at runtime.
static void
do_help(char *arg)
{
  printf("[agentd] commands (usage | access | priority):\n");
  for(int i = 0; i < NFN; i++)
    printf("[agentd]   %s\t%s\tprio=%d\n",
           table[i].usage, table[i].allowed ? "ALLOW" : "DENY",
           table[i].priority);
}

// SPAWN <bin>|<argv space-separated> — fork+exec a binary inside the sandbox.
// agentd 가 직접 exec 호출하면 자기 자신이 죽음 → 반드시 fork. 자식이
// is_agent=1 을 상속 (proc.c fork) 하므로 exec() 가 confirm-escape 게이트를
// trip → 호스트 사용자의 y/N 응답 후 실제 exec 또는 거부.
//
// 와이어 포맷: "<bin>|<argv0> <argv1> ..."  (argv 가 공백 분리). argv 가 비면
// argv = [bin].  argv 최대 8 개 (간단 한계 — 대부분 셸 명령 충분).
static void
do_spawn(char *arg)
{
  // split bin | argv
  char *c = arg;
  while(*c && *c != '|') c++;
  if(*c != '|'){
    // 무 | 인자 = argv 가 비어 있는 형태 (bin 만). argv = [bin].
    char *argv[2] = { arg, 0 };
    int pid = fork();
    if(pid < 0){ printf("[agentd] SPAWN: fork failed\n"); return; }
    if(pid == 0){ exec(arg, argv); printf("[agentd] SPAWN: exec %s failed\n", arg); exit(1); }
    int st = 0; wait(&st);
    printf("[agentd] SPAWN %s done (status=%d)\n", arg, st);
    return;
  }
  *c = 0;
  char *bin  = arg;
  char *args = c + 1;

  // tokenize argv on space
  char *argv[9];
  int argc = 0;
  argv[argc++] = bin;          // argv[0] convention
  char *t = args;
  while(*t && argc < 8){
    while(*t == ' ') t++;
    if(!*t) break;
    argv[argc++] = t;
    while(*t && *t != ' ') t++;
    if(*t == ' '){ *t = 0; t++; }
  }
  argv[argc] = 0;

  int pid = fork();
  if(pid < 0){ printf("[agentd] SPAWN: fork failed\n"); return; }
  if(pid == 0){
    // child — is_agent / jail_root inherited (proc.c fork). exec trips
    // confirm-escape per syscall.c agent_blocked branch.
    exec(bin, argv);
    printf("[agentd] SPAWN: exec %s failed\n", bin);
    exit(1);
  }
  int st = 0;
  wait(&st);
  printf("[agentd] SPAWN %s done (status=%d)\n", bin, st);
}

// ───────────────────────────── dispatch ─────────────────────────────

static void
execute(char *line)
{
  // line = "REQ|<CMD>|<arg>"
  if(!(line[0]=='R' && line[1]=='E' && line[2]=='Q' && line[3]=='|'))
    return;
  char *cmd = line + 4;
  char *bar = cmd;
  while(*bar && *bar != '|') bar++;
  if(*bar != '|'){ printf("[agentd] malformed: %s\n", line); return; }
  *bar = 0;
  char *arg = bar + 1;

  for(int i = 0; i < NFN; i++){
    if(streq(cmd, table[i].name)){
      if(!table[i].allowed){
        printf("[agentd] DENY '%s' (not whitelisted)\n", cmd);
        return;
      }
      // F8: run this function's work at its (LLM-tunable) priority.
      setpriority(getpid(), table[i].priority);

      if(streq(cmd, "PRINT"))        do_print(arg);
      else if(streq(cmd, "CHAT"))    do_chat(arg);
      else if(streq(cmd, "READ"))    do_read(arg);
      else if(streq(cmd, "WRITE"))   do_write(arg);
      else if(streq(cmd, "LS"))      do_ls(arg);
      else if(streq(cmd, "NICE"))    do_nice(arg);
      else if(streq(cmd, "LIST"))    do_list(arg);
      else if(streq(cmd, "SETPRIO")) do_setprio(arg);
      else if(streq(cmd, "PS"))      do_ps(arg);
      else if(streq(cmd, "HELP"))    do_help(arg);
      else if(streq(cmd, "SPAWN"))   do_spawn(arg);
      return;
    }
  }
  printf("[agentd] unknown cmd '%s'\n", cmd);
}

// jail 진입 직전에 자주 쓰는 user 바이너리들을 /agentbox/ 로 hard-link.
// 이렇게 해야 spawn'd 자식 (jail 안) 의 exec 가 binary 를 lookup 할 때
// namex() 가 jail_root 안에서 찾을 수 있다. /agentbox 가 비어 있으면 모든
// exec 시도가 -1 — 사용자 입장에선 "echo 가 안 돌아간다" 처럼 보임.
//
// 멱등 — link() 가 이미 있는 dst 에 대해 -1 반환하지만 무해 (이전 부팅의
// fs.img 가 그대로 살아 있을 수 있음).
static void
populate_jail(void)
{
  static const char *bins[] = {
    "echo", "cat", "ls", "wc", "grep",
    "mkdir", "rm", "ln", "kill",
    "sh",            // LLM often picks `sh -c "..."` — needs to be reachable
    0
  };
  for(int i = 0; bins[i]; i++){
    char src[32], dst[64];
    int j = 0;
    src[j++] = '/';
    for(int k = 0; bins[i][k]; k++) src[j++] = bins[i][k];
    src[j] = 0;
    j = 0;
    for(int k = 0; JAIL[k]; k++) dst[j++] = JAIL[k];
    dst[j++] = '/';
    for(int k = 0; bins[i][k]; k++) dst[j++] = bins[i][k];
    dst[j] = 0;
    link(src, dst);   // ignore EEXIST — idempotent across reboots
  }
}

int
main(void)
{
  // Ensure the jail directory exists, populate it with the bin set the LLM
  // is allowed to spawn, then confine ourselves to it.
  // (mkdir + link must happen before jail() — afterward '/' is the jail root.)
  mkdir(JAIL);
  populate_jail();
  if(jail(JAIL) < 0){
    printf("[agentd] FATAL: jail(%s) failed\n", JAIL);
    exit(1);
  }
  printf("[agentd] jailed LLM agent runtime ready (root=%s)\n", JAIL);

  char line[256];
  while(agent_recv(line) >= 0)
    execute(line);

  exit(0);
}
