# OS for LLM — xv6 기반 에이전트 런타임

**Direction A — OS for LLM** (project.md §2). xv6-riscv 운영체제 위에 LLM 에이전트
런타임을 구현했다. Upstage Solar Pro 를 호스팅·지휘하며, AIOS 논문의 핵심 컴포넌트
(Agent Scheduler, Tool Manager, Memory/Storage Manager, Jailed Agent Runtime)를 xv6
커널에 직접 구현하여, 자연어 입력을 운영체제 액션으로 번역·실행한다.

## 핵심 기능

- **8개 액션 도구**: `PRINT`/`CHAT`/`READ`/`LS`/`PS`/`WRITE`/`NICE`/`SETPRIO` — LLM이
  자연어를 JSON 액션으로 번역하면 jail 안의 `agentd` 가 실제 동작 수행
- **Jail 기반 샌드박싱** (Phase 5): `init` 이 `fork → exec(agentd) → jail("/agentbox")`
  로 에이전트를 *프로세스 단위 chroot* 안에 가둠. 위험 syscall (`exec`/`kill`/`mknod`)
  은 커널 `agent_blocked()` 가 거부. 사용자 셸은 jail 밖이라 진단 가능
- **LLM 응답 캐시 (의미 매칭)**: FNV-1a 64-bit 해시 정확 매치 + **MinHash 64-D
  signature** 기반 Jaccard ≥ 7/10 paraphrase 매칭. 16 슬롯 RAM LRU + 1 MB
  `/cache.bin` 디스크 오버레이. CHAT/PRINT 응답에만 semantic hit 허용
- **CFS 스케줄러**: `vruntime` + priority 가중치, 다중 에이전트 공정 분배
- **커널-주관 오케스트레이션**: 캐시 조회·LLM 호출 판단·디스패치 메타-명령은 xv6
  커널이 처리, 위험 명령 실행은 jail 안 agentd — `agent.py` 는 Solar API 프록시
  (의사결정 0)
- **다중 에이전트 데모**: 4개 자식 프로세스가 동시 dispatch, CFS 시분할 + inode
  sleeplock 직렬화 시각화
- **평가지표 측정**: cache hit-rate (exact + semantic) / CFS fairness — `eval` 셸 명령

## 기술 스택

| Layer            | Technology                                                  |
| ---------------- | ----------------------------------------------------------- |
| 운영체제         | xv6-riscv (MIT educational OS) + 직접 작성한 커널 모듈      |
| 가상화           | QEMU `qemu-system-riscv64` (`-machine virt`, smp=3, 128 MB) |
| 컴파일러         | `riscv64-linux-gnu-gcc`                                     |
| LLM 백엔드       | Upstage Solar Pro 3 (OpenAI-API compatible)                 |
| 호스트 브릿지    | Python 3 + `openai` SDK + TCP socket on `127.0.0.1:4444`    |
| 통신 프로토콜    | 라인 기반 텍스트: `REQ\|agent:<role>\|<CMD>\|<arg>\n`       |

## 시스템 구성도

```
 ┌─────────────────────────┐                  ┌──────────────────────────┐
 │ 사용자 (agent.py REPL)  │ ── REQ|ASK|... ──┤  xv6 커널 (QEMU)         │
 │ 자연어 + :role 토글     │ ←── LLM_REQ ─────┤                          │
 └────────────┬────────────┘                  │  agent_dispatch_now      │
              │ HTTPS                         │   ├─ meta? (ASK/         │
              ▼                               │   │   LLM_RESP/CACHE_*)  │
 ┌─────────────────────────┐                  │   │   → cache_get/set    │
 │  Upstage Solar Pro 3    │                  │   └─ else → agentq       │
 │  api.upstage.ai/v1      │                  │            ▼              │
 └─────────────────────────┘                  │  ┌─────────────────────┐ │
                                              │  │ jailed agentd       │ │
                                              │  │ (chroot /agentbox)  │ │
                                              │  │ agent_blocked:      │ │
                                              │  │   exec/kill/mknod   │ │
                                              │  └─────────────────────┘ │
                                              │  CFS scheduler           │
                                              │  /cache.bin (디스크)     │
                                              └──────────────────────────┘
```

자세한 설계·구현 디테일: [Implementation.md](Implementation.md)
초보자를 위한 가이드: [Project_Guide.md](Project_Guide.md)

## 사용 OS 개념

project.md 의 강제 조건 *"OS concepts must be present as a substantive part of
the design"* 를 충족하는 항목들:

- **프로세스 / 스케줄링** — CFS 스케줄러(vruntime + priority), `struct proc`,
  `fork()` / `wait()` / `kkill()`
- **시스템 콜** — `setpriority`, `getpriority`, `set_cache`, `get_cache`,
  `dispatch` (새로 추가)
- **동기화** — `spinlock` (cache_lock, agent_q_lock), `sleeplock` (디스크 I/O),
  `sleep`/`wakeup`
- **파일 시스템 / 저장소** — `/cache.bin` 디스크 오버레이, WRITE/READ/LS 액션이
  `begin_op`/`create`/`namei`/`readi`/`writei` 호출
- **IPC** — QEMU UART 직렬 ↔ TCP 4444 ↔ Python 브릿지
- **인터럽트 컨텍스트** — `consoleintr` 가 인터럽트에서 enqueue, `consoleread`
  가 process 컨텍스트에서 drain (2단계 설계)

## 설치

### 호스트 의존성

```bash
apt install qemu-system-misc gcc-riscv64-linux-gnu python3-pip
pip install openai --break-system-packages --ignore-installed
```

### Solar API 키 (선택 — 없으면 mock 모드 작동)

Upstage 콘솔에서 발급받은 키를 `.env` 에 저장:

```bash
cp .env.example .env
# 텍스트 에디터로 .env 열어서 UPSTAGE_API_KEY=up_xxxxx 입력
```

`.env` 는 `.gitignore` 로 차단됨 — **절대 커밋하지 않는다** (project.md §3).
키 없이도 mock 모드로 모든 기능 시연 가능.

## 실행

### 방법 1 — LLM 통합 모드 (자연어 입력)

두 터미널 필요:

```bash
# 터미널 1: xv6 부팅 + TCP 시리얼 노출
cd xv6-riscv && make clean && make qemu-agent

# 터미널 2: Python 브릿지 + REPL
set -a; source .env; set +a
python3 agent.py
```

`agent[reader]>` 프롬프트에서 자연어 입력:
- `hello world` — PRINT 액션으로 출력
- `/test.txt 에 hi 라고 써줘` — WRITE 액션 (jail 내 `/agentbox` 에 기록)
- `7번 죽여` — KILL 액션 → 커널 `agent_blocked()` 가 차단 (`exec`/`kill`/`mknod`
  는 jail 안에서 거부)
- `:role admin` 토글은 wire 의 `agent:<role>` 접두어를 바꿔 진단 로깅에 영향
  주지만, sandboxing 의 *실제 경계* 는 jail (chroot + syscall 가드)

### 방법 2 — 셸 모드 (자동 테스트 / 데모)

```bash
cd xv6-riscv && make qemu
```

xv6 셸 (`$ `) 에서:

```bash
$ cache_test           # 캐시 단위 테스트 (13/13 PASS — exact + semantic)
$ eval cache 10        # 캐시 hit-rate 측정 (exact)
$ eval semantic 50     # MinHash + Jaccard paraphrase recall 측정
$ eval acl 5           # (Sejoong 시제품) role-ACL 거부율 측정 — jail 통합 후
                       # 의미가 약해졌으나 측정 도구는 유지
$ eval fair 20         # CFS 공정성 측정 — prio=0 cohort ≈1.8× 더 많은 CPU
$ agent_multi          # 4개 동시 에이전트 dispatch 동시성 데모
$ agentdemo            # jail 시연 (chroot + 위험 syscall 차단 4 케이스)
$ write_race           # 4 writer 동시 write → inode sleeplock 직렬화 시각화
```

## 데모 (셸 모드 출력 예시)

### `eval cache 5`

```
=== eval cache N=5 ===
  round 1 (cold) : miss=5  hit=0
  round 2 (warm) : miss=0  hit=5
  overall        : hits 5 / total 10 (50%)
```

### `eval acl 3` *(Sejoong 시제품 시점 출력 — jail 통합 후엔 jail 의 syscall 가드가 동일 효과를 더 깊은 단에서 제공)*

```
=== eval acl N=3 ===
[deny] role=reader cmd=KILL
[deny] role=reader cmd=KILL
[deny] role=reader cmd=KILL
  reader×KILL : denied 3 / 3 (100%)
```

### `eval fair 20`

```
=== eval fair target_ticks=20 (4 spinners on 3 CPUs) ===
  [A prio=0 ] work=9616 units in 20 ticks
  [B prio=0 ] work=9613 units in 20 ticks
  [C prio=20] work=5273 units in 20 ticks
  [D prio=20] work=5404 units in 20 ticks
```

priority 0 cohort 가 priority 20 cohort 대비 약 **1.8 배의 CPU 점유** — CFS
가 priority 를 vruntime 가중치로 반영함을 실증.

### `agent_multi` *(Sejoong 시제품 시점 출력 — role-ACL 시연. jail 통합 후엔
KILL/exec 가 `agent_blocked()` 에서 차단되며 role 은 진단용으로 남음)*

```
=== Multi-agent demo: 4 concurrent agents, mixed roles ===
    reader[0]  writer[1]  reader[2]  admin[3]

[chat] hello from agent 0   ← CHAT 은 모든 role 허용
[chat] hello from agent 1
[chat] hello from agent 2
[chat] hello from agent 3
[deny] role=reader cmd=WRITE
[deny] role=reader cmd=WRITE
[agent] wrote 18 bytes to /multi_1.txt   ← writer 통과
[agent] wrote 18 bytes to /multi_3.txt   ← admin 통과
[deny] role=writer cmd=KILL
[agent] no such pid=999                   ← admin × KILL 통과 (pid 없음)
```

## 디렉토리 구조

```
OS_Project/
├── README.md                        ← 이 파일
├── Implementation.md                ← 기술 보고서 (Phase 1~5 상세)
├── Project_Guide.md                 ← 처음 보는 사람용 풀 가이드
├── Weekly_Development_Process.md    ← 개발 진행 기록
├── project.md                       ← 과제 명세
├── agent.py                         ← Python 브릿지 (Solar 프록시)
├── .env.example                     ← API 키 템플릿
└── xv6-riscv/
    ├── kernel/
    │   ├── agentcmd.c               ← 메타-디스패처 (ASK/LLM_RESP/CACHE_*) +
    │   │                              일반 명령을 jailed agentd 큐로 forward
    │   ├── cache.c                  ← LLM 응답 캐시 (exact + semantic) + 디스크 스왑
    │   ├── fs.c                     ← jail 의 chroot 훅 (namex)
    │   ├── proc.{c,h}               ← CFS 스케줄러 + jail_root / is_agent 필드
    │   ├── syscall.c                ← agent_blocked: exec/kill/mknod 차단
    │   ├── console.c                ← REQ| 시리얼 감지
    │   ├── sysproc.c                ← sys_jail / sys_agent_recv /
    │   │                              sys_set_cache / sys_get_cache / sys_dispatch
    │   └── ...                      ← 기존 xv6 kernel
    └── user/
        ├── cache_test.c             ← 캐시 단위 테스트 (10/10 PASS)
        ├── eval.c                   ← 평가지표 측정 프로그램
        ├── agent_multi.c            ← 다중 에이전트 데모
        └── ...                      ← 기존 xv6 user
```

## 라이선스

xv6-riscv 는 MIT 라이선스. 이 프로젝트의 추가 코드도 동일 라이선스.
