# Changelog

본 프로젝트의 모든 주요 변경 사항을 시간순으로 기록한다.
형식은 [Keep a Changelog](https://keepachangelog.com/ko/1.1.0/) 기반.

> **읽는 법**
> - `Added` (추가) · `Changed` (변경) · `Fixed` (수정) · `Removed` (삭제) ·
>   `Security` (보안)
> - 각 항목 끝의 `(작성자)`로 누가 작업했는지 표시
> - 더 상세한 진행 상태는 [plan.md](plan.md), 아키텍처 설명은
>   [Implementation.md](Implementation.md) 참조

---

## [Unreleased]

### Changed (2026-06-09 — June, 팀원 git author 변형 병기 + 누락 작업 보완)
- **`README.md` / `README.ko.md` §11 Team 표** — 각 멤버 행에 한글 이름 +
  git author nickname 병기:
  Se-Joong Kim (김세중, `Se-Joong-Kim` / `Se-Joong_Kim`),
  SeungBeom Kim (김승범, `server3342`),
  June Kong (공성준, `SJ-Kong` / `June Kong`),
  Dongjin Ka (가동진, `dongjinka` — repo owner).
  각 멤버의 주요 작업도 같이 보강(SeungBeom audit fix PR #13 + 문서 정합 PR
  #17/#18, June 06-04~06-09 작업, Dongjin PR #15 README 재구축 + 32KB stack).
- **`docs/Development_Process.md` §1 Roles** — June 행에 누락됐던
  **`e8ac26b` (2026-06-04) Technical Report + Development Process 재정합**
  작업(confirm-escape v2 / `spawn` / ralph 65/65 / 06-04 audit 반영) 추가 +
  email 두 개(`june020110@gmail.com` / `steady11317@gmail.com`)도 병기 —
  git author 변형이 둘인 사실이 추적 가능하도록.
- **한글 이름 정정 (4인 확정)** — README EN/KO 팀 표 + Dev Process §1 Roles +
  CHANGELOG에서 잘못 추측됐던 *"강동진"*을 **가동진** (Ka = 한국어 성씨 "가",
  not "강")으로, 그동안 *"성준"*으로만 적혔던 June을 **공성준**으로 정정.
  Se-Joong = 김세중, SeungBeom = 김승범은 기존 표기 그대로 유지.
- **`docs/Development_Process.md` §1 Workflow** — "PR #1–#10 over the project"
  표현을 **"PR #1–#18 over the project"** 로 정확성 갱신. PR #12(영문 산출물),
  #13/#14(audit fixes), #15(06-05 README/kernel-stack), #17/#18(06-09 문서
  통합)의 역할도 한 줄로 명시 — Workflow 표현이 main의 실제 PR 진행과 정합.
- **`docs/README.md` 인덱스** — Security & Evaluation 표에 누락됐던
  [`UNIT_IO_MATRIX.md`](docs/UNIT_IO_MATRIX.md) (에이전트 명령 경로 단위
  기능별 입력→기대 출력 매트릭스) 행 추가 — 새 main 추가 검증 자료의 라우팅
  복원.

### Changed (2026-06-10 — June, 회의록 채움 + Deliverable #4 main 반영)
- **`docs/Development_Process.md` §5 Meeting Notes M1~M6 Attendees** —
  6곳 모두 `[fill in]` → *Se-Joong Kim (김세중), SeungBeom Kim (김승범),
  June Kong (공성준), Dongjin Ka (가동진) — 전원* 으로 채움 (커밋은 특정인이
  했어도 회의·논의는 전원 참여).
- **`docs/Development_Process.md` §1 Roles Dongjin 행** — 06-10 PR #20/#21/#22
  (`slides/OS for LLM.html` 2,719줄 reveal.js + EN/KR scripts +
  presentation-guide + GitHub Pages 라이브 deck + README header 재작업)
  추가.
- **`docs/Development_Process.md` §3 Timeline Week 14 행** —
  *"Still pending: English slides, asciinema GIF"* 표현 제거.
  PR #20/#21/#22 narrative + GitHub Pages URL 추가. KR 주석도 *"4개
  산출물 모두 main에 적재 완료, 남은 일은 영어 발표 리허설 + (선택) 잔여
  감사 #2/#5"* 로 정합.
- **`docs/Development_Process.md` §4 Week 14 execution** — Team-wide
  remaining work 단락을 *"rehearse English presentation; (optional) audit
  #2/#5"* 로 축소 + June 06-08~06-09 narrative + Dongjin 06-10 PR #20/#21/#22
  narrative 추가.
- **`docs/Development_Process.md` §5 M6 Outcome** — Dongjin 06-10 PR
  #20/#21/#22 (Deliverable #4 landing) 결과 한 줄 추가.
- **`docs/Development_Process.md` §7 Remaining before final** — *"English
  slides not yet started", "asciinema GIF pending", "audit triage 4건",
  "smp>1 재테스트"* 표현 모두 제거. **남은 일은 "영어 발표 리허설" 1건 +
  (선택) 잔여 감사 #2/#5** 로 정정.
- **`docs/README.md` 인덱스 #4 행** — `slides/ *(to be added)*` →
  `[Live deck →](https://dongjinka.github.io/2026-OS-Team-Project/)` + 소스
  `slides/OS for LLM.html` + EN/KR scripts 경로.

### Added (2026-06-09 — June)
- **End-to-end 데모 비디오 (Google Drive) 링크** — `README.md` §5 + `README.ko.md` §5
  헤더에 배너로 박음 + `docs/assets/README.md` 인덱스에도 외부 미디어로 표시.
  한 세션에 캐시 HIT · `spawn` confirm-escape allow/deny · `NICE` init 거부 ·
  WRITE/READ + 대화 메모리 · `PS` `[K]`/`[A]` 마커 모두 시연. `Project_requirements`
  §3 *"demo screenshots and/or short demo video/GIF"* 의 video 측면도 충족.

### Changed (2026-06-09 — June, 보고서 정합 ralph 검토 후 검증된 부분만 반영)
- **`docs/Development_Process.md`** — §1 Roles의 June 행에 *"2026-06-08
  (`70858a0`) 데모 캡처 8장 + README/Tech Report 인라인"* 과 *"2026-06-09
  end-to-end 데모 비디오 링크 + post-PR-#12 변경 정확성 검토"* 추가.
  SeungBeom 행에 *"06-04 (PR #13) audit fixes #1·#3·#4 + 06-09 (PR #17/#18)
  대형 문서 정합 (SECURITY.md 통합·UNIT_IO_MATRIX.md/CLAUDE.md 신설·Weekly
  archive)"* 추가. Dongjin 행에 *"06-05 (PR #15) README EN-primary 재구축 +
  agent.py 사용자 언어 응답 + 커널 스택 4 KB → 32 KB (`KSTACK_PAGES 8`)"*
  추가. §3 Timeline Week 14 행에 *"2026-06-09 (June) — 데모 비디오 +
  정확성 검토"* 한 줄 추가 (header 정책 "snapshot through PR #12 + 이후는
  CHANGELOG"는 유지하되, *각 멤버의 PR #12 이후 작업 한 줄 요약*만 Roles
  표에 명시해 한 눈에 보이도록).
- **`Implementation.md`** header — 골격은 05-31 그대로 두되 "**최신 갱신
  (2026-06-09)**" 절을 추가해 main에 들어와 있는 코드 변경(PR #13/#14 audit
  guards · PR #15 `KSTACK_PAGES 8` = 32KB · PR #17/#18 정리) 명시.
  ⚠ `CLAUDE.md` 정책에 따라 *"smp>1 kernelvec trap-entry race는 32KB와 별개,
  여전히 존재"* 도 함께 적시 — 32KB stack을 SMP race 해소로 *오해하지 않도록*.
- **`Project_Guide.md` §11.10** (`scause=0xf` `kerneltrap` 진단 절) — PR #15
  `3249dff`가 *agent/spawn deep exec chain overflow*는 32KB stack(`KSTACK_PAGES 8`)
  으로 해소했음을 명시. **단, `smp>1` kernelvec trap-entry race는 별개로 여전히
  존재** ((`CLAUDE.md`가 `make qemu-agent` `smp=1` 강제를 의도된 것으로 못 박음)
  도 함께 표기 — 두 가지 다른 `scause=0xf`를 혼동하지 않도록.

### Docs (2026-06-09 — server3342)
- **문서 정리/통합** — `docs/SECURITY_AND_EVALUATION.md`(EN)와 `docs/SECURITY_AUDIT.md`(KR)를
  단일 [`docs/SECURITY.md`](docs/SECURITY.md)(§1 EN 개요 + §2 KR 전체 발견 등록부 #1–#9)로 병합;
  평가 수치는 이미 `docs/BENCHMARKS.md`에 있어 중복 제거. `Weekly_Development_Process.md`는
  `docs/Development_Process.md` §3·§5가 EN으로 전부 커버하므로 삭제. `plan.md`는 2026-05-18
  스냅샷으로 아카이브 헤더 표기. 모든 교차참조/인덱스(`docs/README.md`)를 갱신.
- **CLAUDE.md** (신규) — Claude Code용 저장소 가이드(빌드·실행·테스트·에이전트 명령 경로·핵심 파일).
- **docs/UNIT_IO_MATRIX.md** (신규) — 에이전트 명령 경로 단위기능별 입력→기대출력→
  검증 하네스 매트릭스.
- **문서 사실 정합성 패스** — 코드/git 히스토리 대조로 문서의 file:line·수치 불일치를 정정:
  semantic 임계 0.40, confirm 타임아웃 15초(150 ticks), CFS 가중치 역비례 식, agentd 도구 11종,
  `eab475d` 날짜 2026-05-11, 부재 브랜치/`README.en.md` 참조 정정 등.

### Added (2026-06-08 — June)
- **데모 캡처 8장** `docs/assets/`에 추가 — 모두 `solar-pro2` 라이브
  agent-mode 세션 실제 출력:
  - `cache-hit.png` — 동일 산수 질문 2회, 2번째에 `[cache HIT] Solar not called`
    (F9 캐시 hit, [Technical Report §5.4](docs/Technical_Report.md))
  - `confirm-escape-allow.png` — *"make a process that runs echo hello"* → `y`
    → `SPAWN /echo done (status=0)` (confirm-escape v2 + `spawn`, allow path)
  - `confirm-escape-deny.png` — 같은 요청에 `N` → `denied (confirm-escape)`
    (confirm-escape v2, deny path)
  - `nice-init-denied.png` — *"change init priority to 5"* →
    `[agentd] NICE: denied (pid=1 prio=5)` (F2 kernel-class 보호, init = −5)
  - `jail-populate.png` — *"list files in /agentbox"* → `populate_jail()`이
    부팅 시 hard-link한 `echo`/`cat`/`ls`/`grep`/`sh`/... 보임
  - `agent-write.png` — *"write 'TODO 1\nTODO 2' into /plan.txt"* →
    `[agentd] WROTE 13 bytes` (WRITE + wire newline escape)
  - `agent-read-memory.png` — *"re-read the file you just created and summarize it"*
    → READ + `agent.py self.messages` 대화 메모리로 어떤 파일인지 인식
  - `agent-ps.png` — *"show me the currently running processes"* → PS 출력에
    `init [K] -5 / agentd [A] 8 / sh 10` (자기관찰 + `[K]`/`[A]` 클래스 마커)

### Changed (2026-06-08 — June)
- **README.md** — §5.1 끝에 `cache-hit.png` 인라인; §5.2에 confirm-escape
  allow/deny 페어와 `nice-init-denied.png` 인라인; §5.3 placeholder 테이블을
  실제 4컷 갤러리(jail-populate · agent-ps · agent-write · agent-read-memory)로
  교체.
- **README.ko.md** — 영문 README와 동일 위치에 한글 캡션으로 미러.
- **docs/Technical_Report.md** — §4.4 끝(jail/F7 설명)에 `nice-init-denied.png`,
  §6.2 끝(agentd tool table + AI self-observation)에 `agent-ps.png`,
  §6.6 끝(`spawn` + `populate_jail`)에 `confirm-escape-allow.png` ×
  `jail-populate.png` 페어 인라인.
- **docs/Development_Process.md** — §3 Week 14 행과 §7 "Remaining before the
  final presentation" 항목을 갱신 — "데모 캡처: PNG 8장 done 2026-06-08, GIF
  pending".
- **Weekly_Development_Process.md** — Week 14 단락 끝에 06-04 보안 감사 +
  06-08 데모 캡처 8장 추가 사실을 명시(영문 `docs/Development_Process.md`
  Week 14 행과 일관).
- **docs/assets/README.md** — 인덱스를 1줄 placeholder 표에서 캡처 8장의
  파일명·시연·매핑된 보고서 섹션을 모두 적은 표로 교체. 본문 첫 줄에서
  "데모 자리표시가 가리키는 폴더" 문구를 "**8장이 들어 있고** asciinema GIF /
  `priority_test` / `cfs_bench` 화면이 아직 안 들어왔다"로 정리(미완성 인상 제거).
- **docs/README.md** — 라우팅 인덱스의 "Security & Evaluation" 표에 "Demo
  media" 행 추가 — `docs/` 하위 산출물 인덱스가 `assets/`를 안 가리키던 누락
  보완.
- **README.md §5.3 / README.ko.md §5.3 캡션** — "All eight ... captured 2026-06-08"
  /  "스크린샷 8장 모두 ... 캡처" 문구를 §5.2와 §5.1에 이미 인라인된 4장 +
  §5.3의 4장으로 구체화. 추가 자리표시(`agent-demo.gif` 등)도 "아직 안 들어온
  후보"임을 명시해 미완성 인상 제거.

### Removed (2026-06-08 — June)
- **README.md / README.ko.md 마지막 줄의 `</content>` 잔재** — 이전 세션의
  Read tool 출력 닫힘 태그가 파일에 섞여 커밋된 흔적. Markdown 렌더에는 보이지
  않지만 raw view에서 미완성 인상을 줘서 제거. 두 파일 모두 마지막 의미 줄
  ("- Design motif: ...") 직후로 자연 종료.

---

## [2026-05-28] — 자연어 에이전트 안정화 · confirm-escape v2 · spawn 도구 · 회귀 65/65 (커밋 `574c3d7`)

> exec/kill/mknod 같은 위험 syscall을 단순 거부에서 *호스트 사용자에게 y/N
> 확인을 받고* 통과시키는 **confirm-escape v2** 와, 자연어 `프로세스
> 만들어줘` → 도구 verb `spawn` → fork+exec → confirm-escape 게이트의 풀
> 파이프라인. 회귀를 **65/65 자동** (셸/syscall 26 + 자연어 39)으로 끌어
> 올림.

### Added
- (Se-Joong) **confirm-escape v2** — `kernel/confirm.c` 신규. yield-poll 방식
  v1을 대체하는 dedicated channel sleep/wakeup. `confirm_request()`가
  요청자를 `&confirm_wait_chan`에 잠재우고, `clockintr`가 매 tick
  `confirm_tick()` (`kernel/trap.c`)을 호출해 만료를 깨움. `CONFIRM_TIMEOUT_TICKS = 150`
  (≈15s) — 사용자가 stdin 정리 후 응답할 충분한 시간 확보.
- (Se-Joong) **CONFIRM_RES 인라인 처리** — `kernel/agentcmd.c`에
  `try_inline_confirm_res()` 추가. 모든 user proc이 SLEEPING 일 때 (드물지
  않은 경우 — agent.py가 host 응답 대기 중) 큐 드레인 없이 spinlock+wakeup
  만으로 바로 처리해 wakeup race 차단.
- (Se-Joong) **`SYS_kill` / `SYS_mknod` confirm-escape 게이트 확장** —
  `kernel/syscall.c`의 `agent_blocked` 분기가 단순 -1 반환 대신
  `confirm_request()` 호출. 직접 호출 회귀용 user 프로그램
  `user/confirm_kill.c`, `user/confirm_mknod.c` 신규.
- (Se-Joong) **spawn 도구 verb** — 자연어 "프로세스 만들어줘"를 받는 끝단:
  `agent.py SYSTEM_PROMPT/TRANSLATE_PROMPT/wire_for()`에 `spawn` 추가, wire는
  `SPAWN|<bin>|<argv-joined>`. `user/agentd.c`에 `do_spawn()` 신규 — fork
  → exec → wait. exec 자체가 `is_agent` 의 위험 syscall이므로 자식이
  confirm-escape 게이트에 걸려 호스트에 y/N 프롬프트가 자연스럽게 발생.
- (Se-Joong) **`populate_jail()`** — `user/agentd.c`. `jail("/agentbox")`
  진입 전에 호스트 fs의 `echo`/`sh`/`cat`/`ls`/`wc`/`grep`/`mkdir`/`rm`/`ln`/`kill`
  바이너리를 `/agentbox/`로 hard-link. 종전 빈 jail 에서 `echo failed`가
  나던 자연어 시나리오를 정상 실행 가능하게 변경.
- (Se-Joong) **자동 회귀 하니스 2종** — `tools/ralph_battery.py` (26 셸/syscall
  시나리오, 격리 포트 5555, ~3 min)와 `tools/ralph_natlang.py` (39 자연어
  시나리오, mock 모드 + 격리 포트 6666, ~50 s). 각각 자체 fs.img 복사본을
  써 사용자의 4444 세션과 동시 실행 가능. 누적 **65/65 GREEN**.
- (Se-Joong) **agent.py mock 모드** 확장 — `UPSTAGE_API_KEY=""` 시
  결정적 rule-based stub. N11(인사 캐시 HIT) / N14(write→read 다단계 chain) /
  N15(nice ACL) / N16(kill confirm deny) / N17(한글 argv 보존) 분기와
  `_mock_step_after_write` 다단계 flag 추가.

### Changed
- (Se-Joong) `kernel/console.c` **REQ\| payload echo skip** — `consoleintr`가
  REQ\| 라인의 prefix 4 byte + `\n`만 echo하고 payload 본문은 echo 생략.
  agent.py가 보낸 wire의 byte-단위 echo가 `consolewrite` 출력과 byte-race로
  interleave해 socket으로 `NT|__OBS6__` 같은 garbled line이 새던 문제 차단.
  line boundary는 보존돼 reader의 `split('\n')`는 그대로 동작.
- (Se-Joong) **wire newline escape** — `agent.py:_wire_escape()`가
  chat/write/print 페이로드의 `\n`을 `\\n`으로 치환, `user/agentd.c
  unescape_inplace()`가 복원. 다중 라인 `/plan.txt TODO 1\nTODO 2`가 wire
  newline에서 잘리던 문제 해결.
- (Se-Joong) `user/agentdemo.c` **fork+exec 패턴** — confirm-escape allow
  분기가 데모 프로세스를 echo로 *replace* 해버려 `=== demo done ===`가 안
  찍히던 회귀 깨짐을 해결. 자식에서 exec, 부모는 wait → allow/deny 양쪽
  모두 데모 완료 도달.
- (Se-Joong) `agent.py:_handle_confirm_req` — 프롬프트 "5초 안"을 "15초 안"
  으로 정정, `input()` 직전 `termios.tcflush(TCIFLUSH)` 호출로 stale stdin
  enter 키 잔재 제거 (이전엔 자동 enter로 묵묵부답 deny).
- (Se-Joong) `agent.py:_read_line` **cooked 모드 기본** — raw 모드 (UTF-8
  incremental decoder + `_wipe_input`/`_draw_input`)는 한글 보일 때 4-5 회
  키 입력이 필요한 UX 버그가 있어 default를 cooked로 전환. raw는
  `AGENT_RAW_INPUT=1` opt-in. raw 사용시 `_wipe_input`에 `rows_above + 1`
  safety margin 추가 — 한글 wide-cell wrap 경계에서 backspace 후 첫 단어가
  잔재로 남던 버그 fix.
- (Se-Joong) `agent.py:SYSTEM_PROMPT` **★ token-boundary rule** clause 분리 —
  spawn 블록 안에 묻혀 있어 chat/write/print에 신호가 안 가던 한글/CJK byte
  보존 규칙을 standalone clause로 빼냄. `"22 + 45는?"`처럼 숫자가 조사
  앞에 붙는 prompt에서 Solar tokenizer가 `45` 를 drop하던 회귀 mitigation.

### Fixed
- (Se-Joong) **`_cache_lookup` strip mismatch** (Issue A) — `agent.py`
  line 800 `_cache_lookup`이 lookup key를 strip 안 함, `_cache_store`는
  strip 함 → FNV-1a hash 불일치로 동일 prompt 가 항상 MISS. 한 줄 `.strip()`
  추가로 두 번째 호출부터 `[cache HIT]` 정상 출력. 자연어 회귀 N11(인사)·
  N13(`:ask 22+45는?`)이 이를 직접 검증.

### Security
- (Se-Joong) confirm-escape 도입으로 *명시적 사용자 동의 없이는* 위험 syscall
  (exec/kill/mknod)이 절대 통과 못 함. 타임아웃 만료시 default deny —
  사용자가 자리를 비워도 차단이 fallback 정책.

---

## [2026-05-22] — AI 자기관찰 명령어 · 설정 가능한 거부 목록 · 보안 강화 · LLM 캐시

### Added
- (Se-Joong) **F9 — LLM 응답 캐시** 원작. `kernel/cache.c` — 16-슬롯 RAM 테이블 +
  `/cache.bin` 디스크 오버레이(LRU evict→append, 디스크 hit 시 RAM promote) +
  MinHash/Jaccard 의미 매칭, 단독 시연 테스트 `user/cache_test.c`. 커밋 `76b2737`
  (PR #5, Sejoong 브랜치, 원본 `c56b028`)에서 작성.
- (SeungBeom) 위 **F9 캐시를 SeungBeom 브랜치로 이식** — `76b2737`을 second parent로
  갖는 머지 커밋으로 가져옴(브랜치 그래프에 pull처럼 기록). 신규 syscall
  `set_cache`/`get_cache`로 노출하되 번호는 **29/30**으로 재배정(SeungBeom의
  `set_deny`/`get_deny`/`procinfo` 26/27/28과 충돌 회피). `cacheinit()`를 `main.c`에
  추가하고 `create()`를 non-static으로 전환(cache.c가 `/cache.bin` 지연 생성에 사용).
  - 보안 하드닝: `sys_get_cache`의 copyout 길이를 `val_buf`(256B) 한도로 한정하고
    `disk_scan` 반환 vlen을 `CACHE_VAL`로 클램프 — 격리 agent가 심은 `/cache.bin`
    레코드의 위조 vlen(최대 0xFFFF)으로 커널 스택을 유저에 노출하던 경로 차단.
- (SeungBeom) **F9 캐시를 명령 경로에 유기적으로 연결** — `76b2737`의 오케스트레이션을
  SeungBeom 구조(설정형 deny 목록·PS/HELP)에 적응. `agentcmd.c`를 2단계로 분리:
  `agent_dispatch`(인터럽트, enqueue) → `agent_drain`(프로세스 컨텍스트,
  `usertrap`/`consoleread`) → `agent_dispatch_now`. 커널 메타 명령 `ASK`/`LLM_RESP`/
  `CACHE_GET`/`CACHE_SET` 추가: `ASK`는 캐시 조회 → 히트면 agentd로 직접 전달(Solar
  생략), 미스면 호스트에 `LLM_REQ` 발신 → `LLM_RESP` 수신 시 `cache_set` 후 전달.
  - 신규 `dispatch` syscall(번호 31) + 이식 프로그램 `eval`/`agent_multi`/`write_race`
    (Se-Joong 원작, `WRITE` 와이어를 SeungBeom `:` 구분자로 적응). agentd에 `CHAT` 핸들러.
  - 호스트 `agent.py`: `:ask <프롬프트>`로 캐시 경로 사용(기본 ReAct 루프 유지);
    `LLM_REQ` 수신 시 Solar 1회 호출 후 `LLM_RESP` 회신. 송신부 스레드 안전화 +
    개행 정리.
  - F7 deny 검사를 `agent_dispatch_now` forward 경로로 이동(경계 유지). 동시 `ASK`용
    `pending_lock` 가드, 커널 스택 보호용 키 스냅샷 256B 상한, `agent_drain` 빈 큐
    fast-path. `console` 입력 버퍼 256→2048(긴 `ASK` 수용).
- (SeungBeom) **AI 자기관찰 명령어 셋** (`PS`·`HELP`) 추가 — LLM이 환경을 보고
  판단해 명령을 쓰도록 정보 명령 제공.
  - `PS` — 프로세스 목록(pid·state·priority·name, `[K]`/`[A]`). 신규 syscall
    `procinfo(buf,max)`(번호 28, `kernel/procinfo.h`)로 proc 스냅샷을 copyout →
    `NICE`가 대상 pid를 알 수 있게 함(관찰→행동 루프).
  - `HELP` — 인자 형식(usage) 포함 명령 카탈로그로 LLM이 호출법을 런타임 확인.
  - agentd 도구 테이블에 `usage` 필드 추가, `agent.py`에 `ps`/`help` 도구 노출.
- (SeungBeom) F7 명령 거부 목록을 **설정 가능**하게 전환 — 기존 하드코딩
  `{KILL, EXEC}`을 스핀락 보호 커널 RAM 가변 목록으로 바꿔 사용자 입력처럼
  변경 가능. 신규 syscall `set_deny`/`get_deny`(번호 26·27), 셸 도구
  `user/denyctl.c`, 공유 헤더 `kernel/deny.h`. 커널 목록 하나가 하드 경계
  명령(KILL/EXEC)과 agentd 도구를 모두 통제하고(단일 소스), agentd `LIST`가
  실효 정책(`DENY(kernel)`)을 표시.
  - **일회성**: `denyctl add/rm/reset` (이번 세션 RAM만)
  - **영구**: `denyctl save` → `/denylist.conf`, `init`이 부팅 시 `denyctl load`로 자동 적용 → 재부팅 생존
  - **권한**: `set_deny`는 `is_agent` 프로세스를 거부 → 격리 agent가 자기 샌드박스를 약화 불가 (사람 전용)

### Changed
- (SeungBeom) `xv6-riscv/Makefile` UPROGS에 `_denyctl`·`_cache_test`·`_eval`·
  `_agent_multi`·`_write_race`, 커널 OBJS에 `cache.o` 등록.
- (SeungBeom) `xv6-riscv/user/init.c`: 부팅 시 `denyctl load` 1회 실행(agentd 기동 전).

### Fixed
- (SeungBeom) `user/cfs_share.c` 정리 — 직접 만든 정수→문자열 변환을 `fprintf`로
  대체해 10자리 `uint` count에서 발생하던 `tmp[8]` 스택 오버플로 위험 제거. 결과
  count 출력도 `%d`→`%u`로 수정.

### Security
- (SeungBeom) `sys_setpriority` 권한 가드 강화 — user-클래스 호출자가 이미
  커널-클래스(priority < 0)인 대상(init 등)을 변경하지 못하도록 거부. 기존
  가드는 음수 priority 부여(상승)만 막아, `PS`로 init pid를 알아낸 격리 agent가
  `NICE`로 init을 user 범위로 강등시킬 수 있었음. 커널-클래스 호출자만 가능.
- (SeungBeom) `sys_procinfo` 커널 스택 정보 노출 수정 — `struct procinfo`를
  `memset`으로 0 초기화. `safestrcpy`가 `name[]` 꼬리를 0으로 채우지 않아
  미초기화 커널 스택 바이트(엔트리당 최대 ~11B)가 copyout으로 유저에 노출됐음.

---

## [2026-05-20] — 평가 자동화 & 설계 결정 문서화

### Added
- (june) `user/cfs_share.c` 신규 — N개 자식을 다른 priority로 띄워 동일
  wall-clock 동안 카운터를 경쟁시키고 priority별 CPU 점유율(%)을 출력하는
  벤치 프로그램. F3/F4의 가중치 효과를 수치로 검증. smp=1 권장.
- (june) `user/priority_test.c` Test 3에 **pipe 기반 finish-order 자동 검증**
  추가. HIGH(1)→MED(10)→LOW(19) 순서가 아니면 FAIL 종료. `burn()` 반복수도
  30M으로 상향해 스케줄링 효과가 시작 노이즈를 압도하도록 조정.

### Changed
- (june) F6(JSON 역직렬화) 설계 결정: **호스트(`agent.py`) 측 파싱 유지**.
  근거 4개(커널 안전성·부동소수/동적할당 제약·계층 분리·검증 단순화)를
  `plan.md §5.1` + `Implementation.md §7.2`에 명시. 제안서 §F6 원문(커널 내
  파싱)에서 변경된 사유를 보고서에 명기.
- (june) `Makefile` UPROGS에 `_cfs_share` 등록.
- (june) `plan.md` 진행 현황 표: F6 ✅ 완료, §5.3 평가 강화 ✅ 완료로 갱신.
- (june) `Implementation.md` §6 검증 표·§7.1 평가 한계·§8 파일 변경 요약에
  Test 3 자동 검증·`cfs_share` 항목 반영.

---

## [2026-05-20] — 문서 정합성 정리 & 빌드 누락 복원

### Added
- (june) `CHANGELOG.md` 신규 — 팀 변경 이력 단일 추적 채널

### Fixed
- (june) `xv6-riscv/.gitignore` 패턴 `mkfs` → `mkfs/mkfs`
  로 좁힘. 기존 패턴이 `mkfs/` 디렉터리 전체를 무시해 `mkfs.c` 소스가 git에
  들어가지 못하던 문제 해소.
- (june) `xv6-riscv/mkfs/mkfs.c` 복원 — upstream `mit-pdos/xv6-riscv`의 riscv
  브랜치에서 가져옴. fresh clone 시 Makefile 빌드 실패 문제 해결.
  (`kernel/fs.h`·`param.h`가 초기 import 이후 미수정이라 그대로 호환)

### Changed
- (june) `Implementation.md` 전면 재작성 — 현 구현(가중치 테이블 CFS·jail·
  agentd·ReAct 루프) 반영. 구버전(Phase 5 보류, priority+1 가산, 직접
  dispatch) 서술 제거. plan.md(상태 추적)·CHANGELOG.md(이력)와의 역할 분리
  명시.

### Removed
- (june) `xv6-riscv/priority.patch` 삭제 — 현 CFS 구현(Linux 가중치 테이블)
  이 단순 priority+1 가산 방식을 완전히 대체. 복원이 필요하면
  `git show <삭제 커밋>~1:xv6-riscv/priority.patch`.

---

## [2026-05-18] — LLM-OS 핵심 기능 통합 (커밋 `30c81dc`)

> 본 항목은 커밋 메시지·`plan.md`에서 역추적한 요약. 상세는 해당 커밋 참조.

### Added
- (SeungBeom) CFS 스케줄러 (`vruntime` 기반 leftmost 선택, Linux 가중치 테이블)
- (SeungBeom) `setpriority`/`getpriority` 시스템 콜 + 음수 권한 가드 (F1·F2)
- (SeungBeom) 샌드박싱 — `jail()` 시스템 콜, agentd 격리 워커, 위험 syscall
  차단, 명령 큐 (F7)
- (SeungBeom) 함수별 priority 적용 (`LIST`/`SETPRIO`, F8)
- (SeungBeom) `agent.py` ReAct 루프 + 대화 메모리 (F5)
- (SeungBeom) `agentdemo` — F2·F7 검증 데모

---

## [2026-05-11] — CFS 기반 통합 (커밋 `6f406d4`)

### Added
- (Se-Joong) xv6-riscv를 submodule에서 일반 트리로 전환
- (Se-Joong) `priority.patch` 베이스라인 위에 CFS(vruntime + creation_tick) 추가
- (Se-Joong) `agent_dispatch()` 신설 (PRINT/KILL/NICE, init·sh 보호)
- (Se-Joong) `consoleintr()`에서 `REQ|...` 라인 가로채 디스패치

---

## 이전 변경

이전 커밋들은 `git log --oneline`으로 조회. 주요 마일스톤:

- `eab475d` (2026-05-11) docs: 코스 요구사항 문서 추가
- `08157c9` (2026-05-11) chore: 주간 개발 진행 사항 업데이트
- 그 외 초기 셋업 커밋들

---

## 작성 규칙

1. **새 변경 시작할 때** `[Unreleased]` 섹션 하위에 카테고리·작성자 표기로 추가.
2. **마일스톤·PR 머지 시점에** `[Unreleased]` → `[YYYY-MM-DD] — 설명`으로 승격.
3. **카테고리 선택**:
   - `Added` — 새 기능·파일·도구
   - `Changed` — 기존 동작·문서·인터페이스 변경
   - `Fixed` — 버그 수정
   - `Removed` — 삭제
   - `Security` — 보안 관련 (별도 강조)
4. **각 항목**은 한 줄 한 가지 변경. "왜" 또는 "어떤 효과"를 한 문장 덧붙이면
   리뷰어가 git log 안 봐도 됨.
5. **작성자 표기**(괄호 안)는 닉네임/이니셜로 통일 — git author와 일치 권장.
