# OS for LLM — xv6 위의 자율 에이전트 런타임

**Direction A — OS for LLM.** xv6-riscv 위에 Upstage Solar(LLM)를 호스팅·지휘하는
에이전트 런타임을 구현한 2026 운영체제 팀 프로젝트. AIOS 논문의 세 핵심
컴포넌트(Agent Scheduler / Tool Manager / LLM Kernel Bridge)를 xv6 커널·사용자
프로그램·호스트 브릿지에 직접 이식해, **사람의 셸 입력은 그대로 두면서 LLM이
생성한 명령만 샌드박스 안에서 실행**한다. CFS 스케줄러·우선순위 시스템콜·chroot
jail·격리 워커(`agentd`)·ReAct 에이전트 루프가 한 줄로 연결된다.

| AIOS 컴포넌트     | 이 프로젝트 구현                                                |
| ----------------- | --------------------------------------------------------------- |
| Agent Scheduler   | **CFS 스케줄러** — Linux `sched_prio_to_weight` 이식, vruntime  |
| Tool Manager      | **agentd** + 커널 측 명령 큐 (`REQ\|CMD\|arg`) + 화이트리스트   |
| LLM Kernel Bridge | **agent.py** — Solar API ↔ QEMU TCP 시리얼, ReAct 루프          |

> 자세한 진행 현황·설계 근거는 [plan.md](plan.md), 모듈별 구현 세부는
> [Implementation.md](Implementation.md) 참조.

---

## 1. 핵심 기능

| # | 기능 | 상태 |
| - | ---- | ---- |
| F1 | `setpriority` / `getpriority` 시스템 콜 (`priority_test` 통과) | ✅ |
| F2 | user/kernel 우선순위 클래스 — 음수 priority = kernel-class (`init` = −5) | ✅ |
| F3 | CFS 본체 — Linux 가중치 테이블 + vruntime + 배열 스캔(min-vruntime) | ✅ |
| F4 | CFS 세부 — fork 상속, wakeup 보너스, 전역 `cfs_min_vruntime` | ✅ |
| F5 | QEMU ↔ Upstage Solar Python 브릿지 (`.env` 자동 로드) | ✅ |
| F6 | LLM 응답 JSON 역직렬화 (호스트 측 파싱) | ✅ |
| F7 | 샌드박싱 — chroot jail + `exec`/`kill`/`mknod` 차단 + 명령 화이트리스트 | ✅ |
| F8 | LLM이 다루는 도구별 priority 커스터마이즈 (`SETPRIO` / `LIST`) | ✅ |
| 보너스 | ReAct 자율 에이전트 루프 + 대화 메모리 | ✅ |
| F9 | LLM 응답 캐시 | 🟡 커널 구현 완료 (cache.c · `set_cache`/`get_cache` · 테스트 통과) — `agent.py` 연동만 남음 |
| F10 | 유휴 시간대 LoRA 학습 | ❌ (범위 외 — Future Work) |

---

## 2. 시스템 아키텍처

```
 ┌───────────────────┐      자연어 입력
 │  사용자 (REPL)    │ ─────────────────►
 └─────────┬─────────┘
           │
           ▼
 ┌─────────────────────┐  HTTPS  ┌────────────────────────┐
 │  agent.py           │────────►│  Upstage Solar Pro     │
 │  (ReAct 루프 +      │◄────────│  (api.upstage.ai/v1)   │
 │   대화 메모리)       │  JSON   └────────────────────────┘
 │  • {tool,args} 파싱  │
 │  • REQ|CMD|arg 변환  │
 └─────────┬───────────┘
           │ TCP 4444  (QEMU -serial tcp:)
           ▼
 ┌──────────────────────────────────────────────────────────────┐
 │                       xv6 커널                                │
 │                                                              │
 │   사람 입력  ─► sh (셸)                                       │
 │                                                              │
 │   REQ| 라인 ─► consoleintr ─► agent_dispatch                 │
 │                                  │  거부 목록(KILL/EXEC) 즉시 차단
 │                                  ▼                           │
 │                              ┌────────┐                       │
 │                              │ agentq │  ← 커널 링버퍼        │
 │                              └────┬───┘                       │
 │                                   │ sys_agent_recv (is_agent only)
 │                                   ▼                           │
 │   ┌──────────────────────────────────────────────────────┐   │
 │   │  agentd  (jailed: chroot=/agentbox, is_agent=1)       │   │
 │   │  ─────────────────────────────────────────────────    │   │
 │   │  도구 테이블(화이트리스트):                            │   │
 │   │    PRINT · READ · WRITE · LS · NICE · LIST · SETPRIO  │   │
 │   │  각 도구 실행 전 setpriority(self, fn.priority)       │   │
 │   └────────────────────┬─────────────────────────────────┘   │
 │                        ▼                                     │
 │   ┌──────────────────────────────────────────────────────┐   │
 │   │  CFS 스케줄러 — cfs_weight[41] · vruntime ·          │   │
 │   │  cfs_min_vruntime · fork 상속 · wakeup 보너스         │   │
 │   └──────────────────────────────────────────────────────┘   │
 └──────────────────────────────────────────────────────────────┘
```

---

## 3. 기술 스택

- **커널**: xv6-riscv (C, RISC-V 64), QEMU 7.2+
- **사용자 프로그램**: `agentd` (격리 에이전트 런타임), `agentdemo`(F2·F7 데모),
  `priority_test`(F1·F3 검증)
- **호스트 브릿지**: Python 3, [`openai`](https://pypi.org/project/openai/) SDK
  (Solar API는 OpenAI 호환)
- **LLM**: Upstage Solar Pro 2 (`UPSTAGE_MODEL=solar-pro2`)
- **호스트–게스트 통신**: QEMU `-serial tcp:127.0.0.1:4444,server,nowait`

---

## 4. 디렉터리 구조

```
OS_Project_main/
├── README.md                  # (이 문서)
├── Implementation.md          # 모듈별 상세 구현
├── plan.md                    # 진행 현황·평가 지표·남은 작업
├── Project_requirements.md    # 과제 요구사항 원문
├── agent.py                   # 호스트 측 LLM 에이전트 루프
├── .env.example               # API 키 템플릿 (.env 는 .gitignore 처리)
└── xv6-riscv/
    ├── Makefile               # qemu / qemu-agent 타깃
    ├── kernel/
    │   ├── proc.{c,h}         # CFS · 우선순위 · is_agent / jail_root
    │   ├── trap.c             # 타이머 vruntime 가산 (cfs_vdelta)
    │   ├── fs.c               # namex() chroot jail
    │   ├── syscall.{c,h}      # SYS_jail · SYS_agent_recv · agent 차단
    │   ├── sysfile.c          # sys_jail()
    │   ├── sysproc.c          # sys_setpriority 음수 가드, sys_agent_recv
    │   ├── agentcmd.c         # 명령 큐 + 거부 목록 게이트
    │   └── console.c          # REQ| 라인 감지 훅
    ├── user/
    │   ├── agentd.c           # ★ 격리 에이전트 런타임 (jail + 도구 테이블)
    │   ├── agentdemo.c        # ★ F2·F7 데모 프로그램
    │   ├── priority_test.c    # F1·F3 단위 테스트
    │   ├── init.c             # 부팅 시 agentd 자동 기동
    │   └── usys.pl, user.h    # jail/agent_recv 스텁
    └── mkfs/mkfs.c            # 디스크 이미지 빌더
```

---

## 5. Setup

### 5.1 사전 의존성 (Ubuntu / WSL2 기준)

```bash
sudo apt install qemu-system-misc gcc-riscv64-linux-gnu python3-pip make
pip install openai
```

### 5.2 저장소 클론

```bash
git clone <팀-저장소-URL> OS_Project_main
cd OS_Project_main
```

### 5.3 Upstage Solar API 키

1. [console.upstage.ai/docs](https://console.upstage.ai/docs) 에서 API 키 발급
   (강의에서 팀별로 배포된 키가 있으면 그것을 사용).
2. 템플릿을 복사해 `.env` 생성:

   ```bash
   cp .env.example .env
   # 편집기로 .env 를 열어 UPSTAGE_API_KEY=up_xxxxxxxx 채우기
   ```

3. **`.env` 는 절대 커밋하지 않는다.** `.gitignore` 에 이미 차단되어 있으나
   확인 권장. `agent.py` 가 스크립트 옆 `.env` 를 자동 로드한다 (실제
   환경변수가 있으면 그쪽이 우선).

키가 비어 있어도 `agent.py` 는 **mock 모드**(룰 기반 더미)로 실행되어 커널
경로만 점검할 수 있다.

### 5.4 커널 빌드

```bash
cd xv6-riscv
make clean
make qemu-agent   # 4444 포트에서 시리얼 수신 대기
```

`make qemu-agent` 는 QEMU 콘솔을 TCP 4444 로 노출한다. 일반 셸 상호작용이 필요할
때는 대신 `make qemu` 사용.

---

## 6. 실행 방법

### 6.1 LLM 에이전트 모드

터미널 두 개를 띄운다.

**터미널 1 — xv6 부팅**

```bash
cd xv6-riscv
make qemu-agent
```

**터미널 2 — 에이전트 브릿지**

```bash
python3 agent.py
```

기동 시 `[agent] mode = solar (solar-pro2)` 또는 `[agent] mode = mock` 로
모드를 알린다. `you ▸` 프롬프트에 자연어로 요청하면 LLM이 ReAct 루프로
계획·도구 호출·관찰을 반복한 뒤 답한다.

실증된 시나리오 예:

```
you ▸ /agentbox 안에 plan.txt 라는 파일을 하나 만들어줘
you ▸ 지금까지 만든 파일들 목록 보여줘
you ▸ 그 파일들 내용을 요약해줘
you ▸ 아까 CFS 얘기 나왔던 파일이 뭐였지?      # ← 대화 메모리에서 답변
```

종료는 `Ctrl-D` 또는 `Ctrl-C`. xv6 측은 `make qemu-agent` 터미널에서
`Ctrl-A X` 로 빠져나간다.

### 6.2 셸 단독 모드 (CFS / 샌드박스 데모)

```bash
cd xv6-riscv
make qemu
```

xv6 셸에서:

```
$ priority_test        # F1·F3·F4 우선순위/CFS 검증
$ agentdemo            # F2·F7 샌드박스 데모 (jail 진입 후 차단 시나리오)
```

`priority_test` 는 Test 1·2·3 모두 PASSED 가 떠야 한다. `agentdemo` 는 jail 내
read/write 성공 후 `..`·외부 경로·음수 priority·`exec`·`kill` 차단을 차례로 검증.

---

## 7. 검증 / 평가

| 검증 항목 | 결과 |
| --------- | ---- |
| 커널·`fs.img` 빌드, smp=1·smp=3 QEMU 부팅 | ✅ panic 없음 |
| `priority_test` Test 1/2/3 | ✅ 모두 PASSED |
| `agentdemo` 체크 5개 (jail read/write, `..` 차단, 음수 priority 거부, `exec`·`kill` 차단) | ✅ 모두 통과 |
| `REQ\|PRINT\|...`, `REQ\|KILL\|1`(거부), `REQ\|NICE\|2:5`, `REQ\|FOO\|bar`(unknown) | ✅ 기대 동작 |
| 실제 Solar API 멀티스텝 에이전트 시나리오 (write → ls → read × N → 요약 → 대화 메모리 활용) | ✅ 통과 |

세부 평가 지표(공정성·우선순위 효과·jail 격리·도구 호출 통계)는
[plan.md §4](plan.md) 참조.

---

## 8. 알려진 한계 / Future Work

- **F6 JSON 파싱 위치** — 현재는 호스트(`agent.py`)에서 파싱한다. 제안서
  원문은 "xv6 내부 구현"을 명시하므로 보고서에 설계 근거를 명시하거나
  `kernel/json.c` 미니 파서로 이식하는 옵션이 남아 있다 ([plan.md §5.1](plan.md)).
- **F9 LLM 응답 캐시** — 커널 측 구현 완료. `kernel/cache.c`(16-슬롯 RAM +
  `/cache.bin` 디스크 오버레이 + MinHash/Jaccard 의미 매칭), 시스템콜
  `set_cache`/`get_cache`(번호 29·30), `cache_test` 13/13 통과 (Se-Joong 원작
  `c56b028`을 `76b2737`에서 이식). 남은 작업은 `agent.py`가 Solar 호출 전
  `get_cache`로 조회해 히트 시 API 왕복을 생략하는 요청-경로 연동
  ([plan.md §5.2](plan.md)).
- **F10 유휴 시간대 LoRA 학습** — xv6(RISC-V, FP·디스크·메모리 극히 제한)에서
  실제 학습은 불가. 보고서 Future Work 로 분류, 필요 시 "유휴 tick 감지 →
  호스트 트리거 신호" 수준의 stub 가능.

---

## 9. 라이선스 / 출처

- xv6-riscv 원본은 MIT 라이선스 ([xv6-riscv/LICENSE](xv6-riscv/LICENSE)).
- 본 프로젝트의 자체 변경분도 동일하게 MIT 로 공개.
- 설계 모티프: Kai Mei et al., *AIOS: LLM Agent Operating System*, 2024.

---

## 10. 참조 문서

- [Implementation.md](Implementation.md) — 모듈별 구현 상세, 코드 인용, 와이어 프로토콜
- [plan.md](plan.md) — 요구사항 매핑, 진행 현황, 평가 지표, 남은 작업
- [Project_requirements.md](Project_requirements.md) — 과제 요구사항 원문
- [Weekly_Development_Process.md](Weekly_Development_Process.md) — 주차별 개발 과정
