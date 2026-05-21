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
#include "user/user.h"

#define JAIL "/agentbox"

struct fn {
  char *name;
  int   allowed;
  int   priority;
};

static struct fn table[] = {
  { "PRINT",   1, 10 },
  { "CHAT",    1, 10 },
  { "READ",    1,  8 },
  { "WRITE",   1, 12 },
  { "LS",      1,  8 },
  { "PS",      1,  6 },
  { "NICE",    1,  5 },
  { "LIST",    1,  0 },
  { "SETPRIO", 1,  5 },
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

// ───────────────────────── command handlers ─────────────────────────

static void
do_print(char *arg)
{
  printf("[agentd] %s\n", arg);
}

// CHAT <text> — natural-language response. agent.py forwards cache hits
// of "CHAT|..." as well as the LLM's final answer through this path.
static void
do_chat(char *arg)
{
  printf("[chat] %s\n", arg);
}

// PS — process list. xv6 does not yet expose process info via syscall to
// userspace (procdump() is kernel-only). Stub until a sys_proclist is
// added in a follow-up PR.
static void
do_ps(char *arg)
{
  printf("[agentd] PS: no userspace proc-list syscall yet; use kernel Ctrl-P\n");
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

// LIST — report the agent's whitelist (F7) and per-function priority (F8)
static void
do_list(char *arg)
{
  printf("[agentd] agent functions (name | access | priority):\n");
  for(int i = 0; i < NFN; i++)
    printf("[agentd]   %s\t%s\tprio=%d\n",
           table[i].name, table[i].allowed ? "ALLOW" : "DENY",
           table[i].priority);
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
      else if(streq(cmd, "PS"))      do_ps(arg);
      else if(streq(cmd, "NICE"))    do_nice(arg);
      else if(streq(cmd, "LIST"))    do_list(arg);
      else if(streq(cmd, "SETPRIO")) do_setprio(arg);
      return;
    }
  }
  printf("[agentd] unknown cmd '%s'\n", cmd);
}

int
main(void)
{
  // Ensure the jail directory exists, then confine ourselves to it.
  // (mkdir must happen before jail() — afterward '/' is the jail root.)
  mkdir(JAIL);
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
