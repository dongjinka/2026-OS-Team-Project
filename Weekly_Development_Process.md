## Weekly_Development_Process

Week 09 (완료): 팀 빌딩 및 주제 선정



요구사항: 팀 결성, 방향성(Direction A) 확정, 1문단 제안서 작성.

실제 진행: LLM 전용 OS 함수(샌드박싱, 캐싱, Priority 스케줄러) 및 LoRA 학습 활용이라는 구체적인 아이디어 도출 완료. GitHub repo 생성 + 개인 브랜치 + PR 워크플로 합의(2026-04-30, Dongjin).

Week 10 (완료): 시스템 설계 및 문제 정의



요구사항: 문제 정의 및 시스템 스케치(블록 다이어그램 포함), 적용할 핵심 OS 개념 명시.

실제 진행: QEMU의 Serial Port와 Host 간 통신을 중계할 Python 브릿지 아키텍처 다이어그램 및 설계 완료. Se-Joong이 host↔xv6 socket 프로토타입(2026-05-06)을 띄우고, Solar API + 인터랙티브 REPL을 연결(2026-05-11).

Week 11 (완료): 최소 기능 프로토타입 (MVP) 구현



요구사항: 최소 기능 프로토타입 (MVP) 구현 — 스케줄러·디스패처가 실제로 동작하는 단계.

실제 진행: Se-Joong이 **CFS 스케줄러(vruntime + creation_tick)와 `REQ|` 명령 디스패처**를 xv6 커널에 통합(2026-05-11, 커밋 `6f406d4`). xv6를 submodule에서 일반 트리로 전환. `agent_dispatch()` 신설(PRINT/KILL/NICE, init/sh 보호). `consoleintr()`가 `REQ|...` 라인을 가로채 디스패치. 강의 요구사항 문서(Project_requirements.md) 추가.

Week 12 (완료): 통합 프로토타입 완성 및 평가 지표 세팅



요구사항: 통합 프로토타입 + ≥1 평가 지표.

실제 진행: SeungBeom이 **LLM-OS 핵심 기능 세트**를 한꺼번에 머지(2026-05-18, 커밋 `30c81dc`) — Linux 가중치 테이블 기반 CFS, `setpriority`/`getpriority` syscall + 음수 권한 가드(F1·F2), `jail()` 시스템 콜과 격리 워커 `agentd`, 위험 syscall 차단(F7), 함수별 priority(F8), `agent.py` ReAct 루프 + 대화 메모리(F5), `agentdemo`. 이후 거부 목록을 **설정 가능**하게 전환(`denyctl` 셸 도구, `/denylist.conf` 영구화, 부팅 시 init이 자동 로드)하고 **AI 자기관찰 명령**(`PS`·`HELP`, 신규 syscall `procinfo`) 추가, **보안 가드 강화**(`sys_setpriority` 양방향 권한 검사, `sys_procinfo` 커널 스택 노출 fix)도 완료(2026-05-22, 커밋 `8c0f8da`). Se-Joong이 **F9 LLM 응답 캐시**(`cache.c` — 16-슬롯 RAM + `/cache.bin` 디스크 오버레이 + MinHash/Jaccard, `cache_test` 13/13)를 원작 작성(2026-05-22, `76b2737`)하고 SeungBeom이 명령 경로에 연결(`ASK`/`LLM_RESP`/`CACHE_GET/SET` 메타 명령, 호스트 `agent.py`의 `:ask` 경로, F7 deny 검사를 `dispatch_now` forward 경로로 이동). June이 **평가 자동화**(`priority_test` Test 3에 pipe 기반 finish-order 자동 검증, `cfs_share` CPU 점유율 벤치) + `CHANGELOG.md` 신설 + `Implementation.md` 전면 재작성 + **F6 호스트 측 파싱** 설계 결정 문서화 + `.gitignore` 패턴 fix로 `mkfs.c` fresh clone 빌드 복원(2026-05-20).

Week 13 (완료): 시스템 최적화 및 리허설



요구사항: 평가 결과 + 리허설.

실제 진행: Se-Joong이 자연어 에이전트 안정화 라운드 — **confirm-escape v2**(`kernel/confirm.c` dedicated channel sleep/wakeup, `clockintr`의 `confirm_tick` 타임아웃 15초, `agentcmd.c`의 `try_inline_confirm_res()`로 CONFIRM_RES 큐 우회)로 v1의 yield-poll wakeup race를 해결. **`spawn` 도구 verb**(자연어 "프로세스 만들어줘" → `agent.py wire_for` → `SPAWN|<bin>|<argv>` → `agentd do_spawn` → fork+exec → confirm-escape 게이트) 추가. **`populate_jail()`** 으로 jail 진입 전 `echo`/`sh`/`cat`/`ls`/... hard-link. **console.c REQ| payload echo skip**으로 wire byte-race 차단. **`_cache_lookup` strip 정규화** fix(Issue A — 동일 prompt 가 2번째부터 `[cache HIT]`). **자동 회귀 하니스 2종 신설** — `tools/ralph_battery.py`(26 셸/syscall 시나리오) + `tools/ralph_natlang.py`(39 자연어 시나리오, mock 모드). 누적 **65/65 GREEN** (2026-05-28, 커밋 `574c3d7` → PR #11 머지 `e441461`). README/Project_Guide도 모든 변경 반영. **영어 산출물 추가** — `README.en.md` + `docs/Technical_Report.md` + `docs/Development_Process.md`(2026-05-28, 커밋 `8ee2db7`).

Week 14 (현재): 최종 발표 및 산출물 마감



요구사항: 영어로 실제 프레젠테이션 진행. GitHub Public 레포지토리에 요구되는 4가지 최종 산출물(애플리케이션 코드/README, 기술 보고서, 개발 프로세스 문서, 영어 슬라이드) 업로드 마감.

실제 진행: 4개 산출물 중 3개 완료 — `README.md`/`README.en.md` + 소스 (Deliverable #1), `docs/Technical_Report.md` (Deliverable #2), `docs/Development_Process.md` (Deliverable #3) 영어로 작성·머지됨. **영어 슬라이드(Deliverable #4)** 작성과 데모 캡처(asciinema → GIF), 그리고 개인 브랜치 최종 통합 머지가 남은 작업. 한국어 부속 문서(`Implementation.md`/`CHANGELOG.md`/`Weekly_Development_Process.md`)도 Week 13 변경을 반영해 최신화 완료(2026-05-31).
