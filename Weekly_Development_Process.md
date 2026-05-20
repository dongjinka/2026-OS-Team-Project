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

### Week 12: 통합 프로토타입 완성 및 평가 지표 세팅

계획: CFS 공정성 정량 측정 프로그램 작성(priority별 CPU 점유율), 캐시 hit rate /
LLM 호출 절감률 측정. Phase 5(역할 기반 샌드박싱) 착수.

---

### Week 13: 시스템 최적화 및 리허설

계획: 평가 결과 정리, 최종 발표 dry-run.

---

### Week 14: 최종 발표 및 산출물 마감

요구사항: 영어 프레젠테이션, GitHub Public 레포지토리에 4가지 최종 산출물
(애플리케이션 코드/README, 기술 보고서, 개발 프로세스 문서, 영어 슬라이드) 업로드.
