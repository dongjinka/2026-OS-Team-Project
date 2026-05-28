# Development Process Document — LLM-OS

**Project:** LLM-OS (Direction A — *OS for LLM*) · 2026 Operating Systems Team Project
**Window:** Week 9 – Week 14 (2026-04-30 → final presentation)
**Repository:** `github.com/dongjinka/2026-OS-Team-Project`

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
| **Se-Joong Kim** (김세중) | `Sejoong` | Integration & lead: host↔xv6 socket, Solar API + REPL, CFS+dispatcher integration, jail-based sandbox rewrite, semantic cache (F9), regression harness |
| **SeungBeom Kim** (김승범) | `SeungBeom` | Core LLM-OS features: CFS/priority/sandbox/agent loop, configurable deny-list, security guards, self-observation commands (PS/HELP), F9 cache wiring |
| **June Kong** (성준) | `june_os` | Evaluation automation (Test 3 auto-verify, `cfs_share`), documentation (Implementation.md, CHANGELOG, README), F6 design decision |
| **Dongjin Ka** | (repo owner) | Repository setup & administration, branch/PR hosting |

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
| **9**  | ~04-28 | Team formation, direction, 1-paragraph proposal | Repo created (`Initial commit`, 04-30, Dongjin); direction A + AIOS idea agreed |
| **10** | ~05-05 | Problem statement + system sketch | Weekly-progress docs; **host↔xv6 socket** prototype (05-06); Solar API + REPL (05-11) |
| **11** | ~05-12 | Minimal working prototype | **CFS scheduler + command dispatcher** integrated into xv6 (05-11); course-requirements doc added |
| **12** | ~05-19 | Integrated prototype + ≥1 eval metric | **Core LLM-OS features** (CFS/priority/sandbox/agent loop, 05-18); **jail-based sandbox** rewrite (05-21); **F9 cache** + security guards (05-22); **eval automation** Test 3 + `cfs_share`, CHANGELOG, Implementation.md rewrite (05-20) |
| **13** | ~05-26 | Evaluation results + dry-run | **9-test regression harness** + 3 latent-bug fixes (05-26); host-confirm escape for exec/kill; agent.py REPL/Unicode fixes |
| **14** | ~06-02 | **Final presentation** (English) | Final deliverables: README, Technical Report, Development Process, English slides |

*(KR: 오늘 기준(2026-05-28)은 Week 13. 남은 일: 영어 슬라이드, 데모 캡처,
브랜치 통합·최종 머지.)*

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

**Week 13 — hardening & dry-run (lead: Se-Joong)**
- Se-Joong: added a **9-case regression harness**, found and fixed **3 latent
  bugs**; upgraded the agent confirm/timeout behavior; fixed REPL line-editing
  and multibyte-output corruption in `agent.py`.

### 4.2 Evaluation metrics defined

- **Scheduler correctness:** `priority_test` Test 3 auto-verifies finish order
  HIGH(1)→MED(10)→LOW(19); wrong order ⇒ FAIL.
- **Fairness (quantitative):** `cfs_share` reports CPU-share % per priority
  (run with `CPUS=1`).
- **Sandbox:** `agentdemo` confirms jail confinement, `..`/outside-path denial,
  privilege-escalation denial, and `exec`/`kill` blocking.
- **Agent loop:** multi-step tool-call success + conversation-memory reuse on a
  live Solar session.

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

### M4 — Week 13 · Hardening & dry-run (around 2026-05-26)
- **Attendees:** `[fill in]`
- **Decisions:** add a regression harness before the dry-run; gate dangerous
  syscalls behind a one-time host confirmation; freeze scope (F9 in, F10 out).
- **Action items:** fix the 3 latent bugs; prepare the English slides and demo
  capture; **merge all branches for the final deliverable**.

### M5 — Week 14 · Final prep `[fill in]`
- Rehearse the English presentation; finalize README demo media.

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
| 9 | Confirm-escape branch caused a `kerneltrap` **panic** | Temporarily disabled that branch to keep the kernel stable | Se-Joong · 05-26 |
| 10 | `agent.py` tool calls timed out under slow API responses | Raised timeout 12 s → 24 s + one marker retry | Se-Joong · 05-26 |
| 11 | F6 proposal said "parse JSON in the kernel," which conflicts with xv6's no-float/no-heap rules and kernel safety | **Decision:** parse on the host; document the rationale in the report | June · 05-20 |

---

## 7. Retrospective *(회고)*

### What went well *(잘된 점)*
- **Real OS mechanisms, not a wrapper.** CFS, priority classes, a chroot jail,
  an in-kernel command queue, and new syscalls are all genuinely in the kernel.
- **Defense in depth.** Three independent layers (deny-list → blocked syscalls →
  jail) bound what the LLM can do.
- **PR + personal-branch workflow** kept the trunk buildable and gave every
  change a review.
- **Evaluation is automated** (Test 3 pass/fail, `cfs_share` numbers), not just
  eyeballed — and a regression harness caught real bugs before the dry-run.

### What was hard *(어려웠던 점)*
- Interrupt-context constraints (no fork/file-I/O in `consoleintr`) forced the
  queue + worker split.
- xv6's no-float / no-dynamic-allocation rules shaped several decisions (F6,
  integer-only CFS math, fixed-size buffers).
- Kernel-stability regressions (boot order, confirm-escape panic) cost real time
  and motivated the regression harness.

### What we'd do differently *(개선점)*
- Add the regression harness **earlier** — several latent bugs would have been
  caught sooner.
- Tighten the `.gitignore` from the start (the `mkfs` breakage hit fresh clones).
- Keep personal branches merged more frequently to avoid divergence near the
  deadline.

### Remaining before Week 14 *(남은 일)*
- Produce the **English presentation slides** (Deliverable #4).
- Capture demo screenshots / GIF for the README.
- **Merge the personal branches** into a single canonical state for submission.

> *(KR: 제출 전 반드시 — 영어 슬라이드 작성, 데모 캡처, 개인 브랜치 최종 통합.)*

---

## 8. Cross-references

- Architecture & OS concepts: [Technical_Report.md](Technical_Report.md)
- Setup / run / demo: [../README.en.md](../README.en.md) (English) · [../README.md](../README.md) (Korean)
- Code-referenced detail: [../Implementation.md](../Implementation.md)
- Feature status: [../plan.md](../plan.md) · Change log: [../CHANGELOG.md](../CHANGELOG.md)
