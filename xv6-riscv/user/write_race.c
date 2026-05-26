// Ported into SeungBeom from commit 76b2737 (Se-Joong). Uses dispatch().
//
// write_race — 동일 파일에 동시에 wire WRITE 를 시도하는 4 자식 프로세스.
//
// 의도: inode sleeplock 으로 인한 직렬화를 시각화. 자식 4개가 거의 같은 tick
// 에서 dispatch() 를 호출하지만, ilock(ip) 가 같은 inode 에 대해 한 명만
// 허용하므로 end tick 은 계단처럼 증가한다. itrunc 가 매 트랜잭션에서 파일을
// 0 으로 잘라 다시 쓰므로 최종 내용은 마지막 writer 의 content 만 남는다.

#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define WRITERS 4
// 파일명을 일반적인 /shared.txt 가 아닌 wr_ prefix 로 — agentd 가 다른 경로로
// 같은 이름을 만지는 통합 충돌 가능성을 차단.
#define SHARED  "/wr_shared.txt"

static int
itoa(int n, char *out)
{
  if(n == 0){ out[0] = '0'; return 1; }
  char tmp[16];
  int l = 0;
  while(n > 0){ tmp[l++] = '0' + n%10; n /= 10; }
  for(int i = 0; i < l; i++) out[i] = tmp[l-1-i];
  return l;
}

int
main(void)
{
  printf("=== write_race: %d writers contending on %s ===\n", WRITERS, SHARED);
  int t_launch = uptime();

  for(int i = 0; i < WRITERS; i++){
    int pid = fork();
    if(pid < 0){ printf("fork failed\n"); exit(1); }
    if(pid == 0){
      // wire = "REQ|agent:writer|WRITE|/shared.txt:pid=<P>-" + 'X'×N (긴 content
      // 로 ilock 점유 시간 늘려 sleeplock 직렬화가 tick 단위로 보이도록).
      // SeungBeom agentd do_write wire는 WRITE|<file>:<data> (콜론 구분).
      char line[1024];
      char *p = line;
      const char *pfx = "REQ|agent:writer|WRITE|" SHARED ":pid=";
      while(*pfx) *p++ = *pfx++;
      char num[16];
      int nl = itoa(getpid(), num);
      for(int j = 0; j < nl; j++) *p++ = num[j];
      *p++ = '-';
      for(int k = 0; k < 700; k++) *p++ = 'X';
      *p = 0;

      int t0 = uptime();
      dispatch(line);
      int t1 = uptime();
      printf("  [pid=%d] start=%d end=%d delta=%d\n",
             getpid(), t0, t1, t1 - t0);
      exit(0);
    }
  }

  for(int i = 0; i < WRITERS; i++) wait(0);
  int t_done = uptime();
  printf("=== all writers exited — wall=%d ticks\n", t_done - t_launch);

  // dispatch() 는 큐에 enqueue 만 하고 즉시 리턴하므로, 자식 종료 시점에
  // agentd 는 아직 마지막 WRITE 트랜잭션을 처리 중일 수 있다.
  // user-space 의 sleep 시스템콜이 없으므로 uptime() polling 으로 백오프 +
  // 재시도하여 inode 충돌이 풀릴 때까지 기다린다.
  int fd = -1;
  for(int retry = 0; retry < 30 && fd < 0; retry++){
    fd = open(SHARED, 0);
    if(fd < 0){
      int t_wait_until = uptime() + 2;        // 약 200ms 백오프
      while(uptime() < t_wait_until) { }
    }
  }
  if(fd < 0){ printf("can't open %s\n", SHARED); exit(1); }
  char buf[1024];
  int n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if(n < 0){ printf("read failed\n"); exit(1); }
  buf[n] = 0;
  // content 앞 32 byte 만 보여줌 — 어느 자식의 pid 가 살아남았는지 확인용
  int show = (n > 40) ? 40 : n;
  char head[64];
  for(int i = 0; i < show; i++) head[i] = buf[i];
  head[show] = 0;
  printf("=== final %s (%d bytes) head: %s...\n", SHARED, n, head);
  exit(0);
}
