// confirm.c — Confirm-Escape: jail 의 위험 syscall (exec/kill/mknod) 차단을
// *무조건 거부* 에서 *호스트 사용자에게 확인 후 일회 허용/거부* 로 격상.
//
// 흐름:
//   1. 게스트의 syscall() 가 agent_blocked() 인 호출을 만나면, 직접 -1 을
//      반환하지 않고 confirm_request() 를 부른다.
//   2. confirm_request() 는 console 에 `CONFIRM_REQ|<pid>|<call>|<summary>`
//      한 줄을 인쇄 — 호스트의 agent.py 가 그걸 _reader 스레드에서 잡아
//      사용자에게 y/N 프롬프트를 띄움.
//   3. 사용자 응답이 `REQ|agent:host|CONFIRM_RES|<pid>|y` 또는 `|n` 으로
//      들어오면, agentcmd.c 의 와이어 핸들러가 confirm_resolve() 를 호출.
//   4. 게스트의 confirm_request() 가 wakeup 되어 결과를 syscall() 에 반환.
//   5. timeout (5초) 시 자동 거부.
//
// v1 의 제약: 한 번에 *하나만* 처리 — 다른 confirm 이 이미 대기 중이면
// 새 요청은 즉시 거부. 미래에 큐로 확장 가능.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

#define CONFIRM_TIMEOUT_TICKS 50    // 약 5초 (1 tick ≈ 100ms)

static struct spinlock confirm_lock;
static int confirm_pending_pid;     // 대기 중인 syscall 의 pid (0 = idle)
static int confirm_result;          // -1=pending, 1=allow, 0=deny

// extern from trap.c — 시스템 ticks 카운터 (tick polling 용)
extern uint ticks;
extern struct spinlock tickslock;

void
confirminit(void)
{
  initlock(&confirm_lock, "confirm");
  confirm_pending_pid = 0;
  confirm_result = -1;
}

// 동기 호출 — 호스트의 응답 또는 timeout 까지 대기.
// 반환: 1=allow, 0=deny.
//
// 구현 노트: xv6 의 sleep() 은 timeout 자체 지원 X. 단순 yield 기반
// polling 으로 — 매 라운드에 lock 잡아 검사, 다음 라운드는 yield() 로
// CPU 양보. 다른 프로세스 (특히 agentcmd 큐 처리) 가 진행할 수 있음.
int
confirm_request(int call_num, const char *summary)
{
  struct proc *p = myproc();

  acquire(&confirm_lock);
  if(confirm_pending_pid != 0){
    release(&confirm_lock);
    return 0;     // v1: 단일 pending — 동시 다발 시 즉시 거부
  }
  confirm_pending_pid = p->pid;
  confirm_result = -1;
  release(&confirm_lock);

  printf("CONFIRM_REQ|%d|%d|%s\n", p->pid, call_num, summary ? summary : "");

  acquire(&tickslock);
  uint t0 = ticks;
  release(&tickslock);

  int rc = 0;
  for(;;){
    acquire(&confirm_lock);
    if(confirm_result != -1){
      rc = confirm_result;
      release(&confirm_lock);
      break;
    }
    release(&confirm_lock);

    acquire(&tickslock);
    int elapsed = (int)(ticks - t0);
    release(&tickslock);
    if(elapsed >= CONFIRM_TIMEOUT_TICKS) break;   // timeout → rc=0 (deny)

    yield();    // CPU 양보 — scheduler 가 다른 RUNNABLE 프로세스 실행
  }

  acquire(&confirm_lock);
  confirm_pending_pid = 0;
  confirm_result = -1;
  release(&confirm_lock);
  return rc;
}

// 호스트 응답 도착 — agentcmd.c 의 CONFIRM_RES 와이어 핸들러가 호출.
// 대기 중인 pid 와 일치할 때만 결과 set. wakeup(&ticks) 로 즉시 깨워 반응성 ↑.
void
confirm_resolve(int pid, int allow)
{
  acquire(&confirm_lock);
  if(confirm_pending_pid == pid && confirm_result == -1){
    confirm_result = allow ? 1 : 0;
  }
  release(&confirm_lock);
  // 매 tick 깨어남 이미 보장됨 — 추가 wakeup 없어도 다음 tick 에 응답. 단
  // 즉시 반응을 위해 명시적 wakeup(&ticks) 가능하지만 다른 sleep 까지 깨워
  // 사이드 이펙트 — 생략.
}
