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
// v2 노트 — yield-polling → sleep/wakeup 전환:
//   v1 은 매 iteration 에 yield() 로 CPU 양보 → 호스트 응답 polling. 이 패턴
//   에서 kerneltrap panic (scause=0xf, sp 가 유저 영역값) 이 관찰돼 비활성화.
//   v2 는 표준 sleep(&ticks, &confirm_lock) 으로 — clockintr() 가 매 tick
//   `wakeup(&ticks)` 를 호출하므로 자동으로 ≤10ms latency 로 깨어남 (timeout
//   도 같은 매커니즘으로 동작). confirm_resolve() 가 즉시 `wakeup(&ticks)`
//   를 추가 호출해 sub-tick latency 보장.
//
// v1 의 제약은 그대로: 한 번에 *하나만* 처리 — 다른 confirm 이 이미 대기
// 중이면 새 요청은 즉시 거부. 미래에 큐로 확장 가능.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

#define CONFIRM_TIMEOUT_TICKS 150   // 약 15초 (1 tick ≈ 100ms). 사용자가
                                    // prompt 를 읽고 결정할 충분한 시간.

static struct spinlock confirm_lock;
static int confirm_pending_pid;     // 대기 중인 syscall 의 pid (0 = idle)
static int confirm_result;          // -1=pending, 1=allow, 0=deny
static char confirm_wait_chan;      // sleep 채널 (전용 — &ticks 와 분리)

// extern from trap.c — 시스템 ticks 카운터 (timeout 체크)
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
// 구현 노트: `sleep(&ticks, &confirm_lock)` 를 사용 — clockintr() 가 매 tick
// `wakeup(&ticks)` 를 호출하므로 ≤10ms 마다 자연 wake. wake 후 timeout 검사 +
// confirm_result 검사 → 만족하면 종료, 아니면 다시 sleep. confirm_resolve()
// 도 같은 채널로 즉시 wakeup → sub-tick latency.
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
  acquire(&confirm_lock);
  for(;;){
    if(confirm_result != -1){
      rc = (confirm_result == 1) ? 1 : 0;
      break;
    }

    // timeout 체크 — tickslock 짧게 잡고 즉시 해제
    acquire(&tickslock);
    uint elapsed = ticks - t0;
    release(&tickslock);
    if(elapsed >= CONFIRM_TIMEOUT_TICKS) break;  // rc=0 (deny by timeout)

    // 전용 채널 sleep — clockintr 의 confirm_tick() 가 매 tick wakeup.
    sleep(&confirm_wait_chan, &confirm_lock);
  }
  confirm_pending_pid = 0;
  confirm_result = -1;
  release(&confirm_lock);
  return rc;
}

// timer-tick broadcast — clockintr() 가 매 tick 호출. pending 이 있을 때만
// wakeup 하여 불필요한 lock contention 회피.
void
confirm_tick(void)
{
  if(confirm_pending_pid != 0)
    wakeup(&confirm_wait_chan);
}

// 호스트 응답 도착 — agentcmd.c 의 CONFIRM_RES 와이어 핸들러가 호출.
// 대기 중인 pid 와 일치할 때만 결과 set. wakeup 으로 즉시 깨워 반응성 ↑.
void
confirm_resolve(int pid, int allow)
{
  acquire(&confirm_lock);
  if(confirm_pending_pid == pid && confirm_result == -1){
    confirm_result = allow ? 1 : 0;
  }
  release(&confirm_lock);
  wakeup(&confirm_wait_chan);
}
