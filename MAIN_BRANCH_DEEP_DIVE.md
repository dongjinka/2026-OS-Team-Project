# `main` 브랜치 심층 분석 — Jail Sandboxing & CFS Scheduler

> 대상 브랜치: `main` (HEAD `8ded8d7`, SeungBeom 의 통합본)
> 분석 일자: 2026-05-20
> 핵심 commit: `30c81dc` "feat: LLM-OS 핵심 기능 구현 (CFS·우선순위·샌드박싱·에이전트 루프)"
>
> 이 문서는 OS / xv6 / 스케줄러를 처음 보는 사람이라도 따라올 수 있도록
> 1) 기초 개념부터 2) 구체 자료구조와 코드 흐름까지 한 단계씩 짚는다.
> 코드 인용은 모두 `xv6-riscv/` 하위 실제 파일의 줄 번호 기준.

---

## 0. 무엇을 다루는 문서인가

`main` 브랜치에는 LLM-OS 프로젝트의 두 축이 들어 있다:

1. **CFS 스케줄러** — Linux 의 Completely Fair Scheduler 를 xv6 위에 단순화해
   포팅. nice 값으로 가중치 기반 CPU 분배. (`proc.c`, `proc.h`, `trap.c`, `sysproc.c`)
2. **Jail 샌드박싱** — LLM 이 발화한 명령을 실행하는 *에이전트 프로세스*를
   파일·syscall·우선순위 세 측면에서 격리. (`sysfile.c`, `fs.c`, `syscall.c`,
   `proc.c`, `agentd.c`, `agentdemo.c`)

이 두 시스템은 독립적이지만 **합주**해야 안전이 성립한다 — jail 만으로는
"느린 CPU 폭주" 를 막지 못하고, CFS 만으로는 "기밀 파일 읽기" 를 막지 못한다.

---

# Part A. CFS — Completely Fair Scheduler

## A.0 사전 지식: 스케줄링 기초

### A.0.1 스케줄러란?

CPU 코어 수는 유한한데 동시에 실행 가능한 프로세스(`state == RUNNABLE`)
는 보통 그보다 많다. 누구를 다음 tick 동안 CPU 에 올릴지 정하는 커널
루틴이 **스케줄러**다.

원본 xv6 의 스케줄러는 **라운드 로빈** (`proc[]` 배열을 순서대로 훑으며
RUNNABLE 보이면 무조건 그 다음 tick 부여) 이라, "어떤 프로세스가 더 중요한가" 라는
개념 자체가 없었다.

### A.0.2 nice / priority

UNIX 전통상 **nice 값**은 -20(우선순위 최고) ~ 19(최저). "남에게 양보 잘 함 =
nice" 라는 의미라서 숫자가 **클수록 양보**, **작을수록 적게 양보 = 우선**.

이 프로젝트는 nice 값을 그대로 `int priority` 라는 이름으로 부른다 — 정확히
동의어. 범위는 `-20 ~ 20` (Linux 보다 한 슬롯 넓힌 게 의도. `cfs_weight[41]` 참고).

음수(`< 0`)는 *kernel-class* — 일반 사용자 프로세스가 자기를 거기까지 올릴
수 없다 (escalation 방지, `sys_setpriority` 에서 차단).

### A.0.3 vruntime

CFS 의 심장. **"이 프로세스가 가중치 보정된 가상 시간으로 얼마나 달렸는가"**
를 누적하는 카운터다. 정의:

```
vruntime += (실제 달린 시간) × (NICE_0_LOAD / load_weight)
```

- 무거운(nice 가 작은) 프로세스는 `load_weight` 가 커서 `vruntime` 이 천천히
  증가 → 같은 vruntime 에 도달하기까지 더 오래 CPU 점유.
- 가벼운(nice 가 큰) 프로세스는 `load_weight` 가 작아서 `vruntime` 이 빨리
  증가 → 빨리 다음 차례에 양보.

**스케줄링 규칙은 단순** — RUNNABLE 중 `vruntime` 가 가장 작은 놈을 다음에
실행. 이게 "공정함"의 의미: *모두가 결국 vruntime 이 동일해질 때까지 양보*.

---

## A.1 자료구조

### A.1.1 `struct proc` 의 추가 필드 — `kernel/proc.h:107-112`

```c
struct proc {
  ...
  int priority;                // CFS nice value: -20..20 (negative = kernel-class)
  uint64 vruntime;             // CFS virtual runtime
  uint creation_tick;          // Tiebreak for equal vruntime
  int is_agent;                // F7: sandboxed agent process
  struct inode *jail_root;     // F7: chroot jail root inode (0 = no jail)
};
```

- `priority` — nice 값. allocproc 에서 10 으로 초기화 (`proc.c:187`).
- `vruntime` — uint64. tick 단위가 아니라 `CFS_WMULT (= 2^20)` 스케일의
  고정소수점. 부동소수점 없이 정수 연산만으로 비율 계산하기 위함.
- `creation_tick` — vruntime 이 정확히 같은 경우의 **tiebreak**. 먼저 만들어진
  쪽이 우선 (FIFO 안정성).
- `is_agent` / `jail_root` — Part B 의 jail 용.

### A.1.2 글로벌 상태 — `kernel/proc.c:30-46, 61-62`

```c
#define NICE_0_LOAD       1024
#define CFS_WMULT         ((uint64)1 << 20)
#define CFS_WAKEUP_BONUS  1000000

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

struct spinlock cfs_lock;
uint64 cfs_min_vruntime;
```

- `cfs_weight[]` — Linux 의 `sched_prio_to_weight` 그대로. `weight(nice=0) = 1024`
  기준으로 nice 가 1 떨어지면 가중치 약 1.25 배. nice=−5 의 가중치는 약 3×.
- `CFS_WMULT (= 1<<20)` — vruntime 증분을 정수로 계산할 때의 스케일 팩터.
  `cfs_vdelta` 의 분자에 곱해두면 작은 priority 차이도 정수 안에서 보존됨.
- `CFS_WAKEUP_BONUS` — sleep 에서 깨어난 task 가 "I/O 대기 동안 잃은 시간" 일부를
  보상받는 양 (자세한 건 A.3.4).
- `cfs_min_vruntime` — 모든 RUNNABLE 의 vruntime 의 **단조 비감소 하한**.
  새 task / wakeup 의 시작값 anchor (자세한 건 A.2).

---

## A.2 핵심 수식 — `cfs_vdelta` (`proc.c:49-56`)

```c
uint64
cfs_vdelta(int priority)
{
  int idx = priority + 20;
  if(idx < 0)  idx = 0;
  if(idx > 40) idx = 40;
  return CFS_WMULT * NICE_0_LOAD / cfs_weight[idx];
}
```

이게 **한 timer tick 동안 누적할 vruntime 증분**이다. 분해해서 보면:

| priority | weight | vdelta (CFS_WMULT × 1024 / weight) | 의미 |
|----------|--------|----------------------------------|------|
| -20      | 88761  | ≈ 12 × CFS_WMULT / 1024 = 12,094  | 매우 천천히 증가 (=가장 오래 점유) |
|  -5      | 3121   | ≈ 344,070                          | 보통의 3× 빠르게 |
|   0      | 1024   | = CFS_WMULT = 1,048,576            | 기준점 (nice 0) |
|  10      | 110    | ≈ 9,765,357                        | 기준의 ~9.3× 빠르게 |
|  20      | 12     | ≈ 89,478,485                       | 매우 빠르게 증가 (=빨리 양보) |

같은 1 tick 을 실제로 달리더라도 priority 에 따라 vruntime 누적 속도가
9~10배 까지 벌어진다. 그래서 **vruntime 이 가장 작은 놈을 뽑는** 단순 규칙
하나로 CPU 비율이 priority 별로 자동 분배된다.

### vruntime 누적 위치 — `trap.c:84-94`

```c
if(which_dev == 2){                           // timer interrupt
  struct proc *cp = myproc();
  if(cp){
    acquire(&cp->lock);
    cp->vruntime += cfs_vdelta(cp->priority);
    release(&cp->lock);
  }
  yield();
}
```

매 timer tick 마다 *현재 CPU 위에서 달리던 프로세스* 의 vruntime 만 증가시킨
뒤 `yield()` 로 양보 → 스케줄러가 다시 가장 작은 vruntime 을 선택. 커널
모드에서 도는 동안에도 동일하게 `kerneltrap()` 에서 같은 처리 (`trap.c:165-171`).

---

## A.3 스케줄러 루프 — `scheduler()` (`proc.c:513-568`)

```c
void
scheduler(void)
{
  ...
  for(;;){
    intr_on(); intr_off();

    // CFS pick: scan for RUNNABLE process with smallest vruntime.
    struct proc *best = 0;
    uint64 best_vr = (uint64)-1;
    uint best_ct = 0;
    for(p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      if(p->state == RUNNABLE) {
        int better = 0;
        if(best == 0) better = 1;
        else if(p->vruntime < best_vr) better = 1;
        else if(p->vruntime == best_vr && p->creation_tick < best_ct) better = 1;
        if(better){
          if(best) release(&best->lock);
          best = p;
          best_vr = p->vruntime;
          best_ct = p->creation_tick;
          continue;
        }
      }
      release(&p->lock);
    }

    if(best == 0){ asm volatile("wfi"); continue; }

    cfs_advance_min(best_vr);     // monotonic global anchor 업데이트

    best->state = RUNNING;
    c->proc = best;
    swtch(&c->context, &best->context);
    c->proc = 0;
    release(&best->lock);
  }
}
```

### 주의 깊게 볼 점

1. **선형 스캔 (Linux 는 RB-tree 사용)** — `NPROC` 가 작으므로(기본 64) 충분.
   요구사항 F3 에 "배열 스캔, RB-Tree 미사용" 명시.
2. **best->lock 을 든 채로 다음 후보 비교** — 더 좋은 후보 발견하면 *이전*
   best 의 lock 을 풀고 새 best 의 lock 을 든 채 계속. 최종 winner 는
   `swtch` 직전까지 잠금 보유 → race 차단.
3. **`cfs_advance_min(best_vr)`** — 글로벌 `cfs_min_vruntime` 를 winner 의
   vruntime 까지 끌어올림. **단조 비감소** 보장 (`proc.c:73-80`):
   ```c
   if(v > cfs_min_vruntime) cfs_min_vruntime = v;
   ```
   이게 wakeup / fork 의 anchor 역할.

### A.3.1 새 프로세스 생성 — `allocproc()` (`proc.c:184-191`)

```c
found:
  p->pid = allocpid();
  p->state = USED;
  p->priority = 10;                 // 기본 user-class nice = 10
  p->vruntime = cfs_min();          // 현재 시점의 글로벌 하한에 anchor
  p->creation_tick = ticks;
  p->is_agent = 0;
  p->jail_root = 0;
```

새 프로세스의 vruntime 을 **0 이 아니라 현재 min_vruntime** 에 맞추는 게
중요. 만약 0 으로 두면 새 프로세스가 등장하자마자 "vruntime 가 가장 작음" 으로
스케줄러를 독점 → 기존 프로세스 starvation.

### A.3.2 init 의 음수 priority — `userinit()` (`proc.c:298-308`)

```c
p->cwd = namei("/");
// F2: init is xv6's supervisor process (pid 1, ...) — closest to a kernel-level
// process, so it is given a negative (kernel-class) priority.
p->priority = -5;
p->state = RUNNABLE;
```

pid 1 (init) 은 부팅 첫 프로세스이자 고아 reaping / shell 재시작 담당. 시스템
필수 supervisor 라 항상 우선 실행되도록 kernel-class priority 부여.

**중요**: init 이 fork 한 자식(=`agentd`, `sh`)은 kfork 에서 user-class 로
강등됨 (다음 A.3.3 참고). 따라서 일반 사용자 프로세스가 음수 priority 를
"상속" 받을 수 없음.

### A.3.3 fork 의 CFS 상속 — `kfork()` (`proc.c:369-372`)

```c
// F4: a fork child inherits the parent's CFS state. A child forked by a
// kernel-class process runs user code, so it drops to the user range.
np->priority = (p->priority < 0) ? 10 : p->priority;
np->vruntime = p->vruntime;
```

규칙 두 가지:

- 부모가 **user-class** (`priority >= 0`) → 자식도 같은 priority. nice 가 가족
  내에서 일관됨.
- 부모가 **kernel-class** (음수) → 자식은 무조건 nice=10 으로 강등.
  init → sh 같은 경우, sh 까지 kernel 우선순위를 끌고 갈 이유 없음.

`vruntime` 은 그대로 상속 — 부모가 이미 "어느 정도 달린 상태" 라면 자식도
그 시점에서 시작. fork bomb 으로 자식들이 새로 0 부터 시작해 다른 프로세스를
밀어내지 않도록 함.

### A.3.4 sleep → wakeup 시 vruntime 보정 — `wakeup()` (`proc.c:684-704`)

```c
void
wakeup(void *chan)
{
  struct proc *p;
  uint64 mvr = cfs_min();

  for(p = proc; p < &proc[NPROC]; p++) {
    if(p != myproc()){
      acquire(&p->lock);
      if(p->state == SLEEPING && p->chan == chan) {
        p->state = RUNNABLE;
        // F4: place the woken task at max(its vruntime, min_vruntime) so it
        // can neither monopolize the CPU (stale low vruntime) nor starve
        // (stale high vruntime), then grant a small bonus for I/O latency.
        uint64 base = (p->vruntime > mvr) ? p->vruntime : mvr;
        p->vruntime = (base > CFS_WAKEUP_BONUS) ? base - CFS_WAKEUP_BONUS : 0;
      }
      release(&p->lock);
    }
  }
}
```

**문제 상황**: I/O bound 프로세스가 디스크/네트워크 기다리느라 1 초 sleep.
그동안 CPU bound 프로세스들의 vruntime 은 한참 증가. 깨어났을 때 자기 vruntime 은
옛날 그대로라 *너무 작음* → 스케줄러가 한참 동안 그놈만 실행 → CPU bound
들이 starvation. 반대로 자기 vruntime 이 *너무 큼* (다른 정책에서 들어온 경우)
이면 자기가 영원히 못 뽑힘.

**해법** (위 두 줄):
1. `base = max(p->vruntime, min_vruntime)` — 너무 뒤처졌으면 현재 글로벌
   하한까지 끌어올림.
2. `p->vruntime = base - CFS_WAKEUP_BONUS` — 거기서 살짝(약 1×CFS_WMULT 정도)
   *빼서* I/O 보상. wakeup 직후 단 한 차례, 거의 즉시 CPU 받음.

이건 Linux 의 wakeup 처리와 정확히 같은 정신.

---

## A.4 priority 시스템콜 — `sys_setpriority` (`sysproc.c:113-139`)

```c
uint64
sys_setpriority(void)
{
  int pid, priority;
  argint(0, &pid);
  argint(1, &priority);

  if(priority < -20 || priority > 20) return -1;
  // F2/F7: a user-class process may not grant kernel-class (negative)
  // priority to anyone — that would be a privilege escalation.
  if(priority < 0 && myproc()->priority >= 0) return -1;

  struct proc *p;
  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){ p->priority = priority; release(&p->lock); return 0; }
    release(&p->lock);
  }
  return -1;
}
```

핵심 가드 두 개:
- **범위 체크** — `-20..20` 외 값 거부.
- **escalation 차단** — 호출자가 user-class(`>=0`) 면 음수 priority 를 부여
  못함. 자기 자신을 kernel-class 로 올리거나, 다른 user 프로세스를 음수로
  올리는 것 모두 차단. (`agentdemo` 에서 이 동작을 실증)

`sys_getpriority` 는 단순 lookup. 둘 다 `user/usys.pl:46` 의
`entry("setpriority"); entry("getpriority");` 로 user space 에 노출됨.

---

## A.5 동작 시나리오: nice=−5 vs nice=10

3 코어 (CPUS=3) 환경에서 두 CPU-bound process A(nice=-5), B(nice=10) 만 RUNNABLE 이라
가정. tick 마다 vruntime 증분:

| | priority | vdelta/tick |
|---|---|---|
| A | -5  | ≈ 344,070  |
| B | 10  | ≈ 9,765,357 |

100 ticks 가 지나면:
- A 의 vruntime: 100 × 344,070 = 34,407,000
- B 의 vruntime: 100 × 9,765,357 = 976,535,700

스케줄러는 항상 vruntime 작은 쪽을 선택 → A 가 약 9.3 배 더 자주 뽑힘.
정확히 `weight(A) / weight(B) = 3121 / 110 ≈ 28.4` 가 *vdelta 의 역수 비율*
이지만, 실제 CPU 분배는 둘이 동시에 RUNNABLE 일 때 vruntime 이 동일해지도록
양쪽이 번갈아 도므로 **각자가 받는 tick 비율은 weight 비율과 같음**.

다중 코어에서는 코어별 scheduler 가 독립적으로 위 알고리즘을 돌리고, 같은
`cfs_lock` 하의 `cfs_min_vruntime` 만 공유. lock contention 최소화 + 글로벌
하한 일관성.

---

# Part B. Jail Sandboxing

## B.0 사전 지식: 프로세스 격리

### B.0.1 왜 격리가 필요한가

LLM 이 발화한 명령을 그대로 OS 위에서 실행하면 위험하다:
- LLM 이 `/etc/passwd` 류 시스템 파일을 읽도록 시도할 수 있음.
- 다른 프로세스를 `kill` 하거나 임의 바이너리를 `exec` 으로 띄울 수 있음.
- `setpriority(-20)` 으로 자기를 최우선 등급에 올리려 시도할 수 있음.

**보안 원리**: 프롬프트 신뢰 경계는 **사용자가 아니라 LLM 출력**이다.
사용자가 자기 머신을 망가뜨리는 건 사용자 책임이지만, LLM 이 사용자 의도와
관계없이 위험한 동작을 *발화* 했을 때 OS 가 그걸 *실행* 해선 안 됨.

### B.0.2 격리 방법 3 가지

| 격리 차원 | Unix 전통 | main 의 구현 |
|---------|----------|-------------|
| 파일 가시성 | `chroot(path)` | `jail(path)` — 영구 chroot |
| 능력(capability) | setuid/SELinux | `agent_blocked` 화이트리스트 (`syscall.c`) |
| 자원 한도 | `nice`, `rlimit` | `sys_setpriority` 의 escalation 가드 |

이 셋이 **AND** 조건으로 함께 걸려야 빠짐 없는 sandbox. main 은 셋 다 구현.

---

## B.1 자료구조

### `struct proc` 의 jail 필드 — `proc.h:110-111`

```c
int is_agent;                // F7: sandboxed agent process
struct inode *jail_root;     // F7: chroot jail root inode (0 = no jail)
```

- `is_agent == 1` — "이 프로세스는 sandboxed 모드". jail / syscall 게이트가
  모두 이 플래그로 켜짐.
- `jail_root` — jail 의 root 디렉토리 inode 포인터. `is_agent` 여도 이게 0 이면
  jail 미적용 (안전 fallback).

allocproc 에서 둘 다 0 으로 초기화 (`proc.c:190-191`). freeproc 에서 단순 reset
(`proc.c:240-241`) — 실제 inode reference 해제는 kexit 에서 (`proc.c:431-435`).

---

## B.2 jail 진입 — `sys_jail` (`sysfile.c:439-465`)

```c
// F7: jail(path) — confine the calling process to `path` as a sandboxed
// agent. Afterward the process's filesystem root is `path`: it cannot see
// or reach anything above it (namex() enforces the chroot), and dangerous
// syscalls (exec/kill/mknod) are refused (see syscall.c). Permanent — there
// is deliberately no "unjail". `path` is resolved with the pre-jail root.
uint64
sys_jail(void)
{
  char path[MAXPATH];
  struct inode *ip;
  struct proc *p = myproc();

  begin_op();
  if(argstr(0, path, MAXPATH) < 0 || (ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);
  if(ip->type != T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }
  iunlock(ip);
  iput(p->cwd);          // drop the old cwd; the jail dir replaces it
  end_op();

  p->jail_root = idup(ip);  // separate ref, released in kexit()
  p->cwd = ip;              // cwd takes namei()'s ref
  p->is_agent = 1;
  return 0;
}
```

### 단계별 동작

1. **argstr** — user 공간에서 jail path 문자열을 커널 버퍼로 복사.
2. **namei(path)** — 이 시점에서는 *아직 jail 이 아닌 상태* 라서 절대 경로가
   진짜 루트(`/`) 기준으로 해석됨. agentd 가 `jail("/agentbox")` 호출하면
   real `/agentbox` 의 inode 를 얻음.
3. **type 검사** — 디렉토리가 아니면 거부 (`T_DIR != ip->type`).
4. **iput(p->cwd)** — 이전 cwd inode 의 ref 감소. transaction 내에서 처리.
5. **jail_root = idup(ip)** — `idup` 으로 ref count 를 따로 잡아둠. 이게
   영구 anchor.
6. **cwd = ip** — namei 가 반환한 ref 는 cwd 가 흡수.
7. **is_agent = 1** — sandbox 플래그 ON. 이 시점 이후의 모든 syscall 이
   sandbox 규칙에 걸림.

**unjail 없음** — 의도적. 일단 들어가면 종료(`kexit`) 전까지 나올 수 없음.
탈출 syscall 자체를 안 만든 게 가장 강한 보장.

---

## B.3 경로 격리 — `namex` (`fs.c:670-720`)

xv6 의 모든 `open`, `mkdir`, `chdir`, `link`, `unlink` 가 결국 호출하는 path
resolver. main 은 여기에 jail 후크 두 줄을 박았다.

```c
static struct inode*
namex(char *path, int nameiparent, char *name)
{
  struct inode *ip, *next;
  struct proc *pr = myproc();
  // F7: a sandboxed agent is chroot'd — its filesystem root is the jail.
  int jailed = (pr && pr->is_agent && pr->jail_root);

  if(*path == '/'){
    if(jailed)
      ip = idup(pr->jail_root);   // "/" resolves to the jail root
    else
      ip = iget(ROOTDEV, ROOTINO);
  } else
    ip = idup(pr->cwd);

  while((path = skipelem(path, name)) != 0){
    ilock(ip);
    if(ip->type != T_DIR){ iunlockput(ip); return 0; }
    if(nameiparent && *path == '\0'){ iunlock(ip); return ip; }
    // F7: block ".." from climbing above the jail root — the agent
    // cannot reach anything outside its confined subtree.
    if(jailed && name[0]=='.' && name[1]=='.' && name[2]=='\0' &&
       ip->dev == pr->jail_root->dev && ip->inum == pr->jail_root->inum){
      iunlock(ip);
      continue;   // stay pinned at the jail root
    }
    if((next = dirlookup(ip, name, 0)) == 0){ iunlockput(ip); return 0; }
    iunlockput(ip);
    ip = next;
  }
  ...
}
```

두 가지 후크:

### B.3.1 절대 경로 시작점 변경 (`*path == '/'`)
- 비 sandboxed: `iget(ROOTDEV, ROOTINO)` — 디스크의 진짜 루트 (`/`).
- sandboxed: `idup(pr->jail_root)` — jail 디렉토리.

따라서 agent 가 `open("/foo")` 하면 실제로는 `/agentbox/foo` 가 열림.
`/etc/passwd` 같은 외부 파일은 *존재 자체가 안 보임*.

### B.3.2 `..` 탈출 차단
경로 도중에 `name == ".."` 이고 *현재 inode 가 jail_root 와 동일* 이면 그
`..` 를 **무시하고 같은 자리에 머무름**. `continue` 로 다음 segment 로 넘어감.

즉 `open("/../../init")` 같은 시도는:
1. `*path == '/'` → jail_root 부터 시작.
2. 첫 `..` segment 처리 시 위 가드에 걸려 jail_root 에 그대로 머무름.
3. 두 번째 `..` 도 동일.
4. 최종적으로 `init` 을 jail_root 안에서 lookup → 없음 → -1.

이 두 줄이 **모든** 파일 syscall (open / mkdir / chdir / link / unlink) 에
공통으로 적용된다. 한 곳에서 막으면 끝.

---

## B.4 능력(capability) 격리 — `agent_blocked` (`syscall.c:139-160`)

```c
// F7: system calls a sandboxed agent process may not invoke.
static int
agent_blocked(int num)
{
  return num == SYS_exec || num == SYS_kill || num == SYS_mknod;
}

void
syscall(void)
{
  int num;
  struct proc *p = myproc();

  num = p->trapframe->a7;
  if(num > 0 && num < NELEM(syscalls) && syscalls[num]) {
    // F7 sandbox: a jailed agent process is refused dangerous syscalls.
    if(p->is_agent && agent_blocked(num)){
      printf("[sandbox] pid %d (%s): syscall %d blocked\n",
             p->pid, p->name, num);
      p->trapframe->a0 = -1;
      return;
    }
    p->trapframe->a0 = syscalls[num]();
  } else {
    ...
  }
}
```

**syscall dispatcher 의 길목**에서 sandbox 규칙을 적용. agent process 가:
- `SYS_exec` (다른 바이너리 띄우기) — 차단.
- `SYS_kill` (다른 프로세스 종료) — 차단.
- `SYS_mknod` (device 노드 생성, 보통 root 권한) — 차단.

세 syscall 만 콕 집은 이유:
- `exec` — agent 가 화이트리스트 외 코드를 *내부적으로 띄우는* 우회 차단.
- `kill` — 시스템 내 다른 프로세스를 죽여 의도치 않은 영향. 자기 자신만 죽고
  싶으면 `exit` 사용.
- `mknod` — 디바이스 핸들 직접 생성으로 권한 우회 가능성.

read / write / open / mkdir / pipe 등은 *허용* 되지만 위의 jail path 격리 때문에
실제로 jail 밖에 닿을 수 없음.

**로깅**: 거부될 때 `[sandbox] pid X (name): syscall N blocked` 출력 — 디버깅
+ 시연 자료로 유용.

---

## B.5 fork / exit 시 jail 처리

### B.5.1 fork — `kfork` (`proc.c:374-378`)

```c
// F7: a sandboxed agent's children stay confined to the same jail.
np->is_agent = p->is_agent;
if(p->jail_root)
  np->jail_root = idup(p->jail_root);
```

부모가 sandboxed 면 자식도 같은 jail 안에 머무름. inode reference 는
`idup` 으로 별도 ref count 추가. 자식이 따로 종료될 때 자기 ref 해제.

이게 빠지면 agent 가 fork → 자식이 sandbox 빠짐 → 우회. 그래서 fork
경로에 두 줄 후크 필수.

### B.5.2 exit — `kexit` (`proc.c:429-435`)

```c
begin_op();
iput(p->cwd);
if(p->jail_root)             // F7: release the jail root inode
  iput(p->jail_root);
end_op();
p->cwd = 0;
p->jail_root = 0;
```

`begin_op` / `end_op` transaction 안에서 jail_root 의 ref count 감소.
fs.c 의 `iput` 이 nlink==0 이고 ref count 가 0 이 되면 실제 disk free 까지
처리하지만 jail 디렉토리는 보통 영구 디렉토리라 disk 변경 없음.

---

## B.6 부팅 흐름 — init → agentd

### B.6.1 init.c 의 fork — `init.c:26-35`

```c
// Start the jailed LLM agent runtime. It confines itself via jail() and
// then serves LLM-issued commands inside its sandbox for the system's
// lifetime (it does not exit, so init's wait() loop never reaps it).
pid = fork();
if(pid == 0){
  char *aargv[] = { "agentd", 0 };
  exec("agentd", aargv);
  printf("init: exec agentd failed\n");
  exit(1);
}
```

- init(pid=1, kernel-class) 이 fork.
- 자식은 kfork 규칙에 따라 priority=10 (user-class) 로 강등 (A.3.3 참고).
- exec("agentd") → agentd 의 main 으로 진입.
- 종료하지 않는 데몬 — init 의 `wait()` 루프는 sh 의 exit code 만 본다.

### B.6.2 agentd 의 self-jail — `agentd.c:232-249`

```c
int
main(void)
{
  // Ensure the jail directory exists, then confine ourselves to it.
  mkdir(JAIL);                       // "/agentbox"
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
```

**순서가 중요**:
1. `mkdir("/agentbox")` 가 jail 진입 *전*. 지금이 마지막으로 진짜 루트를 볼
   기회.
2. `jail("/agentbox")` 호출 — 이 다음부터 `/` = `/agentbox`.
3. `agent_recv()` 루프 — 커널이 큐에 넣어준 LLM 명령 라인 수신.
4. `execute()` 가 명령 dispatch.

### B.6.3 agentd 의 명령 dispatch — `agentd.c:31-39, 197-230`

```c
static struct fn table[] = {
  { "PRINT",   1, 10 },
  { "READ",    1,  8 },
  { "WRITE",   1, 12 },
  { "LS",      1,  8 },
  { "NICE",    1,  5 },
  { "LIST",    1,  0 },
  { "SETPRIO", 1,  5 },
};
```

`{name, allowed, priority}` 의 화이트리스트. 알 수 없는 명령은 자동 거부
(`unknown cmd '%s'`). `allowed=0` 으로 마킹된 함수도 미리 거부.

### B.6.4 함수별 priority (F8)

```c
for(int i = 0; i < NFN; i++){
  if(streq(cmd, table[i].name)){
    if(!table[i].allowed){ ... DENY ... return; }
    // F8: run this function's work at its (LLM-tunable) priority.
    setpriority(getpid(), table[i].priority);

    if(streq(cmd, "PRINT"))        do_print(arg);
    else if(streq(cmd, "READ"))    do_read(arg);
    ...
  }
}
```

**SETPRIO** 명령으로 LLM 이 함수별 priority 를 조정 가능. 가드 (`agentd.c:181-184`):

```c
if(parsei(c+1, &prio) < 0 || prio < 0 || prio > 20){
  printf("[agentd] SETPRIO: priority must be 0..20\n");
  return;
}
```

0..20 만 허용 — 즉 user-class 내에서만 조정. 음수 시도는 user space 에서도
한 번 더 거부됨 (kernel 의 `sys_setpriority` escalation 가드와 이중 방어).

---

## B.7 시나리오: LLM 이 `/etc/passwd` 를 읽으려 하면?

가정: LLM 이 `REQ|READ|/etc/passwd` 같은 명령을 발화 → 커널이 큐에 넣음 →
agentd 가 받아 `do_read("/etc/passwd")` 실행 → `open("/etc/passwd", O_RDONLY)`.

xv6 의 namex 경로:
1. `*path == '/'` → `jailed = true` 이므로 `idup(pr->jail_root)` = `/agentbox` inode.
2. `etc` segment lookup — `/agentbox` 안에 `etc` 디렉토리 없음 → `dirlookup` 실패.
3. namex 반환 0 → sys_open 이 -1 반환.
4. agentd 의 `do_read` 에서 `fd < 0` 분기:
   ```
   [agentd] READ: '/etc/passwd' not reachable inside jail
   ```

LLM 입장에서는 그 파일이 *존재하지 않는 시스템에 사는 것*. 시도 자체가
무의미하다는 신호를 주지도 않음 (jail 의 존재가 노출되지 않음).

### 권한 escalation 시도

LLM 이 `NICE pid:-5` 같은 명령 발화 → `do_nice("pid:-5")` → `setpriority(pid,-5)`:
- agentd 자체가 `is_agent && priority >= 0` 인 user-class.
- sys_setpriority 의 가드: `if(priority < 0 && myproc()->priority >= 0) return -1;`
- → -1 반환. `[agentd] NICE: denied` 출력.

### exec 시도

LLM 이 어떻게든 `exec("/some_binary")` 를 유도해도 syscall.c 의 `agent_blocked`
가 SYS_exec 차단 → `[sandbox] pid X (agentd): syscall 7 blocked`.

---

# Part C. CFS × Jail — 합주

세 층의 보안 경계가 모두 함께 작동해야 안전:

| 차원 | 메커니즘 | 코드 위치 |
|----|---------|----------|
| **파일 가시성** | jail (chroot) — `/` 와 `..` 해석 변경 | `fs.c namex` |
| **능력(capability)** | exec/kill/mknod 차단 | `syscall.c agent_blocked` |
| **자원 점유** | priority escalation 차단 + CFS 가중치 분배 | `sysproc.c sys_setpriority`, `proc.c scheduler` |

### 단일 장애점 분석

- jail 만 있으면? LLM 이 sandbox 안에서 `fork()` 폭주 가능. CFS 가 user-class
  내에서 공정 분배하지만 *시스템 전체 user-class 점유 비중*은 막지 못함.
- syscall 화이트리스트만 있으면? LLM 이 `open("/")` 만 사용해도 진짜 루트 본다.
- priority 가드만 있으면? jail 없으니 파일 접근 무방비.

**셋 다** 있어야 LLM 발화가 사용자 신뢰 경계를 넘지 못함.

### 결정적 관점

이 모델의 OS-for-LLM 메시지: *LLM 은 외부 입력이지 권한 보유자가 아니다*.
화이트리스트(능력) + chroot(가시성) + priority 가드(자원) 의 3 중 격리는
"LLM 이 발화한 어떤 명령도 사용자가 직접 OS 셸에서 해서는 안 되는 동작이라면
실제로 실행되지 않는다" 를 보장.

---

# Part D. 시연 — `agentdemo.c`

`agentdemo` 가 4 가지 확인:

```c
// 1. A file inside the jail is reachable.
fd = open("/notes.txt", O_RDONLY);     // → /agentbox/notes.txt
// → "[demo] read inside jail: hello from inside the jail"

// 2a. '..' escape is blocked.
if(open("/../init", O_RDONLY) < 0)
  printf("[demo] OK   escape via '..' blocked\n");

// 2b. Outside file is invisible.
if(open("/cat", O_RDONLY) < 0)
  printf("[demo] OK   outside file '/cat' is invisible\n");

// 3. Sandboxed (user-class) cannot grant itself negative priority.
if(setpriority(getpid(), -1) < 0)
  printf("[demo] OK   negative priority denied (no escalation)\n");

// 4. Dangerous syscalls are blocked.
char *argv[] = { "echo", "x", 0 };
if(exec("/echo", argv) < 0)
  printf("[demo] OK   exec() blocked by sandbox\n");
```

xv6 shell 에서 `agentdemo` 실행 시 모든 OK 라인이 떠야 정상. 어느 하나라도
FAIL 이 보이면 sandbox 가 깨진 것 — 해당 후크 위치(jail/agent_blocked/
setpriority 가드)를 추적.

---

# Part E. 한계와 가능한 개선

| 한계 | 설명 | 가능한 개선 |
|------|-----|-----------|
| **선형 스캔 스케줄러** | NPROC 이 커지면 O(N) | Linux 의 RB-tree 도입 (요구사항이 허락하면) |
| **글로벌 단일 cfs_lock** | 멀티 코어 contention | 코어별 runqueue (Linux `cfs_rq`) |
| **wakeup bonus 가 고정 상수** | 짧게 잠든 task 와 오래 잠든 task 가 동일 보상 | sleep 시간 비례 보상 |
| **jail 은 단일 디렉토리** | 다중 jail / nested jail 불가 | 디렉토리 트리 리스트로 확장 |
| **read/write 자체는 차단 안 함** | jail 내 파일은 자유 접근 | 파일 단위 ACL (Sejoong 브랜치의 role 시스템 참고) |
| **`agent_blocked` 가 syscall 번호 enumerate** | 신규 syscall 추가 시 잊을 수 있음 | per-syscall `bool agent_safe` 비트 |
| **SETPRIO 가 함수 priority 만 조정** | LLM 이 같은 함수에 다른 priority 적용 불가 | per-invocation override 명령 |

---

# Part F. 참고 — 변경된 파일 한눈

| 파일 | 핵심 변경 |
|------|----------|
| `kernel/proc.h:107-112` | priority/vruntime/creation_tick/is_agent/jail_root 필드 |
| `kernel/proc.c:30-80` | CFS_WMULT, cfs_weight, cfs_vdelta, cfs_min/advance |
| `kernel/proc.c:184-191` | allocproc — 기본 priority/vruntime 초기화 |
| `kernel/proc.c:298-307` | userinit — init 의 priority = -5 |
| `kernel/proc.c:369-378` | kfork — CFS / jail 상속 |
| `kernel/proc.c:429-435` | kexit — jail_root iput |
| `kernel/proc.c:513-568` | scheduler — vruntime 최소 선택 |
| `kernel/proc.c:684-704` | wakeup — vruntime 보정 + I/O 보너스 |
| `kernel/trap.c:84-94` | timer tick 에 vruntime 증분 |
| `kernel/trap.c:165-171` | kernel mode 에서도 동일 처리 |
| `kernel/sysproc.c:113-158` | sys_setpriority / sys_getpriority |
| `kernel/sysproc.c:164-180` | sys_agent_recv |
| `kernel/sysfile.c:439-465` | sys_jail |
| `kernel/fs.c:670-720` | namex jail 후크 |
| `kernel/syscall.c:139-168` | agent_blocked + syscall 게이트 |
| `kernel/syscall.h:25-26` | SYS_jail = 24, SYS_agent_recv = 25 |
| `user/usys.pl:47-48` | jail / agent_recv user stub |
| `user/user.h:29-30` | int jail(const char*); int agent_recv(char*); |
| `user/init.c:26-35` | agentd fork+exec |
| `user/agentd.c` | sandboxed daemon (whitelist + per-fn priority) |
| `user/agentdemo.c` | sandbox 4-항 검증 시연 |
