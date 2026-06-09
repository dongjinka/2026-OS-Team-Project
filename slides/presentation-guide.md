# BUILD GUIDE — "OS for LLM" Final Presentation Deck

A slide-by-slide guide to **hand-build** a 6-slide deck for a 5-minute, 4-speaker Operating Systems final talk. Tool-agnostic (Claude design feature / Canva / PowerPoint). Each slide block is self-contained and paste-able into Marp.

> **Design decision — no code/terminal screenshots.** Per team preference, every terminal transcript on the slides is rendered as a **styled text panel** (real output, recolored — crisper than a PNG, version-controlled, and recolorable), *not* a screenshot capture. The exact panels are already implemented in [`deck.md`](deck.md) (the `.term` style). Wherever a slide below names a "screenshot `x.png`", use the matching **text panel** from `deck.md` instead. The `docs/assets/*.png` captures stay only as a README fallback.

---

## Assets — terminal output is text (no screenshots); build 2 graphics

The slides render terminal output as **text panels** (see the callout above), so the only *image* assets you must **create** are the **2 built graphics** in §B (the CFS bar chart and the data-path diagram). The 8 `solar-pro2` capture PNGs (2026-06-08, commit `70858a0`) sit in `docs/assets/` as a fallback, but the deck no longer embeds them — the table below now documents **what each text panel should say** (and how to re-capture the wording if you need to verify it).

**A. Terminal text panels** (build as `.term` panels — dark `#0b1220`, monospace, real output; copy the exact strings from [`deck.md`](deck.md)):

| Slide | Panel text (real output) | Color this token |
|---|---|---|
| 5 · Demo | `you ▸ In one short sentence, what is a system call?` → `[cache HIT] answer reused from kernel F9 cache (Solar not called)` + the boxed answer | `[cache HIT]` **amber** |
| 3 · Sandbox | `[agentd] READ: '/etc/passwd' not reachable inside jail` · `[agentd] NICE: denied (pid=1 prio=19)` | both denials **red** |
| 4 · Dispatch | `[jail] pid=5 requests dangerous syscall 'exec' — allow within 15s? (y/N)` → times out → **DENY** | `DENY` **red** |
| 6 · Audit | `$ python3 tools/sec_audit.py` → `#1 secconfirm: SAFE  #3 secnice: SAFE  regression: 65/65` | each `SAFE` **green** |

> These are the **real** strings (verified in `agent.py` / the README transcripts). Rendered as text they stay crisp at any zoom, diff in git, and let you color the key token — see the `.term` style in `deck.md`. The `docs/assets/*.png` captures remain only as a README fallback.

**B. Render the 2 built graphics** → save into `slides/assets/`:

| File | Source of truth |
|---|---|
| `slides/assets/cfs-share.png` | The 6-bar measured-vs-expected chart. Numbers come **verbatim** from `docs/BENCHMARKS.md` (regenerate via `cd xv6-riscv && make kernel/kernel fs.img && cd .. && python3 tools/bench_report.py`). |
| `slides/assets/datapath.svg` | The two-lane gate diagram. Gates = `deny-list → /agentbox jail → confirm-escape`; the human shell lane bypasses all three (design invariant, per `Technical_Report.md`). |
| `slides/assets/write-race.png` *(optional, Slide 4)* | The `write_race` "staircase": 4 concurrent writers serialized by an inode sleeplock. Run `user/write_race` and chart finish-tick order. |

Export the finished deck into `slides/`.

---

## Deck at a glance

| # | Title | Speaker | Visual |
|---|-------|---------|--------|
| 1 | We Taught the OS to Host the AI | S1 (Hook) | `datapath.svg`: human bypasses gates / LLM passes all 3 |
| 2 | Real CFS Scheduling, Inside the Kernel | S2 (CFS) | `cfs-share.png` (6 paired bars) |
| 3 | The Sandbox: Human Free, AI Jailed | S3 (Sandbox) | 3-layer stack + **text panel** (READ / NICE denials) |
| 4 | Sleep Is Illegal in an Interrupt Handler | S3 (Sandbox) | 2-stage dispatch diagram + **text panel** (confirm-escape) |
| 5 | Live Demo: Kernel Cache + the Numbers | S4 (Demo) | **text panel** (cache HIT transcript) + `65/65` · `13/13` · `50%` band |
| 6 | Honest Engineering & Close | S4 (Close) | Audit scorecard (fixed/open) + **text panel** (`sec_audit` SAFE) |

> **Strict 4-minute cut:** merge Slide 4 into Slide 3 (keep only the 2-stage diagram as a half-panel) for a 5-slide run. Build all 6; hide Slide 4.

---

## Design system

- **Palette:** dark slate background `#0F172A`; primary text near-white `#F1F5F9`; secondary gray `#94A3B8`. **One accent** = amber `#F59E0B` for the 1–2 emphasized words and key numbers. A single muted "danger" red `#EF4444` ONLY for deny/blocked items. Nothing else colored.
- **Type:** sans-serif for headlines/bullets (Inter / Helvetica / Calibri). **Monospace** (JetBrains Mono / Consolas) for ALL code, wire text, and syscall names — `REQ|CMD|arg`, `exec/kill/mknod`, `[cache HIT]`. Monospace signals "real machine text."
- **Icon motif:** a **gate / lock** glyph for the sandbox path; a **scale / balance** glyph for the scheduler. Line-style, one weight, accent or gray only.
- **Hard rules:** ONE idea per slide · **≤ 6 lines** of text · **visual-first** (the diagram or terminal **text panel** dominates ~60%; bullets are the caption, not the content). Bullets are labels, not sentences. Expand each acronym on first on-slide use.
- **Terminal panels:** real output rendered as **monospace text** on a dark `#0b1220` card — never a screenshot. Color one token per panel (HIT amber · denials red · SAFE green).
- **Notes discipline:** the **Speaker notes** below go in the notes pane ONLY — never paste them onto a slide.

---

### Slide 1 — We Taught the OS to Host the AI

- **Speaker / time:** Speaker 1 (Hook) · ~50s
- **One-line goal:** This is an OS project — the LLM is the demo vehicle; the OS is the content.
- **On-slide text:**
  - **Headline:** The OS hosts the AI — not bolted on
  - Kernel **schedules · sandboxes · caches** one AI agent
  - Built *inside* **xv6-riscv** teaching kernel
  - **AIOS** model: Scheduler · Tool Manager · LLM bridge
  - *(footnote, small: AIOS = AI-as-OS, Mei et al. 2024)*
- **Visual:** `slides/assets/datapath.svg`. Two lanes feeding the kernel: a **human shell** lane (green arrow, "unrestricted") goes *straight* to the kernel, bypassing every gate; an **LLM agent** lane must pass three stacked gates `deny-list → /agentbox jail → confirm-escape` before reaching the kernel. Label the gate row "every AI command, three checks."
- **Speaker notes:** "Instead of running an LLM as just another app, we extended the xv6 teaching kernel so the kernel itself hosts, schedules, sandboxes, and caches an AI agent — following the AIOS paper's three components. Why is this an OS project, not an AI one? The LLM is only the demo vehicle; the real content is scheduling, a sandbox, system calls, and synchronization. This two-lane diagram is the whole talk in one picture: the human goes straight through, the AI passes three gates."
- **Design note:** Diagram dominates center; headline top-left, bullets as a thin left rail. The two-lane gate diagram is the visual thesis — make it the biggest object. **Emphasis:** color "hosts" amber; the human lane green, the gate row red. Monospace the gate labels.

---

### Slide 2 — Real CFS Scheduling, Inside the Kernel

- **Speaker / time:** Speaker 2 (CFS) · ~50s
- **One-line goal:** We ported a real Completely Fair Scheduler into the kernel, and it tracks the Linux weight table.
- **On-slide text:**
  - **Headline:** CFS = Completely Fair Scheduler, ported to xv6
  - Linux `sched_prio_to_weight`: **41 weights**, prio −20..20
  - **Integer math only** (xv6 forbids float + heap)
  - Lowest **vruntime** via array scan, not red-black tree
  - `cfs_min_vruntime` floor = **no starvation**
  - *(footnote, small: vruntime = CPU time used, weighted)*
- **Visual:** `slides/assets/cfs-share.png` — 6 priority groups on X, **measured vs expected** CPU share as paired bars: prio0 **56.9 / 59.1**, prio4 **23.9 / 24.4**, prio8 **9.7 / 9.9**, prio12 **5.2 / 4.0**, prio16 **2.5 / 1.7**, prio19 **1.9 / 0.9** (all %). No screenshot — the chart carries it alone (`deck.md` builds these bars in pure CSS).
- **Speaker notes:** "This is a real CFS inside the kernel: we ported Linux's 41 priority weights verbatim, using integer math only because xv6 forbids floating point and dynamic allocation, and we pick the lowest-vruntime task — that is, the task with the least weighted CPU time — by scanning an array instead of a red-black tree, as the assignment required. Racing six processes, priority 0 takes about 57% of the CPU and priority 19 under 2%; the shares track the Linux weight table top to bottom. The slight excess at the very low end is the min-vruntime floor doing its job: no task ever starves."
- **Design note:** Chart fills ~65% (right/center); the monotonic staircase IS the argument. Bullets are a thin caption rail on the left. **Emphasis:** "Integer math only" and "no starvation" amber; color the measured bars amber, expected bars gray. Monospace the weight-table and vruntime names.

---

### Slide 3 — The Sandbox: Human Free, AI Jailed

- **Speaker / time:** Speaker 3 (Sandbox) · ~50s
- **One-line goal:** The design invariant — humans run unrestricted, every AI command is jailed — enforced by code, not convention.
- **On-slide text:**
  - **Headline:** Human input free · every AI command jailed
  - Enforced by the **code path**, not by trust
  - L1 deny-list `{KILL,EXEC}` — kernel rejects agent edits
  - L2 kernel blocks `exec / kill / mknod`
  - L3 **chroot jail** = locked folder, no escape; `..` refused
- **Visual:** A **3-layer stack** graphic (gate icons), one row per layer L1→L2→L3, with an AI command flowing down through all three. Below it, a **terminal text panel** (not a screenshot) with the real denials — `[agentd] READ: '/etc/passwd' not reachable inside jail` and `[agentd] NICE: denied (pid=1 prio=19)`, both in **red**. (Exact panel in `deck.md`.)
- **Speaker notes:** "The whole point is one invariant: a human at the shell runs unrestricted, but every command the LLM issues runs only inside a /agentbox chroot jail — a locked-down folder the process can't escape — and this is enforced by the code path, not by convention. Three layers: a configurable deny-list the kernel refuses to let an agent process edit; the kernel blocking dangerous system calls like exec, kill, and mknod; and the jail, which makes outside files invisible and refuses any dot-dot above the jail root. Here you can see a jailed agent's attempt to renice init get denied."
- **Design note:** The text panel sits below the bullets (full width); the layer stack center-left. **Emphasis:** color "free" green and "jailed" red in the headline; the two denials red inside the panel; red lock icon on L2/L3. Monospace the panel and the syscall / deny-list text.

---

### Slide 4 — Sleep Is Illegal in an Interrupt Handler

- **Speaker / time:** Speaker 3 (Sandbox, cont.) · ~45s — *(cut for the 4-min run; fold the 2-stage diagram into Slide 3)*
- **One-line goal:** Real work in an interrupt handler can sleep — which is illegal — so we split into ISR-enqueue + process-context drain, and the same care shows in our locking.
- **On-slide text:**
  - **Headline:** You cannot sleep in an interrupt handler (ISR)
  - Serving a command hits disk → `begin_op` may sleep
  - In the ISR `myproc() == NULL` → no sleeping allowed
  - Fix: **ISR enqueues** ring → **process context drains**
  - Lock proof: inode sleeplock serializes 4 writers
- **Visual (hero):** The **2-stage dispatch diagram** — serial bytes → `consoleintr` (ISR box, "short, no sleep") → **ring buffer** → `agent_drain` (process-context box, "real routing, may sleep") → deny-list / jail / confirm-escape. **Secondary:** a one-line **text panel** — `[jail] pid=5 requests dangerous syscall 'exec' — allow within 15s? (y/N)` → times out → **DENY** (red); optionally the `write-race.png` inode-sleeplock "staircase" built chart.
- **Speaker notes:** "Here's a real OS subtlety. Serving an agent command can touch the disk through a log transaction that may sleep — but the console interrupt handler runs with no process context, where sleeping is illegal. So the interrupt only enqueues bytes to a ring buffer and stays short; the real routing happens later in process context, which is allowed to sleep. The same care shows in synchronization: an inode sleeplock serializes four concurrent writers into a clean staircase, and our confirm-escape gate sleeps until the host answers — a 15-second timeout defaults to deny."
- **Design note:** The left-to-right pipeline is the hero (~60%). Keep the ISR box visually small/fast and the drain box wide/heavy to encode the idea. The confirm-escape text panel sits below, compact, so it doesn't compete with the pipeline. **Emphasis:** "cannot sleep" and "ISR enqueues" amber; monospace `begin_op`, `myproc()`, `consoleintr`, `agent_drain`.

---

### Slide 5 — Live Demo: Kernel Cache + the Numbers

- **Speaker / time:** Speaker 4 (Demo + numbers) · ~50s
- **One-line goal:** Watch the kernel cache skip the network, then land the verification numbers.
- **On-slide text:**
  - **Headline:** Same question again → **kernel cache HIT** (Solar not called)
  - F9 cache: 16-slot RAM + `/cache.bin` disk overlay
  - `eval cache 50` → cold 50 miss, warm 50 hit
  - **50% hit-rate** on 50 keys despite 16 slots
  - Tests: **65/65** regression · **13/13** cache
- **Visual:** A **terminal text panel** (not a screenshot) as the dominant element: `you ▸ In one short sentence, what is a system call?` → `[cache HIT] answer reused from kernel F9 cache (Solar not called)` (`[cache HIT]` in **amber**) + the boxed answer. Below it, a **stat band** of three big numbers: `65/65` · `13/13` · `50%`.
- **Speaker notes:** "Live: I ask a question, then ask it again — the second time the kernel answers from its F9 cache and the Solar model is never called, so we skip the whole network round-trip. The cache is a 16-slot RAM table backed by a /cache.bin disk overlay, so running eval over 50 keys is all misses cold and all hits warm — a 50% hit-rate even though 50 keys overflow the 16 slots, because evicted keys are promoted back from disk. For verification: 65 of 65 regression tests — that's 26 shell-and-syscall plus 39 natural-language — and 13 of 13 cache tests, all green."
- **Design note:** The text panel dominates the top ~55%; the three numbers in one horizontal band underneath. This is the emotional peak — the biggest type on this slide is the numbers. **Emphasis:** "HIT", "50%", "65/65" amber; monospace the whole panel + `[cache HIT]`, `/cache.bin`, `eval cache 50`.

---

### Slide 6 — Honest Engineering & Close

- **Speaker / time:** Speaker 4 (Close) · ~45s
- **One-line goal:** We red-teamed our own kernel, fixed what we found, named what's open — then the close.
- **On-slide text:**
  - **Headline:** We attacked our own code and fixed it
  - Fixed **7**: #1/#3/#4 (PR #13/#14) + #10–#13 (PR #16)
  - Open & named: **#2** cache split-brain · **#5** SPAWN not denied
  - 65/65 regression held green
  - Out of scope: **F10** idle-time model training
- **Visual:** A compact **audit scorecard**: left card "FIXED · 7" (green top) `#1 #3 #4` (PR #13/#14) + `#10–#13` (PR #16); right card "OPEN" (red top) `#2` split-brain · `#5` SPAWN. Below the cards, a one-line **text panel** — `$ python3 tools/sec_audit.py` → `#1 secconfirm: SAFE  #3 secnice: SAFE  65/65` (each `SAFE` **green**) — so "we attacked our own code" is concrete, not asserted. Footer ribbon with the close line.
- **Speaker notes:** "We held ourselves to honest engineering — two security audits of our own kernel. The first fixed three sandbox escapes: self-approval of dangerous syscalls, privilege self-scope on renice, and wire injection — via team review in PR #13 and #14, with reproducers that flip from vulnerable to safe. A second, full-codebase sweep then found four more in previously-untouched code — a deny-list bypass, cache wire-forgery, an inode leak, and a filename misparse — all fixed in PR #16. Two remain open and we name them: a cache split-brain between the jailed and host views, and SPAWN missing from the default deny-list; all 65 regression tests held throughout. And idle-time model training — F10 — is out of scope: infeasible on xv6, with RISC-V, no floating point, and tiny memory. We didn't run an LLM on an OS; we taught the OS to host the LLM, and every concept from this course is now something you can watch it do."
- **Design note:** Two-column scorecard up top, the `sec_audit` text panel below it; the closing sentence in a full-width ribbon at the bottom in accent amber — the last thing the room reads. End on the close line, not the table. **Emphasis:** "attacked our own code" amber; red ONLY on the "Open" line. *(Grounding note: this matches `origin/main` — `docs/SECURITY.md` documents the first audit (#1/#3/#4 fixed, PR #13/#14) AND the 2026-06-09 full-codebase audit (#10–#13 fixed, PR #16); #2/#5 remain open. All committed on `main`, so the claims are safe — just present from `main`, not this older `Dongjin` branch.)*

---

## Appendix / backup slides (build but hide)

- **A1 — Full architecture diagram:** complete one natural-language (NL) turn data path — `agent.py → Solar JSON → {tool,args} → REQ|CMD|arg over serial → consoleintr enqueue → agent_drain route → ASK/F9 cache | tool→deny-list→agentq→jailed agentd via sys_agent_recv | exec/kill/mknod→confirm-escape v2`. For deep-dive questions.
- **A2 — Q&A facts:** F6 model-JSON parsed on the HOST (`agent.py`); the kernel validates only a one-line `REQ|CMD|arg` format (JSON clashes with no-float / no-heap; a kernel parser bug = panic). Boots clean `smp=1` and `smp=3`; agent mode is single-core only. Regression = `ralph_battery` 26 + `ralph_natlang` 39 = 65; `cache_test` 13/13; isolated ports (5555 / 6666) so suites coexist with a live 4444 session. Concurrency proofs: `write_race` staircase via inode sleeplock; `agent_multi` = 4 role-based agents interleaved by CFS. Team roles: Se-Joong Kim (integration — jail/cache/confirm-escape/regression), SeungBeom Kim (core CFS/sandbox/agent loop/security guards), Seongjun Gong (eval automation, English docs, F6 decision), Dongjin Ka (repo/PR hosting, 06-04 security audit).
- **A3 — Cache internals (if pressed):** 16-slot RAM table + `/cache.bin` disk overlay (evicted keys promoted back) + MinHash/Jaccard semantic match. Keep MinHash/Jaccard OFF the main slides — backup only.

**Build-order tip:** No screenshots to make — the terminal panels are **text** (copy the exact strings from `deck.md`). Build the **2 graphics first** — `datapath.svg` (Slide 1) and `cfs-share.png` (Slide 2) carry the talk's thesis and proof and deserve your best polish time; the text panels and the stat band are quick drop-ins.