// Ported into SeungBeom from commit 76b2737 (Se-Joong). Uses dispatch().
//
// agent_multi — 4 concurrent agent processes, each with a different role.
//
// This is the Tier-2 demonstration for project.md's "multiple concurrent LLM
// 'processes' (agents) with fair CPU/memory/tool-quota allocation". Each
// fork()ed child issues a fixed sequence of REQ|agent:<role>|... commands
// through the kernel agent dispatcher (via the sys_dispatch() syscall).
//
// What gets demonstrated:
//   (a) ACL is enforced per role — reader children's WRITE/KILL produce [deny]
//   (b) CHAT/PS work for everyone (allowed for all roles)
//   (c) CFS scheduler interleaves the four agents (vruntime + creation_tick)
//   (d) The shared /cache.bin underlies all of them
//
// Output mixes [chat], [agent], and [deny] lines from kernel printf, plus
// per-child "starting / done" markers from this program.

#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

// Append src to dst, return pointer to dst's new '\0' terminator. Enables
// `p = strput(strput(buf, "foo"), "bar");` style chaining.
static char *
strput(char *dst, const char *src)
{
  while(*src) *dst++ = *src++;
  *dst = 0;
  return dst;
}

// Build "REQ|agent:<role>|<cmd>" or "REQ|agent:<role>|<cmd>|<arg>", dispatch.
static void
send_req(const char *role, const char *cmd, const char *arg)
{
  char buf[256];
  char *p = buf;
  p = strput(p, "REQ|agent:");
  p = strput(p, role);
  *p++ = '|';
  p = strput(p, cmd);
  if(arg != 0){
    *p++ = '|';
    p = strput(p, arg);
  }
  *p = 0;
  dispatch(buf);
}

static void
child_main(int idx, const char *role)
{
  printf("  [agent %d role=%s] starting (pid=%d)\n", idx, role, getpid());

  // Build per-agent strings (chat msg, write path+content).
  char chat_msg[64];
  char *q = strput(chat_msg, "hello from agent ");
  *q++ = '0' + idx;
  *q = 0;

  char wpath[32];
  q = strput(wpath, "/multi_");
  *q++ = '0' + idx;
  q = strput(q, ".txt");

  char warg[96];
  q = strput(warg, wpath);
  *q++ = ':';                     // agentd do_write wire is WRITE|<file>:<data>
  q = strput(q, "agent ");
  *q++ = '0' + idx;
  q = strput(q, " wrote this");

  // Sequence: CHAT (all ok), WRITE (reader denied), PS (all ok), KILL (only admin).
  // Small work between calls to encourage CFS interleaving.
  send_req(role, "CHAT", chat_msg);
  for(volatile int i = 0; i < 50000; i++);   // burn some CPU

  send_req(role, "WRITE", warg);
  for(volatile int i = 0; i < 50000; i++);

  send_req(role, "PS", 0);
  for(volatile int i = 0; i < 50000; i++);

  send_req(role, "KILL", "999");             // pid 999 won't exist either way
  for(volatile int i = 0; i < 50000; i++);

  printf("  [agent %d role=%s] done\n", idx, role);
}

int
main(int argc, char *argv[])
{
  // Mix of roles: 2 readers (least privileged), 1 writer, 1 admin.
  const char *roles[4] = {"reader", "writer", "reader", "admin"};

  printf("=== Multi-agent demo: 4 concurrent agents, mixed roles ===\n");
  printf("    reader[0]  writer[1]  reader[2]  admin[3]\n\n");

  for(int i = 0; i < 4; i++){
    int pid = fork();
    if(pid < 0){ printf("fork failed\n"); exit(1); }
    if(pid == 0){
      child_main(i, roles[i]);
      exit(0);
    }
  }
  for(int i = 0; i < 4; i++) wait(0);

  printf("\n=== done — kernel log contains [chat]/[agent]/[deny] interleaved ===\n");
  exit(0);
}
