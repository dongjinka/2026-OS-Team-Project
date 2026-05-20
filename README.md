# OS for LLM — xv6 기반 에이전트 런타임

**Direction A — OS for LLM** (project.md §2). xv6-riscv 운영체제 위에 LLM 에이전트
런타임을 구현했다. Upstage Solar Pro 를 호스팅·지휘하며, AIOS 논문의 핵심 컴포넌트
(Agent Scheduler, Tool Manager, Memory/Storage Manager, Tool ACL Sandbox)를 xv6
커널에 직접 구현하여, 자연어 입력을 운영체제 액션으로 번역·실행한다.

## 핵심 기능

- **8개 액션 도구**: `PRINT`/`CHAT`/`READ`/`LS`/`PS`/`WRITE`/`KILL`/`NICE` — LLM이
  자연어를 JSON 액션으로 번역하면 커널 디스패처가 실제 동작 수행
- **역할 기반 ACL 샌드박싱** (Phase 5): reader/writer/admin 3-role 정적 정책,
  `[deny]` 거부 로그
- **LLM 응답 캐시 + 디스크 스왑**: FNV-1a 64-bit 해시 키, 16 슬롯 RAM LRU + 1 MB
  `/cache.bin` 디스크 오버레이 — 동일 프롬프트 재호출 비용 0
- **CFS 스케줄러**: `vruntime` + priority 가중치, 다중 에이전트 공정 분배
- **커널-주관 오케스트레이션**: 캐시 조회·LLM 호출 판단·디스패치 모두 xv6 커널
  안에서 — `agent.py` 는 Solar API 프록시 (의사결정 0)
- **다중 에이전트 데모**: 4개 자식 프로세스가 서로 다른 role 로 동시 작동,
  CFS 시분할 시각화
- **평가지표 측정**: cache hit-rate / ACL deny-rate / CFS fairness — `eval` 셸 명령

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
 ┌─────────────────────────┐                  ┌──────────────────────┐
 │ 사용자 (agent.py REPL)  │ ── REQ|ASK|... ──┤  xv6 커널 (QEMU)     │
 │ 자연어 + :role 토글     │ ←── LLM_REQ ─────┤                      │
 └────────────┬────────────┘                  │  agent_dispatch_now  │
              │ HTTPS                         │   ├─ ACL check       │
              ▼                               │   ├─ cache_get       │
 ┌─────────────────────────┐                  │   └─ exec_cmd        │
 │  Upstage Solar Pro 3    │                  │                      │
 │  api.upstage.ai/v1      │                  │ CFS scheduler        │
 └─────────────────────────┘                  │ /cache.bin (디스크)  │
                                              └──────────────────────┘
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
- `7번 죽여` — KILL 액션 (reader 는 거부됨)
- `:role admin` 후 `7번 죽여` — ACL 통과
- `/test.txt 에 hi 라고 써줘` — WRITE 액션 (reader 거부, writer/admin 통과)

### 방법 2 — 셸 모드 (자동 테스트 / 데모)

```bash
cd xv6-riscv && make qemu
```

xv6 셸 (`$ `) 에서:

```bash
$ cache_test           # 캐시 단위 테스트 (10/10 PASS)
$ eval cache 10        # 캐시 hit-rate 측정
$ eval acl 5           # ACL 거부율 측정 (100%)
$ eval fair 20         # CFS 공정성 측정 — prio=0 cohort ≈1.8× 더 많은 CPU
$ agent_multi          # 4개 동시 에이전트, mixed role, ACL 차등 데모
```

## 데모 (셸 모드 출력 예시)

### `eval cache 5`

```
=== eval cache N=5 ===
  round 1 (cold) : miss=5  hit=0
  round 2 (warm) : miss=0  hit=5
  overall        : hits 5 / total 10 (50%)
```

### `eval acl 3`

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

### `agent_multi`

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
    │   ├── agentcmd.c               ← 디스패처 + ACL + 8개 액션 핸들러
    │   ├── cache.c                  ← LLM 응답 캐시 + 디스크 스왑
    │   ├── proc.{c,h}               ← CFS 스케줄러
    │   ├── console.c                ← REQ| 시리얼 감지
    │   ├── sysproc.c                ← sys_set_cache/get_cache/dispatch
    │   └── ...                      ← 기존 xv6 kernel
    └── user/
        ├── cache_test.c             ← 캐시 단위 테스트 (10/10 PASS)
        ├── eval.c                   ← 평가지표 측정 프로그램
        ├── agent_multi.c            ← 다중 에이전트 데모
        └── ...                      ← 기존 xv6 user
```

## 라이선스

xv6-riscv 는 MIT 라이선스. 이 프로젝트의 추가 코드도 동일 라이선스.
