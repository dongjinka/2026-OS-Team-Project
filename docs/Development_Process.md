# Development Process Document — LLM-OS

**Project:** LLM-OS (Direction A — *OS for LLM*) · 2026 Operating Systems Team Project
**Window:** Week 9 – Week 14 (2026-04-30 → final presentation)
**Repository:** `github.com/dongjinka/2026-OS-Team-Project`
**Document last updated:** 2026-06-04 (Week 14) — this is a snapshot through PR #12; later changes (e.g. the 06-05 README consolidation, 06-08 demo media, PRs #13–#15) are tracked in [`../CHANGELOG.md`](../CHANGELOG.md).

> *(KR: 본 문서는 기획 → 일정 → 실행 → 회고 순서로 개발 과정을 기록한다. 본문은
> 영어, 핵심 사항에는 한국어 주석을 붙였다. 주차별 진행·이슈·해결은 모두 실제
> git 이력에서 도출했다.)*

> **Sourcing note.** Dates, authors, commits, and resolved issues below are
> taken from the actual git history and `CHANGELOG.md`. The *meeting-notes*
> section (§5) is reconstructed from that history; items only the team knows
> (verbal decisions, exact attendance) are marked **`[fill in]`**.

---

## 1. Team & Roles

| Member | Branch | Primary focus *(derived from git history)* |
| ------ | ------ | ------------------------------------------ |
| **Se-Joong Kim** (김세중) | `Sejoong` | Integration & lead: host↔xv6 socket, Solar API + REPL, CFS+dispatcher integration, jail-based sandbox rewrite, semantic cache (F9), **confirm-escape v1 → v2**, **`spawn` verb + `populate_jail`**, **`ralph_battery` + `ralph_natlang` regression pair (65/65 GREEN)** |
| **SeungBeom Kim** (김승범, `server3342`) | `SeungBeom` | Core LLM-OS features: CFS/priority/sandbox/agent loop, configurable deny-list, security guards, self-observation commands (PS/HELP), F9 cache wiring |
| **June Kong** (성준, `SJ-Kong`) | `june_os` | Evaluation automation (Test 3 auto-verify, `cfs_share`), documentation (Implementation.md, CHANGELOG, English deliverables — README/Technical Report/Development Process), F6 design decision |
| **Dongjin Ka** | `Dongjin` *(repo owner)* | Repository setup & PR hosting; **2026-06-04 — adversarial security audit (7 dimensions, 16 confirmed findings) + red-team harness `tools/sec_audit.py` + reproduction binary `user/secnice.c`** — additive only under the `main` 불변 rule |

> *(KR: 역할은 git 이력에서 추론. 실제와 다르면 이 표를 직접 수정할 것.)*

**Workflow.** Each member works on a personal branch; changes land on the shared
repo through **pull requests** (PR #1–#10 over the project). This gave every
change a review step and kept the trunk buildable.

---

## 2. Planning *(기획)*

### 2.1 Direction & idea (Week 9)

- Chose **Direction A — OS for LLM**.
- Concrete idea: take the **AIOS** three-component framing (Agent Scheduler /
  Tool Manager / LLM-Kernel bridge) and implement each as a **real xv6 kernel
  mechanism**, not a Linux wrapper — to satisfy the "OS concepts you design and
  implement" constraint.
- Core scope bullets identified: (a) a priority + CFS scheduler, (b)
  sandboxing/isolation for LLM-issued commands, (c) a Python bridge to Solar.

### 2.2 Requirements → feature breakdown

The proposal was decomposed into a feature list (see `plan.md`):

| #  | Feature | Class |
| -- | ------- | ----- |
| F1 | priority syscalls (`setpriority`/`getpriority`) | required |
| F2 | user/kernel priority classes (kernel = negative) | required |
| F3 | CFS scheduler (vruntime, array scan, no RB-tree) | required |
| F4 | CFS details (min_vruntime / fork inherit / wakeup) | required |
| F5 | QEMU ↔ Solar Python bridge | required |
| F6 | LLM JSON deserialization | required |
| F7 | sandboxing (whitelist + folder isolation) | required |
| F8 | LLM-callable OS functions + per-function priority | recommended |
| F9 | LLM response cache | recommended |
| F10 | idle-time LoRA training | optional (out of scope) |

### 2.3 OS concepts targeted

Scheduling, processes & priority, system calls, protection/sandboxing, an
in-kernel IPC queue, and the file system — see
[Technical Report §4](Technical_Report.md#4-os-concepts-in-play-and-exactly-where).

---

## 3. Scheduling / Timeline *(일정)*

> Week boundaries are approximate (mapped from commit dates); the dates are
> ground truth.

| Week | Dates (approx.) | Course milestone | What actually happened |
| ---- | --------------- | ---------------- | ---------------------- |
| **9**  | ~04-28 → 04-30 | Team formation, direction, 1-paragraph proposal | Repo created (`Initial commit`, 04-30, Dongjin); direction A + AIOS idea agreed |
| **10** | ~05-05 → 05-11 | Problem statement + system sketch | Weekly-progress docs (`e94c6d5`, 05-05); **host↔xv6 socket** prototype (`406ee19`, 05-06); Solar API + REPL (`baf2566`, 05-11) |
| **11** | ~05-11 → 05-13 | Minimal working prototype | **CFS scheduler + command dispatcher** integrated into xv6 (`6f406d4`, 05-11); course-requirements doc added (`eab475d`, 05-11); PR #2 merged 05-13 |
| **12** | ~05-18 → 05-22 | Integrated prototype + ≥1 eval metric | **Core LLM-OS features** (`30c81dc`, 05-18 — CFS/priority/sandbox/agent loop); **eval automation** Test 3 + `cfs_share` + CHANGELOG + Implementation.md rewrite + `.gitignore` mkfs fix (`f3807d4`/`66a45e1`/`2e0664b`/`1a252d4`, 05-20); **jail-based sandbox** rewrite (`26d9269`, 05-21); **F9 cache** port + command-path wiring (`76b2737`/`7d4dd19`/`5fd0d44`, 05-22); **security guards** NICE + `sys_procinfo` (`a59ee1f`, 05-22); **AI self-observation** PS/HELP (`ad6cd20`, 05-22); configurable deny-list (`13030e0`, 05-22); semantic cache → word-level Jaccard (`c436131`, 05-22) |
| **13a** | ~05-26 | First hardening round | **9-test regression harness** + 3 latent-bug fixes (`05bbe38`, 05-26); REPL backspace fix (`7a539b0`); **confirm-escape v1** (`ac013d6`); agent.py `run_tool` timeout 12 s → 24 s + marker retry (`c77e0a6`); jail confirm-escape branch **temporarily disabled** to dodge a `kerneltrap` panic (`40bf608`); PR #10 merged 05-26 |
| **13b** | ~05-28 → 05-31 | Natural-language stabilisation + doc sync | **Confirm-escape v2** (sleep/wakeup on dedicated channel, `clockintr` timeout, inline `try_inline_confirm_res` bypass of the queue); **`spawn` tool verb** + `populate_jail()` hard-link of core binaries; `console.c` `REQ\|` payload echo skip (wire byte-race fix); `_cache_lookup` strip-normalize (Issue A — same prompt now hits on 2nd call); **two new regression harnesses** — `tools/ralph_battery.py` (26 shell/syscall scenarios, port 5555) + `tools/ralph_natlang.py` (39 NL scenarios, port 6666); **cumulative 65 / 65 GREEN** (`574c3d7`, 05-28; PR #11 merged 05-28). Doc consistency pass `b51938a` (05-31) updated CHANGELOG·Implementation·README.en·Weekly to match. **English deliverables (README.en + Technical Report + Development Process) committed** (`8ee2db7`, 05-28; PR #12 merged 05-31) |
| **14** | 2026-06-04 → final presentation | **Final presentation** (English) + audit pass | **Adversarial security audit** (`24202ad`/`c9e2875`, 06-04 — now orphaned/unreachable, superseded by the 06-05 consolidated docs) — 7 dimensions, 16 confirmed findings, 1 reproduced by `tools/sec_audit.py`; **main 불변** principle (audit is additive; fixes ship as separate PRs). **Demo media captured 2026-06-08** — 8 PNGs of real `solar-pro2` agent-mode sessions landed in [`docs/assets/`](../docs/assets/) (cache HIT, confirm-escape allow/deny, `NICE` denied on `init`, `populate_jail` ls, PS `[K]`/`[A]`, WRITE, READ + conversation memory); README §5.1/§5.2/§5.3 and Technical Report §4.4/§6.2/§6.6 now render them inline. Still pending: **English slides (Deliverable #4)**, GIF capture, triage of audit findings. |

*(KR: 오늘 기준(2026-06-04, Week 14). 남은 일: 영어 슬라이드, 데모 캡처,
보안 감사 발견 #1·#2·#3·#4 별도 PR 정리.)*

---

## 4. Execution *(실행)*

### 4.1 Per-week progress, by role

**Week 9–10 — foundation (lead: Se-Joong)**
- Repo + branch model set up (Dongjin).
- Se-Joong: first `socket` bridge between host Python and the QEMU serial port;
  Solar API integration + an interactive REPL; weekly-progress documentation.

**Week 11 — kernel prototype (lead: Se-Joong)**
- Se-Joong: integrated a **CFS scheduler and a `REQ|` command dispatcher** into
  the xv6 kernel; converted xv6 from a submodule to an in-tree directory; added
  the course-requirements document.

**Week 12 — integration + evaluation (all members)**
- SeungBeom: landed the **core LLM-OS feature set** — CFS with Linux weights,
  priority syscalls + escalation guard, sandboxing, the `agent.py` ReAct loop,
  and `agentdemo`; later made the **deny-list configurable**, added
  **security guards** (NICE permission guard, `sys_procinfo` stack-exposure
  fix), and self-observation commands (**PS/HELP**).
- Se-Joong: rewrote sandboxing onto a **chroot `jail()` model**; added `agentd`
  command handlers; ported the **F9 semantic cache** (MinHash → word-level
  Jaccard).
- June: built **evaluation automation** — pipe-based finish-order check in
  `priority_test` Test 3 and the `cfs_share` CPU-share benchmark; authored
  `CHANGELOG.md`; rewrote `Implementation.md` to match the real implementation;
  documented the **F6 host-side-parsing decision**; restored `mkfs.c` and fixed
  the `.gitignore` build breakage.

**Week 13a — first hardening round (lead: Se-Joong, ~05-26)**
- Se-Joong: added a **9-case regression harness** (`tools/regression.py`),
  found and fixed **3 latent bugs**; fixed REPL line-editing and multibyte-output
  corruption in `agent.py`; **first cut of confirm-escape (v1)** — but a
  `kerneltrap` panic forced the v1 branch to be **temporarily disabled** the
  same day (`40bf608`), keeping the unconditional block as a fallback.

**Week 13b — natural-language stabilisation (lead: Se-Joong, ~05-28 → 05-31)**
- Se-Joong: rebuilt the confirm gate as **confirm-escape v2** — `kernel/confirm.c`
  sleeps the trapping syscall on `&confirm_wait_chan`, `clockintr` enforces a
  15 s timeout, and `agentcmd.c`'s `try_inline_confirm_res()` wakes the channel
  inline (bypassing the agent queue). Removed v1's yield-poll race. Added the
  **`spawn` tool verb** (`do_spawn` + `populate_jail()` hard-link `echo`/`sh`/
  `cat`/`ls`/... into `/agentbox` so `exec` resolves), patched `console.c` to
  **skip echoing the payload of `REQ|` lines** (wire byte-race), and fixed the
  `_cache_lookup` strip-normalize asymmetry (Issue A — repeat `:ask` now hits).
  Promoted regression to a **harness pair**: `tools/ralph_battery.py`
  (26 shell/syscall scenarios on port 5555) + `tools/ralph_natlang.py` (39
  natural-language scenarios in mock mode on port 6666). Cumulative
  **65 / 65 GREEN**. Followed up with a doc-consistency pass `b51938a`
  (CHANGELOG / Implementation / README.en / Weekly aligned with `574c3d7`;
  `README.en.md` was later consolidated into `README.md` + `README.ko.md` by
  `d632060`, 06-05).
- June: wrote the **English deliverables** — the English README (then
  `README.en.md`, now consolidated into `README.md`/`README.ko.md`),
  `docs/Technical_Report.md`, `docs/Development_Process.md` (`8ee2db7`, 05-28).

**Week 14 — final pass & adversarial audit (06-04 →)**
- Dongjin: ran a **7-dimension adversarial audit** of the team's custom code
  (command path · jail · confirm-escape · F9 cache · host bridge). 20 raw
  candidates → **16 confirmed**; 1 reproduced live by `tools/sec_audit.py`
  (jailed `NICE` not self-scoped — `RESULT=VULNERABLE`). This work was committed
  as `24202ad`/`c9e2875` (06-04); those commits are now **orphaned/unreachable**
  from any current branch (superseded by the 06-05 doc consolidation
  `d632060`/`b202df0`), with their content folded into the consolidated
  `docs/SECURITY.md` + `tools/sec_audit.py` + `user/secnice.c`. The audit was
  **additive** to main; main is held
  invariant under the "main 불변" rule so the audit is reproducible against
  `cc40a08`. Actual fixes will ship as separate PRs.
- Team-wide remaining work: English slides (Deliverable #4), demo capture
  (asciinema → GIF for `docs/media/`), and triage of the audit findings.

### 4.2 Evaluation metrics defined

- **Scheduler correctness:** `priority_test` Test 3 auto-verifies finish order
  HIGH(1)→MED(10)→LOW(19); wrong order ⇒ FAIL.
- **Fairness (quantitative):** `cfs_share` reports CPU-share % per priority
  (run with `CPUS=1`).
- **Sandbox:** `agentdemo` confirms jail confinement, `..`/outside-path denial,
  privilege-escalation denial, and that the confirm-escape v2 gate fires on
  `exec`/`kill`/`mknod` (both allow and deny paths reach demo *done*).
- **Agent loop:** multi-step tool-call success + conversation-memory reuse on a
  live Solar session.
- **Regression (host-driven):** `tools/ralph_battery.py` (26 shell/syscall
  scenarios) + `tools/ralph_natlang.py` (39 NL scenarios in mock mode) —
  **65 / 65 GREEN** on `cc40a08`. Isolated ports (5555 / 6666) and per-run
  `fs.img` copies mean they coexist with a developer's 4444 session.
- **Adversarial reproduction:** `tools/sec_audit.py` (now on `main`)
  classifies a sandbox finding as `RESULT=VULNERABLE` / `SAFE`; same harness
  doubles as the regression check that the fix actually flips the result.

---

## 5. Meeting Notes *(회의록)*

> **Reconstructed from git activity.** Use as a skeleton; fill `[fill in]` with
> real attendance and verbal decisions. *(KR: git 활동에서 재구성한 골격 —
> 실제 참석자·구두 결정은 `[fill in]`을 채워 완성할 것.)*

### M1 — Week 9 · Kickoff (around 2026-04-30)
- **Attendees:** `[fill in]`
- **Decisions:** Direction A (OS for LLM); adopt AIOS three-component framing;
  set up the GitHub repo and personal-branch + PR workflow.
- **Action items:** Se-Joong → host↔xv6 transport spike; everyone → read AIOS.

### M2 — Week 10–11 · Architecture (around 2026-05-05 → 05-11)
- **Attendees:** `[fill in]`
- **Decisions:** QEMU serial-over-TCP as the bridge transport; CFS as the
  scheduler; in-tree xv6 (drop submodule). Block-diagram sketch agreed.
- **Action items:** Se-Joong → CFS + dispatcher prototype; define the `REQ|`
  wire protocol.

### M3 — Week 12 · Integration & sandbox direction (around 2026-05-18 → 05-22)
- **Attendees:** `[fill in]`
- **Decisions:** consolidate features (SeungBeom's core set) on main; move
  sandboxing to a **chroot `jail()`** model; add **F9 cache**; **F6 → parse JSON
  on the host** (rationale recorded in the report); split docs into
  Implementation / plan / CHANGELOG.
- **Action items:** June → evaluation automation + docs; SeungBeom → security
  guards; Se-Joong → jail rewrite + cache.

### M4 — Week 13a · Hardening & dry-run (around 2026-05-26)
- **Attendees:** `[fill in]`
- **Decisions:** add a regression harness before the dry-run; gate dangerous
  syscalls behind a one-time host confirmation (confirm-escape v1); freeze
  scope (F9 in, F10 out).
- **Action items:** fix the 3 latent bugs; investigate the confirm v1 panic;
  prepare the English slides and demo capture; **merge all branches for the
  final deliverable**.

### M5 — Week 13b · Natural-language stabilisation (around 2026-05-28 → 05-31)
- **Attendees:** `[fill in]`
- **Decisions:** **replace confirm v1 with v2** (sleep/wakeup on a dedicated
  channel + inline `try_inline_confirm_res()`); promote regression to a
  **two-harness pair** (`ralph_battery` + `ralph_natlang`) with isolated ports
  and per-run `fs.img` so they coexist with interactive sessions; the
  acceptance bar for any further change is **65 / 65 GREEN**; **add a `spawn`
  tool verb** so natural-language process-creation prompts flow through the
  confirm gate; ship the **English deliverables** (English README + Technical
  Report + Development Process) on `main` — the README later consolidated into
  `README.md` + `README.ko.md` (`d632060`, 06-05).
- **Action items:** Se-Joong → land `574c3d7` (PR #11); follow-up doc
  consistency pass `b51938a` (PR #12). June → English README + the two `docs/`
  reports.

### M6 — Week 14 · Adversarial audit + final prep (around 2026-06-04)
- **Attendees:** `[fill in]`
- **Decisions:** keep `main` invariant under the audit ("main 불변");
  Dongjin's audit + red-team harness land as commits `24202ad`/`c9e2875`
  (later orphaned/superseded; their content now lives in the consolidated
  `docs/SECURITY.md` + `tools/sec_audit.py` on `main`); actual fixes for the
  16 findings ship as **separate PRs**
  (so the 65 / 65 baseline stays measurable); the audit's deadlock warning
  (`p->lock` + `tickslock` ordering in `allocproc`) is recorded as a fix to
  **not apply**.
- **Action items:** triage the 4 HIGH/MEDIUM findings into ordered PRs
  (#1 confirm self-resolve → #2 cache jail-root → #3 NICE self-scope → #4
  wire-escape coverage); produce the **English presentation slides**
  (Deliverable #4); capture demo media (`docs/media/`); rehearse the
  presentation in English.

---

## 6. Issues Encountered & Resolutions *(이슈와 해결)*

All entries are real and traceable to commits / `CHANGELOG.md`.

| # | Issue | Resolution | Owner / date |
| - | ----- | ---------- | ------------ |
| 1 | `.gitignore` pattern `mkfs` ignored the whole `mkfs/` dir, so `mkfs.c` never reached git → **fresh clones failed to build** | Narrowed pattern to `mkfs/mkfs`; restored `mkfs.c` from upstream xv6 | June · 05-20 |
| 2 | `priority.patch` became inconsistent with the new weight-table CFS | Removed it (recoverable via git history) | June · 05-20 |
| 3 | **Boot regression after the jail rewrite** | Corrected `main.c` init order + single `init` entry point | Se-Joong · 05-21 |
| 4 | `agent.py` reader **corrupted multibyte (Korean) output** | Fixed the serial reader's chunk decoding | Se-Joong · 05-22 |
| 5 | Semantic cache too coarse (MinHash) | Replaced with **word-level Jaccard** similarity | Se-Joong · 05-22 |
| 6 | **Security:** `NICE` lacked a permission guard; `sys_procinfo` leaked stack data | Added the NICE guard; blocked the stack exposure | SeungBeom · 05-22 |
| 7 | Unknown latent defects before the dry-run | **9-test regression harness** surfaced **3 bugs**, all fixed | Se-Joong · 05-26 |
| 8 | Raw-mode REPL left a residual first character on backspace | Rewrote the line editor | Se-Joong · 05-26 |
| 9 | Confirm-escape v1 caused a `kerneltrap` **panic** (yield-poll wakeup race against the ISR) | Temporarily disabled the branch to keep the kernel stable (`40bf608`) | Se-Joong · 05-26 |
| 10 | `agent.py` tool calls timed out under slow API responses | Raised timeout 12 s → 24 s + one marker retry | Se-Joong · 05-26 |
| 11 | F6 proposal said "parse JSON in the kernel," which conflicts with xv6's no-float/no-heap rules and kernel safety | **Decision:** parse on the host; document the rationale in the report | June · 05-20 |
| 12 | Wakeup race in confirm v1 (yield-polling shared state with the ISR) made re-enabling unsafe | **Confirm-escape v2** — sleep on `&confirm_wait_chan`, wake from `clockintr` (15 s) or inline `try_inline_confirm_res()` in `agentcmd.c`; bypasses the agent queue | Se-Joong · 05-28 |
| 13 | `consolewrite()` from `agentd` interleaved with raw `REQ\|` payload bytes, producing a **wire byte-race** | `console.c` now **skips echoing** the payload of `REQ\|` lines (the kernel still receives every byte) | Se-Joong · 05-28 |
| 14 | Same `:ask <prompt>` missed on the 2nd call (Issue A) — `_cache_lookup` strip-normalize was asymmetric (store vs lookup) | Made stripping symmetric so identical prompts collide on the same key | Se-Joong · 05-28 |
| 15 | LLM tasks like *"make a process that prints hello"* had no path into the sandbox | Added the **`spawn` tool verb** (`SPAWN|<bin>|<argv>` → `agentd do_spawn` → fork+exec in jail → confirm-escape v2); `populate_jail()` hard-links `echo`/`sh`/`cat`/`ls`/… so `exec` resolves | Se-Joong · 05-28 |
| 16 | Solar dropped a token when a Korean particle abutted a number (e.g. `"22 + 45는?"`) — looked like a kernel bug | Confirmed upstream (Solar tokenizer); added a *token-boundary* clause to `SYSTEM_PROMPT`; documented the user workaround | Se-Joong · 05-28 |
| 17 | Single 9-test regression harness was hitting its scope ceiling | Promoted to a **harness pair** with isolated ports — `tools/ralph_battery.py` (26 shell/syscall, port 5555) + `tools/ralph_natlang.py` (39 NL, port 6666). **Cumulative 65 / 65 GREEN** on `cc40a08` | Se-Joong · 05-28 |
| 18 | Confirm-escape prompt was answered with stale `\n` from prior input and auto-denied without showing the prompt | (a) Timeout extended 5 s → 15 s. (b) `agent.py:_handle_confirm_req` now calls `termios.tcflush(TCIFLUSH)` before `input()` to drain stale stdin. (c) `sh` added to `populate_jail()` for `SPAWN /sh` paths. | Se-Joong · 05-28 |
| 19 | Multi-line `/plan.txt TODO 1\nTODO 2` was truncated on the wire | `agent.py:_wire_escape` escapes `\n` → `\\n` for `chat`/`write`/`print`; `user/agentd.c:unescape_inplace` restores it before writing. | Se-Joong · 05-28 |
| 20 | Adversarial audit (06-04) **reproduced** that jailed `NICE` is not self-scoped — a jailed agent could renice any user-class proc → scheduling DoS | Reproduction binary `user/secnice.c` + `tools/sec_audit.py` (`RESULT=VULNERABLE`); proposed ~3-line fix (`if (myproc()->is_agent && pid != myproc()->pid) return -1;`) on a separate PR (main 불변) | Dongjin · 06-04 |
| 21 | Audit also flagged 3 more HIGH/MEDIUM paths (confirm-`CONFIRM_RES` self-resolution, `/cache.bin` resolving under `jail_root`, wire-escape gaps on `read`/`write` filenames + `spawn` argv) | Documented in `docs/SECURITY.md` (on `main`) with `file:line`, severity, fix risk, status; gated through separate PRs to keep the 65 / 65 baseline measurable | Dongjin · 06-04 |
| 22 | Audit's own "false-positive guard": one proposed fix would create an **A-B / B-A deadlock** between `p->lock` and `tickslock` (vs `clockintr`) | Recorded as a fix to **not apply**; alternative is to leave `allocproc`'s ticks read as a benign race | Dongjin · 06-04 |

---

## 7. Retrospective *(회고)*

### What went well *(잘된 점)*
- **Real OS mechanisms, not a wrapper.** CFS, priority classes, a chroot jail,
  an in-kernel command queue, sleep/wakeup on a dedicated confirm channel, and
  new syscalls are all genuinely in the kernel.
- **Defense in depth.** Four independent layers now bound what the LLM can do —
  deny-list → confirm-escape v2 → blocked syscalls / jail → tool whitelist.
- **PR + personal-branch workflow** kept the trunk buildable and gave every
  change a review (PR #1 → #12).
- **Evaluation is automated and load-bearing.** Test 3 pass/fail and `cfs_share`
  numbers, plus the `ralph_battery` + `ralph_natlang` pair at **65 / 65 GREEN**,
  catch regressions before they reach review.
- **Adversarial review before submission.** The 06-04 audit found 16 real
  issues (1 already reproduced); none of those are blockers because the
  audit is additive and the regression baseline is preserved.

### What was hard *(어려웠던 점)*
- Interrupt-context constraints (no fork/file-I/O in `consoleintr`) forced the
  queue + worker split *and* the inline `try_inline_confirm_res()` shortcut.
- xv6's no-float / no-dynamic-allocation rules shaped several decisions (F6,
  integer-only CFS math, fixed-size buffers).
- Kernel-stability regressions (boot order, confirm v1 panic, wire byte-race)
  cost real time and motivated the regression promotion to a harness pair.
- Getting confirm-escape **right** took two attempts: v1 yield-polled and
  raced; v2 had to be a real sleep/wakeup with a clockintr timeout and an
  inline path that doesn't go through the agent queue.

### What we'd do differently *(개선점)*
- Add the regression harness **earlier** — several latent bugs would have been
  caught sooner.
- Tighten the `.gitignore` from the start (the `mkfs` breakage hit fresh clones).
- Keep personal branches merged more frequently to avoid divergence near the
  deadline.
- **Run the adversarial audit before feature freeze, not after.** Of the 16
  findings the audit surfaced, several (e.g. `CONFIRM_RES` self-resolution,
  `_wire_escape` coverage gaps) were design-level oversights that would have
  been cheaper to fix while the surrounding code was still being written.

### Remaining before the final presentation *(남은 일)*
- **English presentation slides** (Deliverable #4) — not yet started.
- **Demo capture (PNG done 2026-06-08, GIF still pending).** 8 PNGs of live
  `solar-pro2` agent-mode sessions are in [`../docs/assets/`](../docs/assets/)
  (cache HIT, confirm-escape allow/deny, `NICE` denied on `init`, `populate_jail`
  listing, PS `[K]`/`[A]` markers, WRITE, READ + conversation memory) — README
  §5.1/§5.2/§5.3 and Technical Report §4.4/§6.2/§6.6 render them inline. An
  asciinema → GIF of the full ReAct loop is the remaining piece.
- **Audit triage** (against the findings in `docs/SECURITY.md`) — at minimum land PRs for
  findings #1 (confirm self-resolve), #2 (cache jail-root), #3 (NICE
  self-scope), #4 (wire-escape gaps). Each PR must keep the 65 / 65 baseline.
- Final rehearsal of the English presentation.

> *(KR: 제출 전 반드시 — 영어 슬라이드 작성, asciinema GIF 캡처(PNG 8장은
> 06-08에 들어옴), 보안 감사 4건 별도 PR 정리, 영어 리허설.)*

---

## 8. Cross-references

- Architecture & OS concepts: [Technical_Report.md](Technical_Report.md)
- Setup / run / demo: [../README.md](../README.md) (English) · [../README.ko.md](../README.ko.md) (Korean)
- Code-referenced detail: [../Implementation.md](../Implementation.md)
- Feature status: [../plan.md](../plan.md) *(archived 2026-05-18)* · Change log: [../CHANGELOG.md](../CHANGELOG.md)
- Korean weekly chronology: §3 (Timeline) and §5 (Meeting Notes) above, plus [../CHANGELOG.md](../CHANGELOG.md)
- **Adversarial audit:** [`SECURITY.md`](SECURITY.md)
  (full finding register #1–#9, file:line, severity, status) + `tools/sec_audit.py`
  (red-team harness) + `xv6-riscv/user/secnice.c` (NICE-escalation
  reproduction). The audit was first committed as `24202ad`/`c9e2875`
  (2026-06-04); those commits are now orphaned/unreachable (superseded by the
  06-05 consolidation `d632060`/`b202df0`), and their content lives on `main`
  in `docs/SECURITY.md` + `tools/sec_audit.py` + `xv6-riscv/user/secnice.c`.
