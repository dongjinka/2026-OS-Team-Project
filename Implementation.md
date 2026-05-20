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
  ([proc.c:804-805](xv6-riscv/kernel/proc.c)).

### 1.2 시스템 콜

- `int setpriority(int pid, int prio)` ([sysproc.c:113](xv6-riscv/kernel/sysproc.c))
- `int getpriority(int pid)` ([sysproc.c:141](xv6-riscv/kernel/sysproc.c))
- 번호: `SYS_setpriority=22`, `SYS_getpriority=23` ([syscall.h](xv6-riscv/kernel/syscall.h))

### 1.3 권한 가드 (F2의 핵심)

`sys_setpriority`는 **user-클래스 프로세스(priority ≥ 0)가 누군가에게 음수
priority를 부여하지 못하도록** 거부한다. → 권한 상승 차단.

```c
if(priority < 0 && myproc()->priority >= 0)
  return -1;
```

### 1.4 init은 커널-클래스

`userinit()`이 첫 프로세스(`init`, pid 1)를 **priority −5**로 시작
([proc.c:303](xv6-riscv/kernel/proc.c)). init은 고아 프로세스 회수·셸 재기동
담당이므로 커널-클래스가 합리적.

fork된 자식은 user 코드를 실행하므로 **user 범위(10)로 환원**되어 커널-클래스가
전체로 전파되지 않는다 ([proc.c:371](xv6-riscv/kernel/proc.c)).

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
  int idx = priority + 20;  // clamp 생략
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
`cfs_min_vruntime` + `cfs_lock` 스핀락 ([proc.c:58-80](xv6-riscv/kernel/proc.c)).

- 스케줄러가 매 픽마다 leftmost vruntime으로 전진(`cfs_advance_min`).
- 신규 프로세스는 `vruntime = cfs_min()`으로 시작 → 즉시 starve하지 않음
  ([proc.c:187-188](xv6-riscv/kernel/proc.c)).

NPROC 매번 스캔하는 옛 `min_vruntime_skip()`은 제거했다.

### 2.4 F4 — fork 상속

`kfork()`에서 자식이 부모의 vruntime을 그대로 상속:
```c
np->priority = (p->priority < 0) ? 10 : p->priority;   // user로 환원
np->vruntime = p->vruntime;
```
([proc.c:371-372](xv6-riscv/kernel/proc.c))

### 2.5 F4 — wakeup 보정

I/O로 잠들었던 프로세스가 깨어날 때 `max(vruntime, min_vruntime) − BONUS`로 보정
([proc.c:688-699](xv6-riscv/kernel/proc.c)):

```c
uint64 base = (p->vruntime > mvr) ? p->vruntime : mvr;
p->vruntime = (base > CFS_WAKEUP_BONUS) ? base - CFS_WAKEUP_BONUS : 0;
```

- `max`로 floor → 오래 잔 프로세스가 작은 vruntime으로 CPU 독점하는 것 차단.
- 작은 보너스(`CFS_WAKEUP_BONUS = 1_000_000`) → I/O 지연 보상.

### 2.6 스케줄러 픽

**순차 배열 스캔**으로 RUNNABLE 중 leftmost(min vruntime) 선택. 동률이면
`creation_tick`(먼저 생긴 것) 우선 ([proc.c:529-551](xv6-riscv/kernel/proc.c)).
요구사항 §1의 RB-Tree 배제 방침 유지.

---

## 3. F7·F8 — 샌드박싱 (3계층 방어)

"사람이 친 셸 명령은 그대로, **LLM이 만든 명령만 jail 안에서**"를 위해 실행
경로 자체를 분리했다.

### 3.1 (a) 콘솔 게이트 + 명령 큐 — [agentcmd.c](xv6-riscv/kernel/agentcmd.c)

`consoleintr()`가 `REQ|`로 시작하는 라인을 가로채 `agent_dispatch()`로 넘긴다.
**인터럽트 컨텍스트라 fork·파일 작업 불가** → 여기서는 실행하지 않고:

1. **거부 목록**(`KILL`, `EXEC`)이면 즉시 차단 ([agentcmd.c:48-58](xv6-riscv/kernel/agentcmd.c)).
2. 나머지는 16-슬롯 링버퍼 `agentq`에 적재 ([agentcmd.c:87-102](xv6-riscv/kernel/agentcmd.c)).

`agentq_get()`이 슬립락으로 dequeue를 제공하지만, **호출 가능한 주체는 신규
syscall `sys_agent_recv`뿐이고 이 syscall은 `is_agent=1` 프로세스만 받는다**
([sysproc.c:163-180](xv6-riscv/kernel/sysproc.c)). → 큐 누출 차단.

### 3.2 (b) chroot jail + 위험 syscall 차단

**`struct proc` 확장** ([proc.h:110-111](xv6-riscv/kernel/proc.h)):
```c
int is_agent;             // F7: sandboxed agent process
struct inode *jail_root;  // F7: chroot jail root inode (0 = no jail)
```

**신규 `jail(path)` 시스템 콜** ([sysfile.c:439-465](xv6-riscv/kernel/sysfile.c)):
호출자를 path에 영구 가둔다. `is_agent=1` 설정. **되돌릴 수 없음** —
deliberately no "unjail".

**경로 해석 차단** — `namex()`가 agent의 `/`를 `jail_root`로 매핑, `..`로
jail 위로 올라가려는 시도 차단 ([fs.c:678-710](xv6-riscv/kernel/fs.c)).

**위험 syscall 거부** — agent 프로세스는 `exec`·`kill`·`mknod` 호출이 모두
−1 리턴 ([syscall.c:141-156](xv6-riscv/kernel/syscall.c)):
```c
static int agent_blocked(int num) {
  return num == SYS_exec || num == SYS_kill || num == SYS_mknod;
}
```

**opt-in 모델** — `jail()`을 직접 호출한 프로세스와 그 자식만 영향. 일반 셸·
명령어는 무영향이다.

### 3.3 (c) 격리 워커 `agentd` — [user/agentd.c](xv6-riscv/user/agentd.c)

[init.c:26-33](xv6-riscv/user/init.c)이 부팅 시 `agentd`를 자동 기동한다.
agentd는 시작 직후 `jail("/agentbox")`로 자기 격리, 큐에서 명령을 받아
**jail 안에서** 실행한다.

**도구 테이블** (F7 화이트리스트 + F8 함수별 priority):

| FN       | allowed | priority |
|----------|---------|----------|
| PRINT    | 1       | 10       |
| READ     | 1       |  8       |
| WRITE    | 1       | 12       |
| LS       | 1       |  8       |
| NICE     | 1       |  5       |
| LIST     | 1       |  0       |
| SETPRIO  | 1       |  5       |

각 도구 실행 직전 `setpriority(getpid(), table[i].priority)` 호출
([agentd.c:217](xv6-riscv/user/agentd.c)) → **F8의 "함수별 priority"가
실제 스케줄러에 반영**된다. `SETPRIO <FN>:<prio>`로 LLM이 런타임에 재조정 가능.

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
| `agentdemo` (F2·F7 종합)          | ✅ 5개 체크 통과              |
| jail 내부 파일 read/write         | ✅                            |
| jail에서 `..`·외부 경로 접근       | ✅ DENY                       |
| agent의 `exec`/`kill`/`mknod`     | ✅ −1 반환                    |
| 음수 priority 권한 가드           | ✅ user → 음수 거부됨         |
| `REQ\|KILL\|...` (커널 거부)      | ✅ DENY 메시지, queue 미진입  |
| 실 Solar API ReAct 멀티스텝       | ✅ ls→read×N→summary 시나리오 |
| 대화 메모리                       | ✅ "CFS 언급한 파일이 뭐였지?" → 메모리 응답 |

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

### 7.3 미구현 (선택)

- **F9 LLM 응답 캐시**: 미구현. `kernel/cache.c` + LRU + `CACHE_GET/SET` 명령
  으로 확장 가능 (plan.md §5.2).
- **F10 LoRA 학습**: xv6 환경 제약상 범위 외.

---

## 8. 변경 파일 요약

| 파일                                                                       | 변경       |
|----------------------------------------------------------------------------|------------|
| [kernel/proc.h](xv6-riscv/kernel/proc.h)                                   | `is_agent`, `jail_root` 필드 |
| [kernel/proc.c](xv6-riscv/kernel/proc.c)                                   | CFS 가중치 테이블·`cfs_vdelta`·`cfs_min`, fork·wakeup·scheduler·procdump 개편 |
| [kernel/trap.c](xv6-riscv/kernel/trap.c)                                   | timer tick에서 `cfs_vdelta()` 사용 |
| [kernel/fs.c](xv6-riscv/kernel/fs.c)                                       | `namex()` chroot jail 적용 |
| [kernel/main.c](xv6-riscv/kernel/main.c)                                   | `agentcmd_init()` 호출 |
| [kernel/syscall.{c,h}](xv6-riscv/kernel/syscall.c)                         | `SYS_jail`·`SYS_agent_recv` 등록, agent 위험 syscall 차단 |
| [kernel/sysfile.c](xv6-riscv/kernel/sysfile.c)                             | `sys_jail()` 신규 |
| [kernel/sysproc.c](xv6-riscv/kernel/sysproc.c)                             | `sys_setpriority` 음수 권한 가드, `sys_agent_recv()` 신규 |
| [kernel/agentcmd.c](xv6-riscv/kernel/agentcmd.c)                           | **신규** — 명령 큐 + 거부 목록 게이트 |
| [kernel/defs.h](xv6-riscv/kernel/defs.h)                                   | `cfs_vdelta`·`agentcmd_init`·`agentq_get` 프로토타입 |
| [user/init.c](xv6-riscv/user/init.c)                                       | `agentd` 자동 기동 |
| [user/user.h, usys.pl](xv6-riscv/user/user.h)                              | `jail()`·`agent_recv()` 스텁 |
| [user/agentd.c](xv6-riscv/user/agentd.c)                                   | **신규** — 격리 에이전트 런타임 |
| [user/agentdemo.c](xv6-riscv/user/agentdemo.c)                             | **신규** — F2·F7 데모 |
| [user/cfs_share.c](xv6-riscv/user/cfs_share.c)                             | **신규** — CFS 점유율 정량 벤치 |
| [mkfs/mkfs.c](xv6-riscv/mkfs/mkfs.c)                                       | **복원** — `.gitignore` 패턴 문제로 누락돼 있던 것 |
| [agent.py](agent.py)                                                       | 단발 → ReAct 루프, `.env` 로더, 출력 동기화 |
| [Makefile](xv6-riscv/Makefile)                                             | `_agentdemo`·`_agentd` UPROGS 등록 |
