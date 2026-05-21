# Implementation — OS for LLM (Direction A)

xv6-riscv 위에서 LLM(Upstage Solar Pro)을 호스팅·지휘하는 **에이전트 런타임**.
Project_requirements.md §2 Direction A에 해당하며, AIOS 논문의 핵심 컴포넌트를
간소화하여 xv6 커널에 직접 구현했다.

| AIOS 컴포넌트          | 이 프로젝트 구현                                                              |
| ---------------------- | ----------------------------------------------------------------------------- |
| Agent Scheduler        | **CFS 스케줄러** — `vruntime`+`priority` 가중 공정 분배                       |
| Tool Manager           | **커널 메타-디스패처 + jailed agentd** — 메타-명령은 in-kernel, 실행은 jail   |
| Memory/Storage Manager | **LLM 응답 캐시 (의미 매칭)** — exact + MinHash/Jaccard, RAM LRU + 디스크 스왑 |
| Tool Sandbox           | **Jail (chroot + syscall 가드)** — `init→fork→exec(agentd)→jail()`, 위험 syscall 차단 |
| LLM Kernel Bridge      | **agent.py** — Solar API 프록시 (인터넷 호출만 대행)                          |

> **설계 원칙**: 오케스트레이션(캐시 조회·LLM 호출 판단·캐시 저장)은 xv6 커널이
> 메타-디스패처로 주관하고, 실제 명령 실행은 jail 안 `agentd` 가 담당. `agent.py`는
> 의사결정을 하지 않으며, xv6가 물리적으로 할 수 없는 일(인터넷 너머 Solar API
> 호출)만 대행하는 **얇은 프록시**다. 과거 단계의 *역할(role) 기반 ACL* 가드는
> jail 통합으로 흡수됐고, 측정 도구(`eval acl`)와 `agent:<role>` wire 라벨은
> 진단·시연용으로 남아 있다.
>
> 구현 완료 범위는 **Phase 1~5 전부** + **다중 에이전트 데모** + **평가지표 측정** (project.md
> Direction A 의 "tool-use sandbox / file & shell tool permissions / multiple concurrent
> LLM agents / fair allocation" 키워드 직접 정합).

---

## 0. 시스템 아키텍처

```
 ┌─────────────────┐   자연어 입력
 │  사용자 (REPL)  │
 └────────┬────────┘
          │
          ▼
 ┌────────────────────┐ HTTPS   ┌────────────────────────┐
 │  agent.py          │────────►│  Upstage Solar Pro     │
 │  (Solar 프록시)    │◄────────│  (api.upstage.ai/v1)   │
 │  • 입력 중계        │  JSON   └────────────────────────┘
 │  • LLM_REQ 응답만   │
 └────────┬───────────┘
          │  TCP 4444 (QEMU 시리얼)
          │  ↓ REQ|ASK|<프롬프트>      ↑ LLM_REQ|<프롬프트>
          │  ↓ REQ|LLM_RESP|<CMD>|<arg>
          ▼
 ┌────────────────────────────────────────────────────────┐
 │                      xv6 커널                          │
 │                                                        │
 │  consoleintr() ─► REQ| 라인 감지 ─► 큐 enqueue         │
 │  usertrap()    ─► agent_drain() ─► agent_dispatch_now() │
 │                          │                             │
 │                  ┌───────┴────────┐                    │
 │                  ▼                ▼                    │
 │             REQ|ASK 처리      PRINT/KILL/NICE 실행      │
 │                  │                                     │
 │           cache_get() 조회                             │
 │         ┌────────┴─────────┐                           │
 │       HIT                 MISS                         │
 │        │                   │                           │
 │  즉시 디스패치       LLM_REQ 송출 → (agent.py) →        │
 │  (agent.py 미개입)   REQ|LLM_RESP → cache_set + 디스패치│
 │                                                        │
 │   ┌──────────────────┐   ┌────────────────────────┐    │
 │   │  CFS 스케줄러     │   │  LLM 캐시 (cache.c)    │    │
 │   │  vruntime 기반    │   │  RAM 16슬롯 LRU        │    │
 │   └──────────────────┘   │  + /cache.bin 디스크   │    │
 │                          └────────────────────────┘    │
 └────────────────────────────────────────────────────────┘
```

**데이터 흐름 한 바퀴:**

1. 사용자가 `agent.py`에 자연어 입력 → agent.py는 가공 없이 `REQ|ASK|<프롬프트>` 전송.
2. 커널이 프롬프트를 해시해 캐시 조회.
3. **HIT** → 캐시된 와이어 명령을 즉시 디스패치. agent.py 개입 0.
4. **MISS** → 커널이 프롬프트를 보관하고 `LLM_REQ|<프롬프트>` 송출.
5. agent.py가 `LLM_REQ`를 받아 Solar 호출 → JSON → 와이어 명령 번역 →
   `REQ|LLM_RESP|<CMD>|<arg>` 회신.
6. 커널이 결과를 캐시에 저장하고 디스패치.

---

## 1. Phase 1 — agent.py (Solar 프록시)

**파일:** [agent.py](agent.py)

agent.py는 **오케스트레이션을 하지 않는다.** 역할은 두 가지뿐이다:
(1) 사용자 입력을 `REQ|ASK|`로 커널에 중계, (2) 커널이 `LLM_REQ`를 보낼 때만
Solar를 호출.

### 1.1 모드 (자동 선택)

| 조건                                     | 모드                       |
| ---------------------------------------- | -------------------------- |
| `UPSTAGE_API_KEY` 환경변수 미설정        | **mock** (로컬 규칙 기반)  |
| `UPSTAGE_API_KEY` 설정 + openai SDK 존재 | **solar** (실제 API 호출)  |
| 키는 있는데 openai SDK 부재              | mock으로 자동 폴백         |

### 1.2 REPL — 입력 중계

```python
def repl(self):
    while True:
        line = input("agent> ")
        # 가공 없이 커널에 위임 — 캐시 조회·디스패치는 커널 소관
        self.sock.sendall(f"REQ|ASK|{line}\n".encode())
```

### 1.3 LLM_REQ 처리 — Solar 호출 대행

수신 스레드가 라인을 파싱하다 `LLM_REQ|`로 시작하면 워커 스레드를 띄운다
(Solar 호출이 수 초 걸리므로 수신 스레드를 막지 않기 위함):

```python
def _handle_llm_req(self, prompt):
    action = self.llm(prompt)              # mock 또는 Solar
    wire = ACTION_TABLE[action["action"]](action)   # JSON → "CMD|arg"
    self.sock.sendall(f"REQ|LLM_RESP|{wire}\n".encode())
```

`ACTION_TABLE`은 JSON 액션을 와이어 명령으로 번역한다 (`REQ|`·`LLM_RESP|`
래핑은 위에서 처리). **총 8개 액션**, 모두 jail 안 `agentd` 가 실행하거나
(`exec`/`kill`) 커널 `agent_blocked()` 가 차단:

| JSON action                                            | 번역 결과              | 실행 경로                        |
| ------------------------------------------------------ | ---------------------- | -------------------------------- |
| `{"action":"print","msg":"hi"}`                        | `PRINT\|hi`            | jailed agentd → `printf`         |
| `{"action":"chat","msg":"hi back"}`                    | `CHAT\|hi back`        | jailed agentd → `printf`         |
| `{"action":"read","path":"/foo"}`                      | `READ\|/foo`           | jailed agentd → `open + read`    |
| `{"action":"write","path":"/foo","content":"data"}`    | `WRITE\|/foo\|data`    | jailed agentd → `open + write`   |
| `{"action":"ls","path":"/"}`                           | `LS\|/`                | jailed agentd → `dirent` 순회    |
| `{"action":"ps"}`                                      | `PS`                   | jailed agentd → stub (sys_proclist 후속) |
| `{"action":"kill","pid":7}`                            | `KILL\|7`              | jailed agentd → `kill` → **agent_blocked 차단** |
| `{"action":"nice","pid":5,"priority":3}`               | `NICE\|5:3`            | jailed agentd → `setpriority`    |

Solar 응답이 `JSONDecodeError`이거나 액션을 모르면 `PRINT|(llm error)` 등으로
폴백 — 절대 크래시하지 않는다.

### 1.5 역할(role) 토글 — REPL `:role`

agent.py REPL은 현재 role 을 보관(`self.role`, 기본 `reader`)하고 매 입력에
prefix 를 부착해 송신한다:

```python
self.sock.sendall(f"REQ|agent:{self.role}|ASK|{line}\n".encode())
```

REPL 내 `:role writer` / `:role admin` / `:role reader` 로 토글. role 값은
**진단·로깅용** 으로 유지된다 — 실제 sandboxing 의 경계는 **jail** (chroot +
`agent_blocked()` syscall 가드) 이다. role 은 자유로이 바꿀 수 있지만, 위험한
syscall (`exec`/`kill`/`mknod`) 은 어떤 role 이든 jail 안에서 거부된다.

### 1.4 환경 설정 (`.env`)

```
UPSTAGE_API_KEY=up_xxxxxxxxxxxxxxxx
UPSTAGE_MODEL=solar-pro2
```

[.gitignore](.gitignore)에서 `.env`는 차단, [.env.example](.env.example)은 템플릿으로
커밋 가능. **키 노출 방지**가 Project_requirements.md §4의 필수 요구사항이다.

---

## 2. Phase 2 — CFS 스케줄러

### 2.1 구조체 확장 ([kernel/proc.h](xv6-riscv/kernel/proc.h))

```c
struct proc {
  // ... 기존 필드 ...
  int priority;          // 기존 (0=최상, 20=최하) → CFS의 nice 가중치로 재해석
  uint64 vruntime;       // ★ 신규: 누적 가상 실행 시간
  uint creation_tick;    // ★ 신규: 동률 시 타이브레이커
};
```

### 2.2 `min_vruntime_skip()` 헬퍼 ([kernel/proc.c](xv6-riscv/kernel/proc.c))

`RUNNABLE+RUNNING` 상태의 모든 프로세스 중 최소 vruntime을 반환. 호출자가
이미 어떤 프로세스의 락을 쥐고 있으면 `skip` 인자로 제외하여 재진입 데드락을
방지.

```c
static uint64 min_vruntime_skip(struct proc *skip) {
  uint64 m = (uint64)-1;
  for (struct proc *q = proc; q < &proc[NPROC]; q++) {
    if (q == skip) continue;
    acquire(&q->lock);
    if ((q->state == RUNNABLE || q->state == RUNNING) && q->vruntime < m)
      m = q->vruntime;
    release(&q->lock);
  }
  return (m == (uint64)-1) ? 0 : m;
}
```

### 2.3 생성·정리 시 초기화

- `allocproc()` — 새 프로세스: `vruntime = min_vruntime_skip(p)`, `creation_tick = ticks`
- `freeproc()` — 해제: `vruntime = 0`, `creation_tick = 0`

이로써 fork된 자식이 부모와 같은 vruntime에서 출발해 즉시 starve하지 않는다.

### 2.4 스케줄러 본체 — `scheduler()` 단일 패스

`priority.patch`의 **2-pass 우선순위 라운드로빈**을 **1-pass min-vruntime 선택**으로
대체했다. 결정 규칙:

1. RUNNABLE 프로세스 중 `vruntime`이 가장 작은 것 선택
2. 동률이면 `creation_tick`이 더 작은(=먼저 생긴) 프로세스 선택

선택된 프로세스의 락만 유지한 채 `swtch()`로 전환 (기존 xv6 관용구 그대로).

### 2.5 timer-tick 시 vruntime 가산 ([kernel/trap.c](xv6-riscv/kernel/trap.c))

`usertrap()` & `kerneltrap()`의 두 timer-yield 경로 모두에서:

```c
if (which_dev == 2) {
  struct proc *cp = myproc();
  if (cp) {
    acquire(&cp->lock);
    cp->vruntime += (uint64)(cp->priority + 1);  // 가중치: 1~21 / tick
    release(&cp->lock);
  }
  yield();
}
```

- priority 0(최상) → +1/tick (느리게 누적) → 자주 선택 → CPU 많이 점유
- priority 20(최하) → +21/tick (빠르게 누적) → 가끔 선택
- 하지만 최하 우선순위도 결국 선택됨 → **굶주림 없음**

**정수 연산만 사용**. `uint64`라 약 10^17년 후에야 오버플로우.

### 2.6 wakeup 시 vruntime floor

오래 SLEEP한 프로세스가 깨어났을 때 작은 vruntime 때문에 CPU를 독점하는 것을
방지하기 위해 `wakeup()`을 수정 — 깨어나는 프로세스의 vruntime을 현재 최소값으로
끌어올린다. Linux CFS `place_entity()`의 단순화.

---

## 3. Phase 4 — 커널 내부 명령어 디스패처

### 3.1 2단계 설계 — enqueue / drain

디스패처는 **인터럽트 컨텍스트에서 fs 작업(`begin_op` → `sleep`)을 호출할 수
없다**는 제약 때문에 두 단계로 나뉜다 ([kernel/agentcmd.c](xv6-riscv/kernel/agentcmd.c)):

```
 [인터럽트 컨텍스트]                  [프로세스 컨텍스트]
 consoleintr()                        usertrap() / consoleread()
      │                                     │
      ▼                                     ▼
 agent_dispatch()  ──► 링 큐 ──►  agent_drain() ──► agent_dispatch_now()
 (라인 enqueue만,                  (큐 dequeue 후                (실제 파싱·실행)
  sleep 불가)                       각 라인 처리)
```

- `agent_dispatch()` — 인터럽트 안전. 라인을 `AGENT_Q_LEN=8` 링 버퍼에 복사만 함.
- `agent_drain()` — `myproc()`이 유효한 컨텍스트에서 호출. 큐를 비우며 디스패치.
- drain 호출 지점: `usertrap()`의 timer-yield 직후, `consoleread()`의 sleep 루프 직전.

### 3.2 명령어 ([kernel/agentcmd.c](xv6-riscv/kernel/agentcmd.c))

`agent_dispatch_now()`가 와이어 형식을 파싱한다. 두 가지 prefix 형태 지원:

```
REQ|<CMD>|<arg>                       # role 생략
REQ|agent:<role>|<CMD>|<arg>          # role 라벨 (진단용)
```

라우팅 결정은 **메타-명령 vs 일반 명령**:
- ASK/LLM_RESP/CACHE_GET/CACHE_SET — **in-kernel 처리** (`handle_*`). 캐시
  (cache.c) 가 커널 메모리이므로 불가피.
- 그 외 모든 명령은 **`forward_to_agentd()` 로 agentq 에 enqueue**. jailed
  agentd 가 `sys_agent_recv()` 로 pull 한 뒤 자기 chroot 안에서 실행.

| 명령어                              | 동작                                                                | 실행 위치                                |
| ----------------------------------- | ------------------------------------------------------------------- | ---------------------------------------- |
| `REQ\|ASK\|<프롬프트>`              | **오케스트레이션 진입점** — 캐시 조회 + (필요 시) LLM_REQ           | 커널 in-kernel (메타)                    |
| `REQ\|LLM_RESP\|<CMD>\|<arg>`       | agent.py의 Solar 응답 — 캐시 저장 + agentq 로 forward               | 커널 in-kernel (메타)                    |
| `REQ\|PRINT\|<msg>`                 | `printf("[agentd] %s\n", msg)`                                      | jailed agentd                            |
| `REQ\|CHAT\|<msg>`                  | `printf("[chat] %s\n", msg)`                                        | jailed agentd                            |
| `REQ\|READ\|<path>`                 | `open(path, O_RDONLY)` + `read` (chroot 안)                         | jailed agentd                            |
| `REQ\|LS\|<path>`                   | `open(".") + read(&dirent)` 순회 (chroot 안)                        | jailed agentd                            |
| `REQ\|PS`                           | stub — 사용자 공간 proc-list syscall 부재 (`sys_proclist` 후속)     | jailed agentd                            |
| `REQ\|WRITE\|<path>\|<content>`     | `open(O_CREATE\|O_RDWR) + write` (chroot 안)                        | jailed agentd                            |
| `REQ\|KILL\|<pid>`                  | `kill(pid)` 호출 시 커널 `agent_blocked()` 가 즉시 거부             | **차단** (jail boundary)                 |
| `REQ\|NICE\|<pid>:<n>`              | `setpriority(pid, n)` (kernel-class 음수 거부)                      | jailed agentd                            |
| `REQ\|CACHE_GET\|<key>`             | 캐시 조회 (수동 디버깅용)                                            | 커널 in-kernel (메타)                    |
| `REQ\|CACHE_SET\|<klen>:<key><val>` | 캐시 저장 (수동 디버깅용)                                            | 커널 in-kernel (메타)                    |
| 기타                                | jailed agentd: `unknown cmd 'X'`                                    | jailed agentd                            |

**실행 경로 — 두 단계 큐**:
- *agent_q* (stage 1, `AGENT_Q_LEN=8`): 인터럽트 → `agent_dispatch_now`. 모든 라인이 일단 여기로.
- *agentq* (stage 2, `AGENTQ_N=16`): kernel → jailed agentd. 메타가 아닌 라인만 여기로.

라인 forward 시 wire 는 `forward_to_agentd()` (REQ| 접두어 보존) 또는 cache hit 에서
`forward_wire_to_agentd()` (REQ| 재포장) 로 enqueue.

### 3.3 시리얼 감지 훅 ([kernel/console.c](xv6-riscv/kernel/console.c))

`consoleintr()`가 들어오는 문자를 **mirror buffer** `agent_buf[2048]`에 적재한다.

- `\n` 도착 시 라인이 `REQ|`로 시작하면: shell 입력 버퍼 되감기 → `agent_dispatch()`로
  큐 enqueue → `wakeup(&cons.r)`로 셸을 깨워 drain 컨텍스트 확보.
- `REQ|`가 아니면 기존 동작 그대로 (셸이 받음).
- `INPUT_BUF_SIZE`를 128 → **2048**로 확대 (긴 ASK/LLM_RESP 라인 수용).

### 3.4 등록

- [kernel/defs.h](xv6-riscv/kernel/defs.h): `agentinit`, `agent_dispatch`,
  `agent_dispatch_now`, `agent_drain` 프로토타입
- [kernel/main.c](xv6-riscv/kernel/main.c): `agentinit()` 부팅 시 호출
- [Makefile](xv6-riscv/Makefile): `OBJS`에 `$K/agentcmd.o`

---

## 4. Phase 3 — LLM 응답 캐시 + 디스크 스왑

**파일:** [kernel/cache.c](xv6-riscv/kernel/cache.c) (신규)

같은 프롬프트의 Solar API 재호출을 우회한다. AIOS의 *Memory + Storage Manager*에
해당.

### 4.1 키 해싱 — 임의 길이 프롬프트 수용

자연어 프롬프트는 길이가 가변적(수백~수천 바이트)이라 원문을 슬롯에 저장할 수
없다. **FNV-1a 64-bit 해시**로 8바이트로 압축한다:

```c
static uint64 fnv1a_64(const char *data, int len) {
  uint64 h = 0xcbf29ce484222325ULL;
  for (int i = 0; i < len; i++) {
    h ^= (uint64)(unsigned char)data[i];
    h *= 0x100000001b3ULL;
  }
  return (h == 0) ? 1 : h;   // 0은 "빈 슬롯" 마커라 회피
}
```

해시 충돌 확률은 1만 항목 기준 P < 10⁻¹¹로 무시 가능.

### 4.2 2-계층 저장

| 계층   | 구조                                            | 용량                  |
| ------ | ----------------------------------------------- | --------------------- |
| RAM    | `cache_entry[16]` (해시+vlen+값1024B) ≈ 16.5 KB | 16 슬롯               |
| 디스크 | `/cache.bin` — 1034B 고정 레코드 append-only    | ~4,000 엔트리 (4 MB)  |

값 상한(`CACHE_VAL`)이 1024 B 인 이유: Solar Pro 한국어 자연어 응답이
50~200 글자 (UTF-8 150~600 B) 인 케이스를 99% 수용. 256 B 던 초기 설정은 50 글자
이상의 한국어 응답을 침묵 거부했음 — §9.3 버그 5 참고.

### 4.3 디스크 I/O sleeplock + 정적 버퍼

`CACHE_VAL=1024` 로 키우면 `cache_get/cache_set/disk_scan/disk_append` 의 로컬
1 KB 버퍼들이 호출 체인을 합산해 4 KB 커널 스택을 초과한다 (§9.3 버그 4
재발 위험). 해결:

- `tmp_val_buf`(cache_get RAM-miss 경로), `rec_buf`(disk_scan/append) 를 파일-static 으로 이동
- `disk_io_lock` sleeplock 으로 두 버퍼를 공유 직렬화 — disk 작업은 `begin_op` 가
  sleep 가능해 sleeplock 적합
- cache_get 의 promote 경로는 cache_set 재호출 대신 인라인 RAM-only 삽입으로 변경 —
  sleeplock 재진입 deadlock 회피

### 4.4 LRU + 디스크 스왑

- **`cache_set`**: RAM에 같은 키 있으면 덮어쓰기 / 빈 슬롯 있으면 채움 /
  모두 full이면 `last_access`가 가장 오래된 슬롯을 `/cache.bin`에 append 후
  그 자리에 새 엔트리 기록.
- **`cache_get`**: RAM hit → 즉시 반환. RAM miss → `/cache.bin` 순차 스캔 →
  디스크 hit 시 RAM 빈 슬롯에 promote (없으면 다음 디스크 hit 으로 미룸).
- 디스크 작업(`begin_op`/`writei`/`readi`)은 **`cache_lock` 해제 후** 수행 —
  스핀락 보유 중 sleep 방지.

### 4.5 시스템 콜 (userspace 노출)

5개 파일 수정으로 syscall 2개 추가:

- `int set_cache(const char *key, int klen, const char *val, int vlen)`
- `int get_cache(const char *key, int klen, char *valbuf, int vbuflen)`

수정 파일: `syscall.h`, `syscall.c`, `sysproc.c`, `usys.pl`, `user.h`.
`/cache.bin` 생성을 위해 [sysfile.c](xv6-riscv/kernel/sysfile.c)의 `create()`를
`static` → 전역으로 노출.

### 4.6 단독 시연 — `user/cache_test.c`

set/get/miss/overwrite/eviction/disk-promote/long-key/bad-args 8개 시나리오,
총 10개 단언. 셸에서 `cache_test` 실행.

### 4.7 커널 주관 오케스트레이션 (`REQ|ASK` / `REQ|LLM_RESP`)

캐시 사용의 **결정 로직이 커널 안에** 있다 — agent.py는 의사결정을 하지 않는다.
캐시 히트 시 wire 는 *agentd 큐로 forward* 되어 jail 안에서 실행된다.

**`REQ|ASK|<프롬프트>` 처리** ([agentcmd.c](xv6-riscv/kernel/agentcmd.c)):
```c
int clen = cache_get_exact(prompt, plen, cached, sizeof(cached));
if (clen >= 0) {                    // 캐시 HIT (exact)
    printf("[cache] HIT\n");
    forward_wire_to_agentd(cached); // 캐시된 "CMD|arg" 를 REQ| 래핑 후 agentd 큐로
} else if ((clen = cache_get_semantic(...)) >= 0
           && (CHAT/PRINT prefix)) {
    printf("[cache] SEMANTIC HIT score=%d/64\n", score);
    forward_wire_to_agentd(cached); // 의미 히트는 CHAT/PRINT 응답에만 허용 (부작용 0)
} else {                            // 캐시 MISS
    copy_line(pending_prompt, prompt, ...);  // 프롬프트 보관 (캐시 키로 쓸 것)
    printf("LLM_REQ|%s\n", prompt);          // agent.py에 Solar 호출 요청
}
```

**`REQ|LLM_RESP|<CMD>|<arg>` 처리**:
```c
cache_set(pending_prompt, pending_len, wire, wire_len);  // 결과를 캐시에 저장
forward_wire_to_agentd(wire);                            // wire 를 agentd 큐로
```

비동기 단일-슬롯 설계: `pending_prompt` 정적 변수. agent.py REPL이 직렬이라
동시에 둘 이상의 요청이 진행되지 않으므로 안전. (과거 `pending_role` 는 jail 통합
으로 의미가 사라져 제거됨 — role 은 wire 진단 라벨로만 남는다.)

---

## 5. Phase 5 — Jail 기반 sandboxing

**파일:** [kernel/syscall.c](xv6-riscv/kernel/syscall.c) (agent_blocked),
[kernel/sysfile.c](xv6-riscv/kernel/sysfile.c) (sys_jail),
[kernel/fs.c](xv6-riscv/kernel/fs.c) (namex chroot 훅),
[kernel/proc.h](xv6-riscv/kernel/proc.h) (`is_agent` + `jail_root`),
[user/agentd.c](xv6-riscv/user/agentd.c) (jailed runtime),
[user/agentdemo.c](xv6-riscv/user/agentdemo.c) (4 시연 케이스).

project.md Direction A의 핵심 키워드 *"tool-use sandbox / file/shell tool
permissions / secure execution sandbox the LLM can call into"* 의 정합 메커니즘.
**프로세스 단위 격리** + **위험 syscall 의 커널 거부** 두 축.

> **역사적 맥락**: PR 1 통합 이전, Sejoong 브랜치에는 *역할 기반 ACL 시제품*
> (reader/writer/admin × 8 액션 비트 마스크) 가 있었다. 그 가드는 jail 모델로
> 흡수됐고, 측정 도구 (`eval acl`) 와 `agent:<role>` wire 라벨은 진단·시연용으로
> 남는다. 아래는 *현재 통합본의 jail 모델* 만 서술.

### 5.1 진입 — `init → fork → exec(agentd) → jail()`

xv6 부팅 시퀀스에 jailed agent 데몬을 자동 기동:

```c
// user/init.c
pid = fork();
if (pid == 0) {
    char *aargv[] = { "agentd", 0 };
    exec("agentd", aargv);       // → user/agentd.c main()
    exit(1);
}
```

```c
// user/agentd.c
mkdir(JAIL);                     // "/agentbox"
if (jail(JAIL) < 0) exit(1);     // 이 시점부터 '/' == /agentbox
while (agent_recv(line) >= 0)    // jailed: 큐에서 명령 pull
    execute(line);
```

`jail()` 은 **단방향** — 한번 들어가면 못 나옴. 사용자 셸은 *jail 밖* 이므로
디버깅·관찰 가능하지만, LLM 발화 명령은 모두 jail 안에서 실행된다.

### 5.2 chroot 훅 — `kernel/fs.c::namex`

`is_agent==1` 인 프로세스는 절대 경로 `/foo` 를 `<jail_root>/foo` 로 재기점하고,
`..` 가 jail_root 위로 가는 시도를 차단:

```c
// fs.c namex 내부
if (myproc()->is_agent && myproc()->jail_root != 0)
    ip = idup(myproc()->jail_root);     // '/' → jail_root
// ...
if (eq(name, "..") && ip == myproc()->jail_root)
    continue;                            // '..' 탈출 차단
```

→ agentdemo 케이스 2: `open("/../init")` → `-1`.

### 5.3 위험 syscall 가드 — `kernel/syscall.c::agent_blocked`

```c
static int agent_blocked(int num) {
    return num == SYS_exec || num == SYS_kill || num == SYS_mknod;
}

void syscall(void) {
    int num = p->trapframe->a7;
    if (p->is_agent && agent_blocked(num)) {
        printf("[sandbox] pid %d (%s): syscall %d blocked\n", ...);
        p->trapframe->a0 = -1;
        return;
    }
    p->trapframe->a0 = syscalls[num]();
}
```

차단 3종 사유:
- `SYS_exec` — LLM 이 임의 바이너리로 자신을 교체하지 못하게 (jail 우회 방지)
- `SYS_kill` — 시스템 프로세스 (init, sh) 보호
- `SYS_mknod` — 디바이스 노드 생성 차단

### 5.4 검증 — `user/agentdemo.c` 4 케이스

```
1. open("/notes.txt", O_RDONLY)         → jail 내 파일 접근 OK
2. open("/../init", O_RDONLY)           → -1 (.. 탈출 차단)
3. setpriority(getpid(), -1)            → -1 (kernel-class 거부)
4. exec("/echo", argv)                  → -1 (agent_blocked)
```

부팅 후 `$ agentdemo` 로 실행. 위 4 줄이 모두 *OK* 로 출력되면 jail 통과.

### 5.5 userspace 진입로 — `sys_dispatch` 시스템 콜

다중 에이전트 데모와 평가지표 측정을 위해 userspace 에서 직접 디스패처에
와이어 라인을 던질 수 있는 syscall ([sysproc.c](xv6-riscv/kernel/sysproc.c)):

```c
int dispatch(const char *line);    // line = "REQ|agent:<role>|<CMD>|<arg>"
```

QEMU 시리얼 TCP 입력과 동일한 와이어 형식. 내부적으로 `agent_dispatch_now()`
를 부르고, 그 결과 메타-명령은 in-kernel 처리, 일반 명령은 agentd 큐로 forward.
5개 파일 plumbing(syscall.h/c, sysproc.c, usys.pl, user.h).

---

## 6. 평가지표 — `user/eval.c`

**파일:** [xv6-riscv/user/eval.c](xv6-riscv/user/eval.c)

Week 12 deliverable *"at least one evaluation metric defined"* 를 위해 3개 측정
서브커맨드:

### 6.1 `eval cache <N>` — 캐시 hit-rate

같은 N개 키를 두 라운드 조회. 1회차는 cold (cache miss → set), 2회차는 warm (hit 기대).
**관찰값**: 2N 시도 중 N hit → 50% (정상 동작).

```
$ eval cache 5
=== eval cache N=5 ===
  round 1 (cold) : miss=5  hit=0
  round 2 (warm) : miss=0  hit=5
  overall        : hits 5 / total 10 (50%)
```

### 6.2 `eval acl <N>` — ACL 거부율

각 시행에서 자식을 fork 해 pause 시키고, `reader×KILL` 을 dispatch.
ACL 이 막아서 자식이 살아 있으면 부모의 unrestricted `kill()` 이 성공
(반환 0) → "denied" 로 집계.

```
$ eval acl 3
[deny] role=reader cmd=KILL
[deny] role=reader cmd=KILL
[deny] role=reader cmd=KILL
  reader×KILL : denied 3 / 3 (100%)
```

### 6.3 `eval fair <ticks>` — CFS 공정성

3-CPU QEMU 에서 spinner 4개 (prio=0 둘, prio=20 둘) 를 동시 실행, 동일 tick window
종료 시 누적 work 단위 비교. 코어 수보다 spinner 가 많아 강제 경합 → priority
차이가 visible.

```
$ eval fair 20
=== eval fair target_ticks=20 (4 spinners on 3 CPUs) ===
  [A prio=0 ] work=9616 units in 20 ticks
  [B prio=0 ] work=9613 units in 20 ticks
  [C prio=20] work=5273 units in 20 ticks
  [D prio=20] work=5404 units in 20 ticks
```

→ priority-0 cohort 가 ≈1.8× 의 CPU 점유 — CFS 가 priority 를 vruntime 가중치
로 반영 함을 실증.

---

## 7. 다중 에이전트 데모 — `user/agent_multi.c`

**파일:** [xv6-riscv/user/agent_multi.c](xv6-riscv/user/agent_multi.c)

project.md *"multiple concurrent LLM 'processes' (agents) with fair CPU/memory/
tool-quota allocation"* 직접 정합.

`fork()` 로 4개 자식 생성, 각자 다른 role 부여:

```c
const char *roles[4] = {"reader", "writer", "reader", "admin"};
```

각 자식이 sys_dispatch 로 동일 시퀀스(CHAT → WRITE → PS → KILL) 발행. 4-tuple
출력에는:
- 모든 role 에서 `[chat]` 통과 (CHAT 허용)
- reader 의 WRITE/KILL → `[deny] role=reader cmd=...`
- writer 의 WRITE 통과 (`/multi_1.txt`, `/multi_3.txt` 작성 확인)
- writer 의 KILL → `[deny] role=writer cmd=KILL`
- admin 의 모든 명령 통과
- 자식들의 stdout 라인이 인터리브 — CFS 가 4 프로세스를 시분할 함을 시각화

```
$ agent_multi
=== Multi-agent demo: 4 concurrent agents, mixed roles ===
    reader[0]  writer[1]  reader[2]  admin[3]
  [agent 0 role=reader] starting (pid=15)
[chat] hello from agent 0
[deny] role=reader cmd=WRITE
[agent] pid=1 init SLEEP prio=10
...
[agent] wrote 18 bytes to /multi_1.txt
[deny] role=writer cmd=KILL
[agent] no such pid=999             # admin × KILL — ACL 통과 후 pid 없음 응답
```

---

## 8. 빌드 & 실행

### 8.1 의존성 (호스트)

```bash
apt install qemu-system-misc gcc-riscv64-linux-gnu python3-pip
pip install openai --break-system-packages --ignore-installed
```

### 8.2 xv6 빌드 + 에이전트 실행

```bash
# 터미널 1
cd /root/OS_Project/xv6-riscv && make clean && make qemu-agent

# 터미널 2
cd /root/OS_Project
set -a; source .env; set +a
python3 agent.py
```

`agent>` 프롬프트에서:

```
agent> hello world     # 1회차: ASK → 캐시 miss → LLM_REQ → Solar → 캐시 저장 → 실행
agent> hello world     # 2회차: ASK → [cache] HIT → 즉시 실행 (agent.py 미개입)
agent> kill 7          # ASK → LLM_REQ → LLM_RESP|KILL|7 → 실행
```

### 8.3 셸 모드 (단독 측정 / 데모)

```bash
cd /root/OS_Project/xv6-riscv && make qemu
# xv6 셸에서:
#   cache_test        — 캐시 단독 테스트 (10/10 PASS)
#   eval cache 10     — hit-rate 측정
#   eval acl 5        — ACL 거부율 측정
#   eval fair 20      — CFS 공정성 측정
#   agent_multi       — 다중 에이전트 ACL 데모
```

---

## 9. 검증 결과

### 9.1 커널 단위 테스트 (셸 모드)

| 테스트              | 결과                                                  |
| ------------------- | ----------------------------------------------------- |
| 커널 부팅 (smp 1/3) | panic 없음, `init: starting sh` 도달                  |
| `priority_test`     | Test 1/2/3 모두 PASSED                                |
| `cache_test`        | **10/10 PASS**                                        |
| `eval cache 5`      | round1 miss=5/hit=0, round2 miss=0/hit=5 (50%)        |
| `eval acl 3`        | denied 3/3 (**100%**) — `[deny]` 3건 출력             |
| `eval fair 20`      | prio=0 ≈9.6K units vs prio=20 ≈5.3K units (**~1.8×**) |
| `agent_multi`       | reader/writer/admin ACL 차등 동작, 4 자식 인터리브 출력|

### 9.2 통합 테스트 (agent.py + QEMU TCP)

| 케이스                                | 결과                                              |
| ------------------------------------- | ------------------------------------------------- |
| `ASK` 1회차 (캐시 miss)               | `LLM_REQ` 송출 → agent.py Solar 호출 → 디스패치    |
| `ASK` 2회차 (같은 프롬프트)           | `[cache] HIT` → 즉시 실행, **agent.py 미개입**     |
| `:role writer` 후 KILL 시도           | `[deny] role=writer cmd=KILL`                     |
| `:role admin` 후 KILL 시도            | `kkill` 수행 (pid 없으면 `no such pid`)           |
| `kill 1` (init 보호)                  | `[agent] DENY kill pid=1 (init/sh protected)`     |
| 캐시 HIT 시 agent.py 로그             | `LLM_REQ` 흔적 없음 — Solar 호출 0                 |

### 9.3 구현 중 발견·수정한 버그

1. **인터럽트 컨텍스트 패닉** — `cache_get`가 인터럽트 핸들러에서 `begin_op→sleep`
   시도, `myproc()==NULL`. → enqueue/drain 2단계 분리로 해결.
2. **drain 트리거 부재** — 셸이 자고 있어 프로세스 컨텍스트 미발생. → `REQ|` 도착 시
   `wakeup(&cons.r)`, `consoleread` sleep 루프에서 `agent_drain()` 호출.
3. **`REQ|` 접두어 누락** — agent.py가 `PRINT|...`를 접두어 없이 전송해 셸이
   받아 exec 실패. → 와이어 래핑 규칙 정리.
4. **커널 스택 오버플로우** — `agent_drain`의 2KB 버퍼 + `exec_wire`의 2KB 버퍼 +
   재귀 호출이 4KB 커널 스택 초과 → store page fault. → `exec_wire`가 새 버퍼·재귀
   대신 `exec_cmd`를 직접 호출하도록 리팩터링.
5. **`cache_set` 의 침묵 거부** — Solar Pro 한국어 응답이 `CACHE_VAL=256` 상한을
   초과(50자 이상)하면 `cache_set` 이 `-1` 을 반환하는데 `LLM_RESP` 핸들러가 그
   값을 무시. 같은 질문 재요청 시에도 캐시에 없어 매번 Solar 재호출. →
   (a) `CACHE_VAL` 256→1024, `DISK_MAX_BYTES` 1MB→4MB 대칭 4× 확장, (b) `cache_set`
   반환값 검사 후 실패 시 `[cache] DROP (oversized N B)` 진단 출력, (c) 1024 B
   버퍼들이 호출 체인에 누적되어 다시 4 KB 스택을 초과하므로 `tmp_val_buf` ·
   `rec_buf` 를 파일-static + `disk_io_lock` sleeplock 으로 직렬화, 핸들러를 별도
   함수(`handle_ask`/`handle_llm_resp`/`handle_cache_*`)로 추출해 `agent_dispatch_now`
   의 단일 프레임 누적 회피, `AGENT_LINE_MAX` 2048→1280 축소.

---

## 10. 알려진 한계 / 다음 마일스톤

### 10.1 평가 한계

- `eval fair` 는 `-smp 3` 에서 4개 spinner 로 강제 경합을 만들어 효과 가시화 — 코어
  수보다 spinner 가 적으면 시분할 효과가 거의 안 보임.
- 해시 충돌 시(P<10⁻¹¹) 잘못된 캐시 응답 가능 — 안전성 무영향, 정확도만 저하.
- `pending_prompt`+`pending_role` 단일 슬롯 — 동시 다중 ASK 시 키가 덮어써짐.
  REPL이 직렬이라 실사용상 무문제이나, agent.py 가 ASK 를 동시 발사하면 큐로 교체 필요.
- ACL 우회: agent.py REPL 의 `:role admin` 토글은 사용자가 자유로이 바꿀 수 있음.
  설계 의도(신뢰 경계 = LLM 출력) 명시 — 진짜 보안엔 커널이 role 을 부여해야 함.
- `agent_multi` 의 stdout 인터리브 — `printf` 가 라인-원자성을 보장하지 않아 4 자식의
  출력이 섞여 보임. 가독성 저하이나 의미상 무문제 (CFS 가 실제로 인터리브한 증거).

### 10.2 향후 확장 후보 (보류)

- **`SPAWN|<path>` 액션**: kfork+kexec 직접 호출, LLM 이 임의의 user 프로그램을
  도구로 실행. 자식 PID 반환, wait/zombie 회수 정책 필요.
- **경로별 ACL**: writer 가 `/tmp/` 만 쓰기 가능, `/etc/` 는 admin 만 등 path-prefix
  정책. 현재는 명령-수준 ACL.
- **다중 agent.py TCP 동시 연결**: 현재 QEMU 시리얼은 단일 TCP — proxy/multiplex
  layer 필요.

---

## 11. 파일 변경 요약

| 파일                                                                | 변경                                          | Phase     |
| ------------------------------------------------------------------- | --------------------------------------------- | --------- |
| [xv6-riscv/kernel/proc.h](xv6-riscv/kernel/proc.h)                  | 수정 (필드 2개)                               | 2         |
| [xv6-riscv/kernel/proc.c](xv6-riscv/kernel/proc.c)                  | 수정 (CFS 본체)                               | 2         |
| [xv6-riscv/kernel/trap.c](xv6-riscv/kernel/trap.c)                  | 수정 (timer+drain)                            | 2·4       |
| [xv6-riscv/kernel/console.c](xv6-riscv/kernel/console.c)            | 수정 (REQ 감지+drain)                         | 4         |
| [xv6-riscv/kernel/agentcmd.c](xv6-riscv/kernel/agentcmd.c)          | **신규/대폭 확장** (디스패처 + 오케 + ACL + 5개 액션) | 3·4·5 |
| [xv6-riscv/kernel/cache.c](xv6-riscv/kernel/cache.c)                | **신규** (LLM 캐시)                           | 3         |
| [xv6-riscv/kernel/sysfile.c](xv6-riscv/kernel/sysfile.c)            | 수정 (`create` 노출)                          | 3         |
| [xv6-riscv/kernel/sysproc.c](xv6-riscv/kernel/sysproc.c)            | 수정 (cache + dispatch syscalls)              | 3·5       |
| `syscall.{c,h}`, `user.h`, `usys.pl`                                | 수정 (syscall 등록 — cache 2, dispatch 1)     | 3·5       |
| [xv6-riscv/kernel/defs.h](xv6-riscv/kernel/defs.h)                  | 수정 (프로토타입)                             | 3·4       |
| [xv6-riscv/kernel/main.c](xv6-riscv/kernel/main.c)                  | 수정 (init 호출)                              | 3·4       |
| [xv6-riscv/Makefile](xv6-riscv/Makefile)                            | 수정 (OBJS·UPROGS — `_eval`, `_agent_multi`)  | 3·4·5     |
| [xv6-riscv/user/cache_test.c](xv6-riscv/user/cache_test.c)          | **신규** (캐시 단독 테스트)                    | 3         |
| [xv6-riscv/user/eval.c](xv6-riscv/user/eval.c)                      | **신규** (cache/acl/fair 평가지표)             | 5         |
| [xv6-riscv/user/agent_multi.c](xv6-riscv/user/agent_multi.c)        | **신규** (다중 에이전트 ACL 데모)              | 5         |
| [agent.py](agent.py)                                                | **확장** (8개 액션, `:role` 토글)              | 1·3·5     |
| [.env.example](.env.example), [.gitignore](.gitignore)              | **신규**                                      | 1         |
