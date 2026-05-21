## Weekly_Development_Process

### Week 09 (완료): 팀 빌딩 및 주제 선정

요구사항: 팀 결성, 방향성(Direction A) 확정, 1문단 제안서 작성.

실제 진행: LLM 전용 OS 함수(샌드박싱, 캐싱, Priority 스케줄러) 및 LoRA 학습 활용이라는
구체적인 아이디어 도출 완료.

---

### Week 10 (완료): 시스템 설계 및 핵심 기능 구현

요구사항: 문제 정의 및 시스템 스케치(블록 다이어그램 포함), 적용할 핵심 OS 개념 명시.

실제 진행: 설계를 넘어 핵심 기능 구현까지 완료 (Implementation.md 참고).

- **시스템 아키텍처 확정**: agent.py(Python 브릿지) ↔ QEMU TCP 시리얼(4444) ↔ xv6 커널.
  AIOS 논문의 Agent Scheduler / Tool Manager / Memory·Storage Manager 컴포넌트를
  xv6에 매핑.
- **Phase 1 — 통신 브릿지**: `agent.py` 작성. Upstage Solar API 연동(mock/solar 자동
  전환), 자연어 → JSON → `REQ|CMD|arg` 와이어 변환, 백그라운드 수신 스레드.
- **Phase 2 — CFS 스케줄러**: `struct proc`에 `vruntime`·`creation_tick` 추가, 기존
  priority RR을 min-vruntime 단일 패스 스케줄러로 교체, timer-tick nice 가중 가산,
  wakeup vruntime floor. → `priority_test` Test 1/2/3 통과.
- **Phase 4 — 커널 명령어 디스패처**: `consoleintr()`에서 `REQ|` 라인 감지,
  `agentcmd.c`에서 PRINT/KILL/NICE 처리, init·sh(pid≤2) 보호.

핵심 OS 개념: 프로세스 스케줄링(CFS), 시스템 콜, IPC(시리얼), 동기화(스핀락).

---

### Week 11 (완료): 최소 기능 프로토타입 (MVP) + LLM 캐시

요구사항: 최소 기능 프로토타입 — LLM 호출 end-to-end 동작, OS 컴포넌트 연동.

실제 진행:

- **Phase 3 — LLM 응답 캐시 + 디스크 스왑**: `kernel/cache.c` 신설. FNV-1a 64-bit
  해시 키(임의 길이 프롬프트 수용), RAM 16슬롯 LRU + `/cache.bin` 디스크 오버레이,
  `sys_set_cache`/`sys_get_cache` 시스템 콜, `cache_test` 단독 시연(10/10 PASS).
- **end-to-end 통합**: agent.py REPL이 LLM 호출 전 커널 캐시를 먼저 조회 →
  같은 프롬프트 재입력 시 Solar API 우회. `agent.py ↔ xv6` 전 경로 검증 완료.
- **이슈 및 해결**:
  1. 인터럽트 컨텍스트에서 fs 작업 호출 시 커널 패닉 → 디스패처를 enqueue/drain
     2단계로 분리.
  2. 셸이 sleep 상태라 drain 트리거 부재 → `REQ|` 도착 시 셸 wake +
     `consoleread` sleep 루프에서 drain.
  3. agent.py 디스패치 와이어에 `REQ|` 접두어 누락 → `ACTION_TABLE` 수정.

핵심 OS 개념 추가: 파일 시스템/스토리지(디스크 스왑), 캐시 교체 정책(LRU).

---

### Week 12 (진행 중, 2026-05-21 기준): 통합 프로토타입 완성 및 평가 지표 세팅

요구사항: CFS 공정성 정량 측정 프로그램 작성(priority별 CPU 점유율), 캐시 hit rate /
LLM 호출 절감률 측정. Phase 5(jail 기반 샌드박싱) 착수.

실제 진행:

- **Phase 5 — jail 기반 샌드박싱 (방향 확정, 통합 진행 중)**: 팀의 통합
  sandboxing 모델로 **`jail()` syscall + jailed agent 데몬** (main 브랜치
  SeungBeom 작업, `kernel/sysfile.c`·`kernel/fs.c`·`user/agentd.c`) 을 채택.
  LLM 발화 명령을 실행하는 에이전트 프로세스를 부팅 시 `init` 이 `fork` →
  `exec(agentd)` → `jail("/agentbox")` 로 *프로세스 단위 chroot* 안에 가두고,
  커널의 `agent_blocked()` 가 `SYS_exec`/`SYS_kill`/`SYS_mknod` 같은 위험
  syscall 을 항상 거부. 사용자 셸은 jail 밖이라 디버깅 가능, 에이전트는 jail
  안이라 호스트 파일계를 *볼 수도 없음*. → `agentdemo` 가 ① jail 내 파일
  접근 가능 ② `..` 로 탈출 시도 차단 ③ kernel-class 음수 우선순위 거부
  ④ `exec()` 차단 4가지를 시연.
  > 참고: Sejoong 브랜치 초기 단계에서 시제품으로 만든 *역할 토글
  > (`:role reader|writer|admin`) + `[deny]` 가드* 는 jail 모델로 일원화하면서
  > "jail 안에서의 명령 화이트리스트" 로 흡수 예정. `eval acl N` 측정 도구는
  > jail 통합 후에도 *명령 허용/거부 통계* 용으로 그대로 활용 가능.

- **Semantic Cache 확장 (MinHash + Jaccard, commit `c56b028`)**: Week 11 의 정확
  매치 한계를 보완. 프롬프트를 byte 3-gram 으로 shingling 한 뒤 K=64 차원
  MinHash signature 를 RAM 슬롯에 저장. 쿼리 시 Jaccard ≥ 7/10 (정수 비교
  `match·10 ≥ 7·64`) 면 hit. **CHAT/PRINT prefix 응답에만** 의미 매칭을 허용해
  WRITE/KILL 같은 부작용 명령이 paraphrase 매치로 재실행되지 않도록 안전 가드.
  부동소수점 미사용, double hashing `g_k = h1 + k·h2` 로 shingle 당 FNV-1a 2회만
  계산해 K 배 가속.

- **다중 에이전트 동시 시연 + 동기화 검증**: `user/agent_multi.c` 로 4 자식이
  서로 다른 role/prompt 로 동시 dispatch → 디스패처 큐의 spinlock 직렬화 확인.
  `user/write_race.c` 로 4 writer 가 같은 파일에 동시 write → inode sleeplock
  으로 직렬화되는 모습을 가시화 (xv6 의 표준 inode lock 활용).

- **평가 프로그램 (`user/eval.c`)** — 4 종 정량 지표 셸 명령:
  - `eval cache N`     → 정확 매치 hit/miss 비율 + LLM 호출 절감률
  - `eval semantic N`  → exact / semantic / miss 3 컬럼, semantic_recall
  - `eval acl N`       → role 별 deny/allow
  - `eval fair N`      → priority 별 CPU 점유율(vruntime tick 누적)
  → Week 12 요구사항의 "캐시 hit rate / CFS 공정성 정량 측정" 직접 충족.

- **REPL UX 개선 (agent.py)**: 비동기 출력(`_emit_async`) 도중에도 readline
  입력버퍼를 보존하도록 prompt 라인 redraw. `_io_lock` 으로 동시 출력 직렬화.
  role 토글·`LLM_REQ` 응답 라우팅을 함께 정리.

- **팀 통합본(main) 분석**: SeungBeom 의 jail 기반 sandboxing + CFS 통합본을
  검토해 `BRANCH_COMPARISON.md`, `MAIN_BRANCH_DEEP_DIVE.md` 작성. *권한 격리*
  (main) 와 *요청 단위 ACL + 응답 캐싱* (Sejoong) 두 모델이 상호 배타가 아니라
  계층 합성 가능함을 확인 (jail 안에서 role + 캐시 운용).

- **다음 단계 설계 — CoW 적합성 평가**: 표준 페이지 CoW 는 현 워크로드
  (`init→exec` 지배, agent_multi footprint <100KB) 엔 ROI 부족으로 판단.
  대신 *Cache-Page Shared Mapping* (방안 B) 과 *Jail-Snapshot 블록 CoW*
  (방안 C — main 의 jail 에 rollback 부여) 를 향후 후보로 보존
  (`/root/.claude/plans/harmonic-wishing-castle.md`).

- **문서 정리**: `Project_Guide.md` §7.7 에 "MinHash + Jaccard 의미 캐시" 11
  소절 추가. `Implementation.md`·`README.md` 갱신. `.gitignore` 에 IDE/개인
  학습용 파일(`.vscode/`, `project.md`, `Project_Guide.md`) 정리.

남은 작업 (Week 13 으로 이월 후보):
- Sejoong ↔ main 머지 — jail (`sys_jail` + agentd) 위에 semantic cache 를 얹는
  2-축 통합. Sejoong 의 role/`[deny]` 가드는 jail 내부 명령 화이트리스트로
  흡수해 단일 sandboxing 축으로 정리.
- `eval semantic` 회귀 자동화 + paraphrase 코퍼스 확장
- 방안 B 또는 C 의 시제품 (선택사항, 발표 임팩트용)

핵심 OS 개념 추가: 프로세스 단위 격리 (`jail()` chroot + 위험 syscall 차단),
확률적 자료구조(MinHash signature) 의 커널 내장, sleeplock 기반 자원 직렬화,
정량 평가 워크로드 설계, 브랜치 통합 전략.

---

### Week 13: 시스템 최적화 및 리허설

계획: 평가 결과 정리, 최종 발표 dry-run.

---

### Week 14: 최종 발표 및 산출물 마감

요구사항: 영어 프레젠테이션, GitHub Public 레포지토리에 4가지 최종 산출물
(애플리케이션 코드/README, 기술 보고서, 개발 프로세스 문서, 영어 슬라이드) 업로드.
