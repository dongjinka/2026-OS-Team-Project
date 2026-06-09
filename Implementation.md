# Implementation — OS for LLM (Direction A)

xv6-riscv 위에서 LLM(Upstage Solar Pro)을 지휘하는 **에이전트 런타임**.
`Project_requirements.md` Direction A에 해당하며, AIOS 논문의 세 핵심 컴포넌트를
xv6 커널에 직접 매핑했다.

| AIOS 컴포넌트       | 본 프로젝트 구현                                                |
| ------------------- | --------------------------------------------------------------- |
| Agent Scheduler     | **CFS 스케줄러** — Linux `sched_prio_to_weight` 가중치 기반     |
| Tool Manager        | **커널 명령 큐 + jailed `agentd` 워커** — REQ\| 프로토콜        |
| LLM Kernel Bridge   | **`agent.py`** — Solar API ↔ QEMU TCP 시리얼, ReAct 루프        |

> 본 문서는 **현재 구현된 아키텍처**를 코드 기준으로 기술한다. 작업 진행 상태·
> 남은 일·평가 지표는 [plan.md](plan.md)를, 변경 이력은 [CHANGELOG.md](CHANGELOG.md)를
> 참고할 것.

---

## 0. 시스템 아키텍처

```
 ┌─────────────────┐   자연어 입력
 │  사용자 (REPL)  │ ─────────────►
 └────────┬────────┘
          │
          ▼
 ┌────────────────────┐ HTTPS  ┌────────────────────────┐
 │  agent.py          │───────►│  Upstage Solar Pro     │
 │  (ReAct 루프)      │◄───────│  (api.upstage.ai/v1)   │
 │  • 대화 메모리     │  JSON  └────────────────────────┘
 │  • REQ| 와이어     │
 │  • OBS 마커 캡처   │
 └────────┬───────────┘
          │ TCP 4444 (QEMU 시리얼)
          ▼
 ┌────────────────────────────────────────────────────────┐
 │                       xv6 커널                          │
 │                                                         │
 │  consoleintr() ─► REQ| 감지 ─► agent_dispatch()        │
 │                                       │                 │
 │                                       ▼                 │
 │                          ┌──────────────────────┐       │
 │                          │  거부 목록(KILL/EXEC) │       │
 │                          │  → 즉시 차단         │       │
 │                          └──────────┬───────────┘       │
 │                                     ▼                   │
 │                          ┌──────────────────────┐       │
 │                          │  agentq 링버퍼       │       │
 │                          └──────────┬───────────┘       │
 │                                     │ sys_agent_recv()  │
 │                                     ▼                   │
 │   ┌──────────────────────────────────────────────┐     │
 │   │  user/agentd  (jailed, is_agent=1)           │     │
 │   │  • chroot /agentbox                          │     │
 │   │  • exec/kill/mknod 차단                      │     │
 │   │  • 화이트리스트: PRINT/READ/WRITE/LS/…       │     │
 │   │  • 함수별 priority 적용                      │     │
 │   └──────────────────────────────────────────────┘     │
 │                                                         │
 │   ┌──────────────────────────────────────────────┐     │
 │   │  CFS 스케줄러                                │     │
 │   │  • cfs_weight[41] (Linux sched_prio_to_weight)│    │
 │   │  • cfs_vdelta(prio) = 가중치 기반 tick 가산  │     │
 │   │  • cfs_min_vruntime: 전역 단조 증가          │     │
 │   │  • fork 상속 / wakeup 보정                   │     │
 │   └──────────────────────────────────────────────┘     │
 └────────────────────────────────────────────────────────┘
```

명령 흐름의 핵심: **사람의 셸 입력은 무제한 그대로, LLM 명령(REQ|…)만 jail 안에서
실행**된다. 사람과 LLM의 권한 경계가 코드 경로 자체에서 분리된다.

---

## 1. F1·F2 — 우선순위 시스템 콜과 user/kernel 클래스

### 1.1 priority 범위 확장

- 기존 0..20 → **−20..20**으로 확장. **음수 = 커널-클래스.**
- `procdump` 출력에서 음수는 `[K]`, agent 프로세스는 `[A]` 마커 표시
  ([proc.c:806-807](xv6-riscv/kernel/proc.c)).

### 1.2 시스템 콜

- `int setpriority(int pid, int prio)` ([sysproc.c:116](xv6-riscv/kernel/sysproc.c))
- `int getpriority(int pid)` ([sysproc.c:161](xv6-riscv/kernel/sysproc.c))
- 번호: `SYS_setpriority=22`, `SYS_getpriority=23` ([syscall.h](xv6-riscv/kernel/syscall.h))

### 1.3 권한 가드 (F2의 핵심)

`sys_setpriority`는 **세 방향**으로 user/kernel 경계를 지킨다:

1. **상승 차단** — user-클래스(priority ≥ 0)가 누구에게도 음수 priority를 부여 불가:
   ```c
   if(priority < 0 && myproc()->priority >= 0)
     return -1;
   ```
2. **커널-클래스 보호** — user-클래스 호출자는 **이미 커널-클래스(priority < 0)인
   대상**(예: init)을 변경 불가. 대상의 현재 priority를 락 하에 확인 후 거부:
   ```c
   if(p->priority < 0 && !caller_kernel){ release(&p->lock); return -1; }
   ```
   → 격리 agent가 `NICE`로 init/커널-클래스를 user 범위로 끌어내리는 강등을 차단.
   커널-클래스 호출자만 가능.
3. **격리 agent 자기-한정** — 격리 agent(`is_agent`)는 **자기 자신만** renice 가능,
   다른 user 프로세스 대상은 거부 (jail 안에서의 스케줄링 DoS 차단):
   ```c
   if(myproc()->is_agent && pid != myproc()->pid)
     return -1;
   ```
   ([sysproc.c:132](xv6-riscv/kernel/sysproc.c)).

### 1.4 init은 커널-클래스

`userinit()`이 첫 프로세스(`init`, pid 1)를 **priority −5**로 시작
([proc.c:305](xv6-riscv/kernel/proc.c)). init은 고아 프로세스 회수·셸 재기동
담당이므로 커널-클래스가 합리적.

fork된 자식은 user 코드를 실행하므로 **user 범위(10)로 환원**되어 커널-클래스가
전체로 전파되지 않는다 ([proc.c:373](xv6-riscv/kernel/proc.c)).

---

## 2. F3·F4 — CFS 스케줄러 (Linux 방식)

### 2.1 가중치 테이블

Linux 커널의 `sched_prio_to_weight`를 그대로 이식 ([proc.c:36-46](xv6-riscv/kernel/proc.c)):

```c
static const uint cfs_weight[41] = {
  88761, 71755, 56483, 46273, 36291,   // -20 .. -16
  29154, 23254, 18705, 14949, 11916,   // -15 .. -11
   9548,  7620,  6100,  4904,  3906,   // -10 ..  -6
   3121,  2501,  1991,  1586,  1277,   //  -5 ..  -1
   1024,   820,   655,   526,   423,   //   0 ..   4
    335,   272,   215,   172,   137,   //   5 ..   9
    110,    87,    70,    56,    45,   //  10 ..  14
     36,    29,    23,    18,    15,   //  15 ..  19
     12,                               //  20
};
```

가중치가 클수록(=낮은 nice일수록) **vruntime이 느리게 증가**하므로 CPU를 더 점유.

### 2.2 vruntime 증분

```c
uint64 cfs_vdelta(int priority) {
  int idx = priority + 20;
  if(idx < 0)  idx = 0;     // clamp [0,40]
  if(idx > 40) idx = 40;
  return CFS_WMULT * NICE_0_LOAD / cfs_weight[idx];   // 2^20 * 1024 / weight
}
```

타이머 틱마다 user/kernel 두 경로 모두에서 가산 ([trap.c:84-92, 164-170](xv6-riscv/kernel/trap.c)):

```c
if(which_dev == 2) {
  struct proc *cp = myproc();
  if(cp){
    acquire(&cp->lock);
    cp->vruntime += cfs_vdelta(cp->priority);
    release(&cp->lock);
  }
  yield();
}
```

정수 연산만 사용 (부동소수 미사용). `uint64`라 사실상 오버플로 없음.

### 2.3 전역 min_vruntime

Linux의 `cfs_rq->min_vruntime`을 모방한 **단조 증가 전역 변수**
`cfs_min_vruntime` + `cfs_lock` 스핀락 (선언 [proc.c:61-62](xv6-riscv/kernel/proc.c),
`cfs_min`/`cfs_advance_min` [proc.c:65-79](xv6-riscv/kernel/proc.c)).

- 스케줄러가 매 픽마다 leftmost vruntime으로 전진(`cfs_advance_min`).
- 신규 프로세스는 `vruntime = cfs_min()`으로 시작 → 즉시 starve하지 않음
  ([proc.c:190-191](xv6-riscv/kernel/proc.c)).

NPROC 매번 스캔하는 옛 `min_vruntime_skip()`은 제거했다.

### 2.4 F4 — fork 상속

`kfork()`에서 자식이 부모의 vruntime을 그대로 상속:
```c
np->priority = (p->priority < 0) ? 10 : p->priority;   // user로 환원
np->vruntime = p->vruntime;
```
([proc.c:373-374](xv6-riscv/kernel/proc.c))

### 2.5 F4 — wakeup 보정

I/O로 잠들었던 프로세스가 깨어날 때 `max(vruntime, min_vruntime) − BONUS`로 보정
([proc.c:690-701](xv6-riscv/kernel/proc.c)):

```c
uint64 base = (p->vruntime > mvr) ? p->vruntime : mvr;
p->vruntime = (base > CFS_WAKEUP_BONUS) ? base - CFS_WAKEUP_BONUS : 0;
```

- `max`로 floor → 오래 잔 프로세스가 작은 vruntime으로 CPU 독점하는 것 차단.
- 작은 보너스(`CFS_WAKEUP_BONUS = 1_000_000`) → I/O 지연 보상.

### 2.6 스케줄러 픽

**순차 배열 스캔**으로 RUNNABLE 중 leftmost(min vruntime) 선택. 동률이면
`creation_tick`(먼저 생긴 것) 우선 ([proc.c:531-562](xv6-riscv/kernel/proc.c)).
요구사항 §1의 RB-Tree 배제 방침 유지.

---

## 3. F7·F8 — 샌드박싱 (3계층 방어)

"사람이 친 셸 명령은 그대로, **LLM이 만든 명령만 jail 안에서**"를 위해 실행
경로 자체를 분리했다.

### 3.1 (a) 콘솔 게이트 + 2단계 명령 디스패치 — [agentcmd.c](xv6-riscv/kernel/agentcmd.c)

`consoleintr()`가 `REQ|`로 시작하는 라인을 가로채 `agent_dispatch()`로 넘긴다.
**인터럽트 컨텍스트라 fork·파일 작업·sleep 불가** → 실행하지 않고 intake 링버퍼에
적재만 한다(stage 1). 실제 라우팅은 프로세스 컨텍스트에서 `agent_drain()`
(`usertrap`/`consoleread`에서 호출)이 `agent_dispatch_now()`로 수행한다(stage 2).
이 분리는 F9 캐시 핸들러가 `begin_op()`로 sleep할 수 있어 **필수**다.

`agent_dispatch_now()`는 선택적 `agent:<role>|` 접두어를 떼고:

1. 메타 명령 `ASK`/`LLM_RESP`/`CACHE_GET`/`CACHE_SET`은 커널에서 처리한다
   (F9 캐시 오케스트레이션 — §7.3).
2. 그 외 명령은 **거부 목록**에 있으면 차단(기본값 `KILL`, `EXEC`), 통과분만
   16-슬롯 링버퍼 `agentq`에 적재한다. 새 `dispatch` syscall(번호 31)도 같은
   `agent_dispatch_now` 경로를 탄다.

**거부 목록은 설정 가능** ([agentcmd.c](xv6-riscv/kernel/agentcmd.c),
[deny.h](xv6-riscv/kernel/deny.h)). 더 이상 하드코딩이 아니라 스핀락으로
보호되는 커널 RAM 목록이며 — `deny_listed()`가 프로세스 컨텍스트(`dispatch_now`)
에서 락 잡고 읽고, `set_deny()`로 변경한다. 셸 도구 `denyctl`로 관리:

| 명령 | 효과 | 지속성 |
|------|------|--------|
| `denyctl list` | 현재 목록 출력 | — |
| `denyctl add/rm <CMD>` | 추가/제거 | 일회성(RAM) |
| `denyctl reset` | 기본값 `{KILL,EXEC}` 복귀 | 일회성(RAM) |
| `denyctl save` | `/denylist.conf`에 저장 | 영구 |
| `denyctl load` | 파일 → 커널 적용 (부팅 시 init이 자동 실행) | — |

신규 syscall `set_deny(op,cmd)`/`get_deny(buf,max)`(번호 26·27). `set_deny`는
`is_agent` 프로세스를 거부 → **사람만** 변경 가능, 격리 agent는 자기 경계를
약화시킬 수 없음 (음수 priority 권한 가드와 동일 철학, §1.3). 이 커널 목록
하나가 하드 경계 명령과 agentd 도구 양쪽을 통제하므로 `WRITE` 같은 도구도
거부 목록에 넣으면 agentd에 도달조차 못 한다. agentd `LIST`는 `get_deny`로
실효 정책을 표시한다(`DENY(kernel)`).

`agentq_get()`이 슬립락으로 dequeue를 제공하지만, **호출 가능한 주체는 신규
syscall `sys_agent_recv`뿐이고 이 syscall은 `is_agent=1` 프로세스만 받는다**
([sysproc.c:183-203](xv6-riscv/kernel/sysproc.c)). → 큐 누출 차단.

### 3.2 (b) chroot jail + 위험 syscall 차단

**`struct proc` 확장** ([proc.h:110-111](xv6-riscv/kernel/proc.h)):
```c
int is_agent;             // F7: sandboxed agent process
struct inode *jail_root;  // F7: chroot jail root inode (0 = no jail)
```

**신규 `jail(path)` 시스템 콜** ([sysfile.c:441-466](xv6-riscv/kernel/sysfile.c)):
호출자를 path에 영구 가둔다. `is_agent=1` 설정. **되돌릴 수 없음** —
deliberately no "unjail".

**경로 해석 차단** — `namex()`가 agent의 `/`를 `jail_root`로 매핑, `..`로
jail 위로 올라가려는 시도 차단 ([fs.c:675-704](xv6-riscv/kernel/fs.c)).

**위험 syscall — confirm-escape 게이트** (2026-05-28 v2) — agent 프로세스가
`exec`·`kill`·`mknod`를 호출하면 즉시 −1을 반환하지 않고 **호스트 사용자에게
y/N 확인을 요청** ([syscall.c](xv6-riscv/kernel/syscall.c)):

```c
static int agent_blocked(int num) {
  return num == SYS_exec || num == SYS_kill || num == SYS_mknod;
}
// ... 호출 시
if(p->is_agent && agent_blocked(num)) {
  char summary[32];
  agent_call_summary(num, summary, sizeof(summary));
  int allowed = confirm_request(num, summary);   // sleep on &confirm_wait_chan
  if(!allowed) {                                  // DENY 또는 TIMEOUT
    p->trapframe->a0 = -1;
    return;
  }
  // approved — fall through to 통상 syscall 디스패치.
}
```

**opt-in 모델** — `jail()`을 직접 호출한 프로세스와 그 자식만 영향. 일반 셸·
명령어는 무영향이다.

### 3.5 confirm-escape v2 — [kernel/confirm.c](xv6-riscv/kernel/confirm.c)

위험 syscall을 *기본 거부* 에서 *기본 보류 → 사용자 동의시 허용* 으로 전환.
v1의 yield-poll 방식이 race로 깨지던 문제를 v2에서 **dedicated channel
sleep/wakeup**으로 재설계.

| 단계 | 위치 | 동작 |
|------|------|------|
| 요청 | `kernel/syscall.c` agent_blocked 분기 | `confirm_request(call_num, summary)` 호출, 호출자를 `pending[]`에 등록 |
| 슬립 | `kernel/confirm.c` `confirm_request` | `sleep(&confirm_wait_chan, &confirm_lock)` |
| 호스트 전송 | 같은 함수 | `CONFIRM_REQ\|<pid>\|<call_num>\|<summary>` 라인을 콘솔로 발신 |
| 호스트 응답 | `agent.py:_handle_confirm_req` | 15초 안에 y/N 입력 (`tcflush(TCIFLUSH)`로 stale enter 제거) |
| 응답 디스패치 | `kernel/agentcmd.c try_inline_confirm_res` | 큐 우회 — spinlock + 즉시 `wakeup(&confirm_wait_chan)`. 모든 user proc 이 SLEEPING 일 때도 race 없이 도달 |
| 타임아웃 | `kernel/trap.c clockintr` → `confirm_tick` | 매 tick `pending[]` 스캔, `CONFIRM_TIMEOUT_TICKS=150` (≈15s) 초과시 DENY로 wakeup |

**왜 인라인 처리가 필요한가**: agent.py가 `CONFIRM_RES`를 보내는 시점에는
agentd가 자기 명령에 대한 응답 대기중이라 *모든* user proc이 SLEEPING이다.
일반 큐 드레인은 *깬 user proc 이 있어야* 실행되므로 v1에서 wakeup race가
발생했다. 인라인 경로는 spinlock 만으로 흐름 완결.

### 3.6 spawn 도구 verb + populate_jail

자연어 *"프로세스 만들어줘"* 가 confirm-escape를 거쳐 실제 exec까지 도달하는
끝-끝 파이프라인:

| 컴포넌트 | 변경 |
|----------|------|
| [agent.py SYSTEM_PROMPT/TRANSLATE_PROMPT/`wire_for()`](agent.py) | `spawn` 도구 등록. wire 형식 `SPAWN\|<bin>\|<argv-joined>`. |
| [user/agentd.c `do_spawn()`](xv6-riscv/user/agentd.c) | fork → 자식이 `exec(bin, argv)` 시도 → confirm-escape 게이트 발생 → 부모는 `wait()`. |
| [user/agentd.c `populate_jail()`](xv6-riscv/user/agentd.c) | `jail("/agentbox")` *직전* 에 호스트 `/`의 `echo`·`sh`·`cat`·`ls`·`wc`·`grep`·`mkdir`·`rm`·`ln`·`kill`을 `/agentbox/`로 hard-link. 종전 빈 jail에서 `echo failed`가 발생하던 회귀 차단. |
| confirm-escape | `exec`가 `agent_blocked`라 자식이 게이트에 sleep → 호스트 y/N → ALLOW면 exec 진행, DENY/TIMEOUT이면 `SPAWN: exec ... failed`. |

### 3.7 console.c REQ| payload echo skip

`consoleintr()`가 `REQ|`로 시작하는 line의 *payload byte* 를 echo하지 않도록
변경 ([kernel/console.c](xv6-riscv/kernel/console.c)). prefix `REQ|` 4 byte와
line terminator `\n` 만 echo. 이유: agent.py가 보낸 wire의 byte-단위 echo가
`consolewrite`의 출력 (예: agentd의 `printf`)과 byte-race로 interleave 해서
`NT|__OBS6__` 같은 garbled line이 socket으로 새던 문제 차단. line boundary가
유지돼 agent.py의 `split('\n')`은 정상 작동.

### 3.3 (c) 격리 워커 `agentd` — [user/agentd.c](xv6-riscv/user/agentd.c)

[init.c:26-33](xv6-riscv/user/init.c)이 부팅 시 `agentd`를 자동 기동한다.
agentd는 시작 직후 `jail("/agentbox")`로 자기 격리, 큐에서 명령을 받아
**jail 안에서** 실행한다.

**도구 테이블** (F7 화이트리스트 + F8 함수별 priority):

| FN       | allowed | priority | 비고 |
|----------|---------|----------|------|
| PRINT    | 1       | 10       | |
| READ     | 1       |  8       | |
| WRITE    | 1       | 12       | |
| LS       | 1       |  8       | |
| NICE     | 1       |  5       | `<pid>` 대상 — PS로 pid 확인 후 사용 |
| LIST     | 1       |  0       | F8 우선순위/접근 뷰 |
| SETPRIO  | 1       |  5       | |
| PS       | 1       |  8       | **AI 자기관찰** — 프로세스 목록(pid·state·prio·name) |
| HELP     | 1       |  0       | **AI 자기관찰** — usage 포함 명령 카탈로그 |

각 도구 실행 직전 `setpriority(getpid(), table[i].priority)` 호출
([agentd.c](xv6-riscv/user/agentd.c)) → **F8의 "함수별 priority"가
실제 스케줄러에 반영**된다. `SETPRIO <FN>:<prio>`로 LLM이 런타임에 재조정 가능.

**AI 자기관찰 명령어 셋**: LLM이 환경을 보고 판단하도록 두 정보 명령을 둔다.
**PS**는 신규 syscall `procinfo(buf,max)`([sysproc.c](xv6-riscv/kernel/sysproc.c),
[procinfo.h](xv6-riscv/kernel/procinfo.h))로 proc 테이블 스냅샷을 받아
pid·state·priority·name(`[K]`/`[A]` 포함)을 출력 — `NICE`가 대상 pid를 알 수
있게 한다. 표현은 유저(agentd)에서, 데이터는 커널에서(`get_deny` 패턴과 동일).
**HELP**는 각 명령의 인자 형식(usage)을 출력해 LLM이 호출법을 런타임 확인한다.

### 3.4 이중 방어

- 거부 목록(`KILL`/`EXEC`)은 **커널 단계에서** 차단 → agentd에 도달조차 못함.
- 가령 거부 목록을 빠져나간 외부 코드가 있더라도, **agent 프로세스 syscall
  레이어**가 `exec`/`kill`/`mknod`를 추가로 거부.
- 파일 접근은 **chroot로 경로 자체가 격리**되어 jail 밖 파일은 보이지도 않음.

---

## 4. F5 + 에이전트 루프 — `agent.py`

원래 "자연어 → JSON 1개 → 명령 1개 → 종료" 단발 번역기였으나, **자율 ReAct
루프 + 대화 메모리**로 재설계됨.

### 4.1 모드 자동 선택

| 조건                                      | 모드   |
|-------------------------------------------|--------|
| `UPSTAGE_API_KEY` 없음                    | mock   |
| 키 있음 + openai SDK 존재                 | solar  |
| 키 있음 + openai SDK 없음                 | mock으로 폴백 |

`.env` 자동 로드 (스크립트 옆 파일). 실 환경변수가 우선.

### 4.2 ReAct 루프

시스템 프롬프트에 도구 목록 + 응답 스키마(`{thought,tool,args}` 또는
`{thought,answer}`) 명시. 매 스텝마다:

1. LLM 호출 → 도구 호출 또는 최종 답변
2. 도구면 xv6에 `REQ|<CMD>|<arg>` 전송
3. xv6 출력을 **OBS 마커**로 정확히 캡처
4. OBSERVATION으로 다음 LLM 호출에 전달
5. 최대 8스텝까지 반복

### 4.3 출력 캡처 (마커 방식)

명령 뒤에 `REQ|PRINT|__OBS<n>__`을 함께 전송. agentd가 큐를 순서대로 처리하므로
**마커 출력 이전의 모든 라인**이 해당 도구의 순수 결과 → 정확히 잘라낼 수 있다.

### 4.4 대화 메모리

`self.messages`에 system + 전체 대화 누적. 후속 질문이 이전 맥락(예: 이미 읽은
파일 내용)을 그대로 활용. 최근 24개 메시지로 자동 정리.

### 4.5 출력 정리

스레드 안전 락 + 3-상태 커서(`start`/`xv6`/`other`)로 메인·리더 스레드 동시
write 충돌 제거. 색 구분:
- xv6 출력 (`xv6 ┃` 녹색)
- 전송 로그 (`→ xv6` 노랑)
- 사고 (`💭` dim)
- 최종 답변 (`╭─ answer ─╮` 청록 박스)

### 4.6 자연어 안정화 (2026-05-28)

| 항목 | 변경 |
|------|------|
| `_cache_lookup` strip 정규화 | lookup key에 `.strip()` 추가 ([agent.py:995](agent.py)). store 키와 정합 → 동일 prompt 가 2번째부터 `[cache HIT]` (Issue A). |
| SYSTEM_PROMPT ★ token-boundary | spawn 블록 내부 → standalone clause로 분리. chat/write/print/spawn 모든 도구에 한글/CJK byte 보존, 숫자 drop 금지 신호 (Issue B mitigation — Solar tokenizer가 `"22 + 45는?"`에서 `45` drop하던 회귀 완화). |
| `_handle_confirm_req` | 프롬프트 "5초"→"15초" 정정, `input()` 직전 `termios.tcflush(TCIFLUSH)`로 stale stdin enter 제거. |
| `_read_line` cooked 모드 기본 | raw 모드(`_wipe_input`/`_draw_input`)에서 한글 입력시 4-5회 키 입력이 필요한 UX 회귀 → cooked 기본. raw는 `AGENT_RAW_INPUT=1` opt-in. |
| `_wipe_input` safety margin | raw 모드 사용시 `rows_above + 1` 한 줄 추가 — 한글 wide-cell wrap 경계에서 backspace 후 첫 단어 잔재 fix. |
| `_wire_escape` | chat/write/print 페이로드의 `\n`→`\\n` (agentd `unescape_inplace`가 복원). 다중라인 write가 wire newline에서 잘리던 문제 해결. |
| mock 분기 확장 | N11(인사 캐시) / N14(write→read chain `_mock_step_after_write`) / N15(nice ACL) / N16(kill confirm deny) / N17(한글 argv 보존). 결정적 회귀의 기반. |

---

## 4.7 자동 회귀 — `tools/ralph_*` (65/65)

| 도구 | 시나리오 수 | 포트 | 모드 | 시간 | 검증 영역 |
|------|------------|------|------|------|----------|
| [tools/ralph_battery.py](tools/ralph_battery.py) | 26 | 5555 | 실제 LLM 없이 raw TCP 셸 명령 직접 | ~3 min | 부팅·셸·agentdemo·cache_test·cfs_share·write_race·priority_test·eval cache/fair/acl·confirm allow/deny·confirm_kill/mknod·다중 pending 경계·still-alive·no-fatal |
| [tools/ralph_natlang.py](tools/ralph_natlang.py) | 39 | 6666 | `UPSTAGE_API_KEY=""`로 mock 모드 + agent.py stdin pipe | ~50 s | bridge connect·ReAct·`:ask` MISS/HIT·spawn 허용/거부·multi-line write/read·greeting 캐시 HIT (Issue A 회귀)·조사 변형·ACL nice·kill confirm·한글 argv·EOF |

두 하니스 모두 격리 포트와 자체 `fs.img` 복사본을 써 사용자의 4444 세션과
*동시 실행 가능*. fatal marker (`panic:`/`kerneltrap`/`scause=`) 전역 스캔
+ 시나리오별 success matcher 카운트로 PASS/FAIL 결정. 누적 **65/65 GREEN**.

---

## 5. 빌드 & 실행

### 5.1 의존성 (호스트)

```bash
# macOS
brew install qemu riscv-software-src/riscv/riscv-tools
pip3 install openai

# Linux
apt install qemu-system-misc gcc-riscv64-linux-gnu python3-pip
pip3 install openai
```

### 5.2 xv6 빌드 & QEMU 부팅

```bash
cd xv6-riscv
make clean
make qemu          # 셸 모드 (priority_test 등 직접 실행)
# 또는
make qemu-agent    # TCP 시리얼 모드 — 4444 포트 listen
```

### 5.3 에이전트 실행 (TCP 모드일 때)

다른 터미널:

```bash
# .env 작성 (UPSTAGE_API_KEY, UPSTAGE_MODEL=solar-pro2)
python3 agent.py
```

`mode = solar (solar-pro2)` 또는 `mode = mock` 표시 후 자연어 입력 대기.

---

## 6. 검증

| 케이스                             | 결과                          |
|-----------------------------------|-------------------------------|
| 커널 부팅 (smp=1, smp=3)          | ✅ panic 없음                 |
| `priority_test` Test 1·2          | ✅ 모두 PASSED                |
| `priority_test` Test 3 (자동 검증) | ✅ pipe로 finish order 강제 검증 (HIGH→MED→LOW) |
| `cfs_share` (점유율 정량)         | smp=1에서 priority별 share 출력 (수동 확인) |
| `agentdemo` 5개 체크 (jail read/write, `..` 차단, 음수 priority 거부, exec confirm-escape allow/deny) | ✅ 모두 통과 |
| jail 내부 파일 read/write         | ✅                            |
| jail에서 `..`·외부 경로 접근       | ✅ DENY                       |
| agent의 `exec`/`kill`/`mknod`     | ✅ confirm-escape 게이트 (호스트 y/N, timeout=default deny) |
| `confirm_kill` / `confirm_mknod`  | ✅ SYS_kill·SYS_mknod 게이트 직접 검증 |
| 음수 priority 권한 가드           | ✅ user → 음수 거부됨         |
| `REQ\|KILL\|...` (커널 거부)      | ✅ DENY 메시지, queue 미진입  |
| `REQ\|SPAWN\|/echo\|...`          | ✅ fork+exec, exec시 confirm-escape 발화 |
| `denyctl add WRITE` → `REQ\|WRITE\|` | ✅ 일회성 추가 후 커널 차단(agentd 미도달) |
| `denyctl save` → 재부팅 → `list`  | ✅ `/denylist.conf` 자동 로드, 항목 생존 |
| `denyctl reset` / `rm KILL`       | ✅ 기본 복귀 / 일회성 해제 동작 |
| `_cache_lookup` strip 정규화      | ✅ 동일 prompt 2번째부터 `[cache HIT]` (Issue A 회귀) |
| **`tools/ralph_battery.py`** — 26 셸/syscall 시나리오 | ✅ 26/26 PASS |
| **`tools/ralph_natlang.py`** — 39 자연어 시나리오 (mock) | ✅ 39/39 PASS |
| 실 Solar API ReAct 멀티스텝       | ✅ ls→read×N→summary 시나리오 |
| 대화 메모리                       | ✅ "CFS 언급한 파일이 뭐였지?" → 메모리 응답 |

누적 자동 회귀 **65/65 GREEN**.

---

## 7. 알려진 한계 / Future Work

### 7.1 평가 한계

- ~~`priority_test` Test 3은 finish-order를 프로그램적으로 검증하지 않음.~~
  → 2026-05-20 pipe 기반 자동 검증 추가.
- `cfs_share`의 점유율 결과는 콘솔 출력만 제공 — 자동 합격 판정은 없음
  (수치 기준이 환경 의존적이라 의도). 보고서용 측정에 사용.
- smp ≥ RUNNABLE 수일 때 시분할 효과가 줄어들어 측정은 smp=1 권장.

### 7.2 F6 — JSON 파싱 위치 (설계 결정)

JSON 파싱은 **호스트(`agent.py`)에서 수행**한다. 커널은 검증된 최소
포맷(`REQ|<CMD>|<arg>`)만 수신. 근거는 다음 네 가지:

1. **커널 안전성** — 동적 입력 파서의 메모리 안전성 결함은 곧 커널 패닉.
   Python `json.loads`는 표준 라이브러리로 검증돼 있어 리스크 ≈ 0.
2. **부동소수·동적할당 제약** — xv6 커널은 둘 다 금지. JSON의 number·중첩
   객체와 자연스럽게 충돌.
3. **분리된 책임** — LLM 응답 해석(에이전트 사고)과 커널 명령 실행을 계층
   분리. 향후 LLM 측 포맷 변경이 커널에 영향 없음.
4. **사후 검증 단순화** — `REQ|` prefix + 명령 화이트리스트 + arg 길이 검사
   한 줄로 끝남. 거부 목록 + jail + 위험 syscall 차단 3중 방어와 정합적.

제안서 §F6 원문은 "xv6 내부 JSON 역직렬화"를 명시하므로, 본 결정과 사유를
기술 보고서에 함께 기재한다.

### 7.3 confirm-escape v2 (2026-05-28 — 활성화)

이전 단계에서는 v1의 yield-poll wakeup race로 인해 `confirm.c`가 비활성 상태였고
F7은 `exec`/`kill`/`mknod`에 대해 단순 -1 반환만 사용했다. 2026-05-28에 v2
(dedicated channel sleep + clockintr broadcast timeout + inline
`try_inline_confirm_res`)로 재설계해 **활성화**됨. 현재 default deny 정책
(timeout=15s)으로 사용자가 자리를 비워도 안전 fallback.

### 7.4 Solar 토크나이저 경계 한계

`"22 + 45는?"`처럼 한글 조사가 숫자에 직접 붙는 일부 prompt에서 Solar
tokenizer가 `45`를 drop하는 경우가 관측됨. agent.py의 wire 경로는 byte-perfect
(byte 단위 검증). mitigation은 SYSTEM_PROMPT의 standalone `★ token-boundary`
clause + 사용자 우회 (공백 추가, 따옴표 제거). 코드 측면 fix는 불가 — LLM
측 한계.

### 7.5 캐시(F9) 연동 · 미구현(선택)

- **F9 LLM 응답 캐시 — 구현·연결 완료**. `kernel/cache.c`(16-슬롯 RAM +
  `/cache.bin` 디스크 오버레이 + MinHash/Jaccard 의미 매칭, Se-Joong 원작
  `76b2737` 이식, syscall `set_cache`/`get_cache` 29·30, `cache_test` 13/13)를
  **명령 경로에 연결**: `agentcmd.c`가 2단계(인터럽트 `agent_dispatch` enqueue →
  프로세스 컨텍스트 `agent_drain`→`agent_dispatch_now`)로 나뉘고, `ASK`/`LLM_RESP`/
  `CACHE_GET`/`CACHE_SET` 메타 명령을 커널에서 처리한다. `ASK`는 캐시 조회 후
  히트면 agentd로 직접 전달(Solar 생략), 미스면 호스트에 `LLM_REQ` 발신 → 호스트가
  `LLM_RESP`로 응답하면 `cache_set` 후 전달. 새 `dispatch` syscall(번호 31)로
  유저 프로그램(`eval`/`agent_multi`/`write_race`)도 이 경로를 구동한다. 호스트
  `agent.py`는 `:ask`로 연결(기본 ReAct 루프 유지). deny 검사는 `dispatch_now`의
  forward 경로로 이동해 F7 경계 유지.
- **F10 LoRA 학습**: xv6 환경 제약상 범위 외.

---

## 8. 변경 파일 요약

| 파일                                                                       | 변경       |
|----------------------------------------------------------------------------|------------|
| [kernel/proc.h](xv6-riscv/kernel/proc.h)                                   | `is_agent`, `jail_root` 필드 |
| [kernel/proc.c](xv6-riscv/kernel/proc.c)                                   | CFS 가중치 테이블·`cfs_vdelta`·`cfs_min`, fork·wakeup·scheduler·procdump 개편 |
| [kernel/trap.c](xv6-riscv/kernel/trap.c)                                   | timer tick에서 `cfs_vdelta()` 사용; `usertrap`에서 `agent_drain()` 호출; `clockintr`에서 `confirm_tick()` 호출 (CPU 0) |
| [kernel/console.c](xv6-riscv/kernel/console.c)                             | `consoleread`에서 `agent_drain()` 호출; 입력 버퍼 256→2048(긴 `ASK`); **REQ\| payload echo skip** (wire byte-race 차단) |
| [kernel/confirm.c](xv6-riscv/kernel/confirm.c)                             | **신규** — confirm-escape v2 (sleep on `&confirm_wait_chan`, `clockintr` driven `confirm_tick` timeout, 15s default deny) |
| [kernel/fs.c](xv6-riscv/kernel/fs.c)                                       | `namex()` chroot jail 적용 |
| [kernel/main.c](xv6-riscv/kernel/main.c)                                   | `agentcmd_init()`·`cacheinit()` 호출 |
| [kernel/syscall.{c,h}](xv6-riscv/kernel/syscall.c)                         | `SYS_jail`·`SYS_agent_recv`·`SYS_set_deny`·`SYS_get_deny`·`SYS_set_cache`·`SYS_get_cache`·`SYS_dispatch` 등록; agent 위험 syscall(`exec`/`kill`/`mknod`)을 단순 -1 → **`confirm_request()` 호출**로 전환 (host y/N 게이트) |
| [kernel/sysfile.c](xv6-riscv/kernel/sysfile.c)                             | `sys_jail()` 신규; `create()`를 non-static 전환(cache.c용) |
| [kernel/sysproc.c](xv6-riscv/kernel/sysproc.c)                             | `sys_setpriority` 음수 권한 가드, `sys_agent_recv()`, `sys_set_deny()`·`sys_get_deny()`, `sys_procinfo()`, `sys_set_cache()`·`sys_get_cache()`, `sys_dispatch()` 신규 |
| [kernel/agentcmd.c](xv6-riscv/kernel/agentcmd.c)                           | **2단계 디스패치**(인터럽트 `agent_dispatch` enqueue → 프로세스 `agent_drain`/`agent_dispatch_now`) + 캐시 메타 명령(`ASK`/`LLM_RESP`/`CACHE_GET/SET`) + **설정 가능한** 거부 목록(`deny_add/remove/reset/clear/snapshot`) + `try_inline_confirm_res()` (CONFIRM_RES 큐 우회 — 모든 user proc SLEEPING 일 때도 wakeup race 없음) + `SPAWN` 디스패치 |
| [kernel/cache.c](xv6-riscv/kernel/cache.c)                                 | **신규(이식)** — F9 LLM 응답 캐시(RAM+디스크 오버레이·MinHash/Jaccard). Se-Joong 원작 `76b2737` 이식 + 보안 하드닝 |
| [kernel/deny.h](xv6-riscv/kernel/deny.h)                                   | **신규** — 거부 목록 op 상수(커널·user 공유) |
| [kernel/procinfo.h](xv6-riscv/kernel/procinfo.h)                           | **신규** — `procinfo` 구조체(프로세스 스냅샷, 커널·user 공유) |
| [kernel/defs.h](xv6-riscv/kernel/defs.h)                                   | `cfs_vdelta`·`agentcmd_init`·`agentq_get`·`agent_drain`/`agent_dispatch_now`·`cacheinit`/`cache_get`/`cache_get_exact`/`cache_get_semantic`/`cache_set`·`create`·`confirm_tick`·`confirm_request`·`confirm_resolve` 프로토타입 |
| [user/init.c](xv6-riscv/user/init.c)                                       | `agentd` 자동 기동 + 부팅 시 `denyctl load` 1회 |
| [user/user.h, usys.pl](xv6-riscv/user/user.h)                              | `jail()`·`agent_recv()`·`set_deny()`·`get_deny()`·`set_cache()`·`get_cache()` 스텁 |
| [user/agentd.c](xv6-riscv/user/agentd.c)                                   | **신규** — 격리 에이전트 런타임; `LIST`가 커널 거부 목록 반영; `PS`·`HELP` 자기관찰 명령 + usage; `CHAT` 핸들러(캐시 응답 출력); **`do_spawn()`** (fork+exec+wait, confirm-escape 게이트로 가는 끝단); **`populate_jail()`** (`echo`/`sh`/`cat`/`ls`/... jail 진입 전 hard-link); `unescape_inplace()` (chat/write/print payload의 `\\n` 복원) |
| [user/denyctl.c](xv6-riscv/user/denyctl.c)                                 | **신규** — 거부 목록 관리 셸 도구(list/add/rm/reset/save/load) |
| [user/agentdemo.c](xv6-riscv/user/agentdemo.c)                             | **신규** — F2·F7 데모. **fork+exec 패턴** (confirm-escape allow가 데모 자체를 replace하지 않도록 자식에서 exec, 부모는 wait — allow/deny 양쪽 모두 `=== demo done ===` 도달) |
| [user/confirm_kill.c](xv6-riscv/user/confirm_kill.c)                       | **신규** — `jail("/agentbox")` 후 `kill(99999)`로 SYS_kill confirm-escape 게이트 직접 검증 |
| [user/confirm_mknod.c](xv6-riscv/user/confirm_mknod.c)                     | **신규** — SYS_mknod 게이트 직접 검증 (analogue) |
| [user/cfs_share.c](xv6-riscv/user/cfs_share.c)                             | **신규** — CFS 점유율 정량 벤치 |
| [user/cache_test.c](xv6-riscv/user/cache_test.c)                           | **신규(이식)** — F9 캐시 단독 시연 테스트(13/13 통과). `76b2737` 이식 |
| [user/eval.c](xv6-riscv/user/eval.c)                                       | **신규(이식)** — 캐시 히트율·ACL deny율 평가 하니스(`dispatch`/`get_cache`). `76b2737` 이식 |
| [user/agent_multi.c](xv6-riscv/user/agent_multi.c)                         | **신규(이식)** — 4개 역할 동시 에이전트 데모(`dispatch`). `76b2737` 이식 + WRITE `:` 적응 |
| [user/write_race.c](xv6-riscv/user/write_race.c)                           | **신규(이식)** — 동일 파일 동시 WRITE 직렬화 데모(`dispatch`). `76b2737` 이식 + WRITE `:` 적응 |
| [mkfs/mkfs.c](xv6-riscv/mkfs/mkfs.c)                                       | **복원** — `.gitignore` 패턴 문제로 누락돼 있던 것 |
| [agent.py](agent.py)                                                       | 단발 → ReAct 루프, `.env` 로더, 출력 동기화; `ps`·`help` 도구; `:ask`(커널 F9 캐시 경로) + `LLM_REQ` 응답; **`spawn` 도구 verb**; `_cache_lookup` strip 정규화 (Issue A); SYSTEM_PROMPT ★ token-boundary clause; `_handle_confirm_req` (15s, tcflush); `_read_line` cooked-mode 기본 + `_wipe_input` safety margin; `_wire_escape`; mock 모드 N11-N17 분기 |
| [Makefile](xv6-riscv/Makefile)                                             | `_agentdemo`·`_agentd`·`_cache_test`·`_eval`·`_agent_multi`·`_write_race`·`_confirm_kill`·`_confirm_mknod` UPROGS, `cache.o`·`confirm.o` 커널 OBJS 등록 |
| [tools/ralph_battery.py](tools/ralph_battery.py)                           | **신규** — 26 셸/syscall 시나리오 자동 회귀 (격리 포트 5555, ~3 min) |
| [tools/ralph_natlang.py](tools/ralph_natlang.py)                           | **신규** — 39 자연어 시나리오 자동 회귀 (mock 모드, 격리 포트 6666, ~50 s) |
