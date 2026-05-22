/*
 * denyctl.c — manage the F7 command deny list from the human shell.
 *
 * The deny list is the kernel's hard sandbox boundary: command names on it
 * never reach the jailed agent runtime (see kernel/agentcmd.c). It used to be
 * hardcoded to {KILL, EXEC}; now it is configurable like user input:
 *
 *   denyctl list            show the current deny list
 *   denyctl add <CMD>       deny <CMD>            (one-time, this session)
 *   denyctl rm  <CMD>       allow <CMD> again     (one-time, this session)
 *   denyctl reset           restore default {KILL, EXEC}
 *   denyctl save            persist current list to /denylist.conf (permanent)
 *   denyctl load            apply /denylist.conf to the kernel (run at boot)
 *
 * Permanent vs one-time: kernel changes (add/rm/reset) live only in RAM until
 * `save` writes them to disk; `load` (run by init at boot) reapplies them, so
 * a saved list survives reboot. Only non-agent processes may mutate the list,
 * so a jailed agent cannot edit its own boundary (enforced in sys_set_deny).
 */
#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"
#include "kernel/deny.h"

#define CONF    "/denylist.conf"
#define BUFSZ   (DENY_MAX * DENY_NAMELEN)

static void
usage(void)
{
  printf("usage: denyctl list | add <CMD> | rm <CMD> | reset | save | load\n");
}

static void
do_list(void)
{
  char buf[BUFSZ];
  int n = get_deny(buf, sizeof(buf));
  if(n < 0){
    printf("denyctl: get_deny failed\n");
    exit(1);
  }
  buf[n] = 0;
  printf("deny list (kernel hard boundary):\n");
  if(n == 0){
    printf("  (empty — nothing blocked)\n");
    return;
  }
  char *s = buf;
  while(*s){
    char *e = s;
    while(*e && *e != '\n') e++;
    if(*e) *e++ = 0;
    printf("  %s\n", s);
    s = e;
  }
}

static void
do_save(void)
{
  char buf[BUFSZ];
  int n = get_deny(buf, sizeof(buf));
  if(n < 0){
    printf("denyctl: get_deny failed\n");
    exit(1);
  }
  int fd = open(CONF, O_CREATE | O_TRUNC | O_WRONLY);
  if(fd < 0){
    printf("denyctl: cannot open %s for writing\n", CONF);
    exit(1);
  }
  if(n > 0 && write(fd, buf, n) != n){
    printf("denyctl: write to %s failed\n", CONF);
    close(fd);
    exit(1);
  }
  close(fd);
  printf("denyctl: saved deny list to %s (survives reboot)\n", CONF);
}

static void
do_load(void)
{
  int fd = open(CONF, O_RDONLY);
  if(fd < 0)
    return;            // no config file: keep kernel default. Silent (boot path).

  char buf[BUFSZ];
  int n = 0, r;
  while(n < (int)sizeof(buf) - 1 &&
        (r = read(fd, buf + n, sizeof(buf) - 1 - n)) > 0)
    n += r;
  close(fd);
  buf[n] = 0;

  // Make the kernel list exactly match the file: clear, then add each line.
  set_deny(DENY_CLEAR, 0);
  int added = 0;
  char *s = buf;
  while(*s){
    char *e = s;
    while(*e && *e != '\n') e++;
    if(*e) *e++ = 0;
    if(*s){                       // skip blank lines
      if(set_deny(DENY_ADD, s) == 0)
        added++;
    }
    s = e;
  }
  printf("denyctl: loaded %d entries from %s\n", added, CONF);
}

static void
do_mutate(int op, const char *cmd, const char *verb)
{
  if(set_deny(op, cmd) < 0){
    printf("denyctl: %s '%s' failed (list full, not present, or not permitted)\n",
           verb, cmd);
    exit(1);
  }
  printf("denyctl: %s '%s' (one-time — run 'denyctl save' to persist)\n", verb, cmd);
}

int
main(int argc, char *argv[])
{
  if(argc < 2){
    usage();
    exit(1);
  }
  char *sub = argv[1];

  if(strcmp(sub, "list") == 0){
    do_list();
  } else if(strcmp(sub, "reset") == 0){
    if(set_deny(DENY_RESET, 0) < 0){
      printf("denyctl: reset not permitted\n");
      exit(1);
    }
    printf("denyctl: reset to default {KILL, EXEC} (run 'denyctl save' to persist)\n");
  } else if(strcmp(sub, "save") == 0){
    do_save();
  } else if(strcmp(sub, "load") == 0){
    do_load();
  } else if(strcmp(sub, "add") == 0){
    if(argc < 3){ usage(); exit(1); }
    do_mutate(DENY_ADD, argv[2], "denied");
  } else if(strcmp(sub, "rm") == 0){
    if(argc < 3){ usage(); exit(1); }
    do_mutate(DENY_REMOVE, argv[2], "allowed");
  } else {
    usage();
    exit(1);
  }
  exit(0);
}
