# Final Presentation — Speaking Script (English, ~5 min)

**Project:** OS for LLM — an agent runtime on xv6-riscv (Direction A · 2026 Operating Systems team term project)

**Big idea (one sentence):** We extended the xv6 teaching kernel so the operating system
*itself* schedules, sandboxes, and caches a live LLM agent — turning classic OS concepts
into something you can watch happen.

> This script refines the 9-bullet outline in [`README.md`](README.md) into a tighter
> **6-slide** deck (better paced for a hard 5-minute slot). Everything marked
> *(stage direction)* and the **Q&A appendix** are **NOT spoken** — do not read them on the clock.

## Timing — 4 speakers, ~5:00 total (includes a ~30 s live demo)

| Segment | Speaker | Time | Slide(s) |
|---|---|---|---|
| 1. Hook + the one big idea | Speaker 1 | 0:00–0:55 | 1 Title · 2 Problem (AIOS) |
| 2. Agent Scheduler → CFS | Speaker 2 | 0:55–2:00 | 3 CFS + weights |
| 3. Tool Manager → sandbox + dispatch | Speaker 3 | 2:00–3:20 | 4 Jail + 2-stage queue |
| 4. Live demo + numbers + close | Speaker 4 | 3:20–5:00 | 5 Demo+Eval · 6 Security/Wrap |

**Spoken length ≈ 640 words (~4:35 at 140 wpm)** — leaves ~25 s for the live demo and the
handoffs. If you speak slowly, drop the *italicised* sentences first.

---

## Speaker 1 — Hook + the big idea  · **[SLIDE 1 → 2]**

One sentence: we made the operating system *itself* host, schedule, and sandbox an AI
agent — instead of bolting the AI on top as just another program.

Our model is the 2024 *AIOS* paper: an "LLM operating system" needs three pieces — an agent
scheduler, a tool manager, and a bridge to the model, which for us is Upstage's **Solar**
model over the network. Most teams would build those three in Python on top of Linux. We
built them **inside xv6**, the tiny teaching kernel from this course.

So why is this an *operating systems* project, not an AI one? Because the LLM is only the
demo vehicle. The real content is CFS scheduling, a sandbox, new system calls, and
synchronization — and the agent makes every one of them visible. Headline result:
**sixty-five of sixty-five** regression tests pass.

*(stage direction: pause — hand off to Speaker 2)*

---

## Speaker 2 — Agent Scheduler → CFS in the kernel  · **[SLIDE 3]**

The first piece is a real **CFS scheduler** in the kernel. We ported Linux's
priority-to-weight table verbatim — forty-one weights, priorities minus-twenty to twenty —
using integer math only, because xv6 allows **no floating point and no dynamic memory**.

CFS always runs whoever has used the CPU least so far, so nothing starves. We find that
process with a simple array scan, not a red-black tree, as the assignment asked.

Does that weight table actually change how much CPU each process gets? We raced six
processes at different priorities. The highest priority took about **fifty-seven percent**
of the machine; the lowest took **under two percent** — and the measured shares track the
Linux table across all six levels. *The lowest one even lands slightly above its raw weight:
that's a minimum-vruntime floor that guarantees every process a turn, so the weakest never
starves.*

*(stage direction: pause — hand off to Speaker 3)*

---

## Speaker 3 — Tool Manager → sandbox + 2-stage dispatch  · **[SLIDE 4]**

Here's the design invariant — the whole point. A human at the shell runs with full
privilege. But every command the LLM issues runs only inside a **chroot jail**: a
locked-down folder it cannot escape — enforced by the kernel's **code path**, not by asking
the model to behave.

Three layers of defense. First, a deny-list humans can extend — but the kernel rejects any
change coming from an agent, so the agent can't weaken its own walls. Second, the kernel
blocks the dangerous syscalls outright: exec, kill, make-node. Third, the jail makes
everything outside the agent's folder invisible and refuses any "dot-dot" that would climb
above it.

One OS subtlety: serving a command can touch disk through a log transaction that may
*sleep*, and sleeping inside an interrupt handler is illegal — so we split dispatch in two.
The interrupt only **enqueues** the request; a process-context drain does the real work.
Same idea for concurrency: when four agents write one file at once, an inode sleeplock
serializes them, so their finish times step up like a staircase. And any dangerous action
sleeps until the operator answers **yes-or-no**, with a fifteen-second timeout that defaults
to *deny* — walk away, and nothing dangerous runs.

*(stage direction: pause — hand off to Speaker 4)*

---

## Speaker 4 — Live demo + numbers + close  · **[SLIDE 5 → 6]**

*(stage direction: run the live demo now — "if the demo gods are kind")*

Watch the cache. I ask the agent a question — it calls Solar once: a **MISS**. I ask the
*same* question again, and it prints "**cache HIT**" — Solar is never called. That's a
network round-trip we simply skip. On our fifty-key benchmark the cold round is all misses
and the warm round is all hits — a **fifty-percent** hit rate — and every key still hits even
though fifty keys overflow a sixteen-slot table, because a disk overlay brings the evicted
ones back.

**[SLIDE 6]** The credibility numbers: **sixty-five of sixty-five** regression scenarios —
twenty-six on shell and syscalls, thirty-nine in natural language — plus **thirteen of
thirteen** cache tests.

And honesty, because it counts. The optional idle-time training feature was out of scope. We
red-teamed our *own* code, found sixteen issues, **patched three sandbox escapes** so their
exploit tests now report SAFE — and we're openly tracking two that are still open.

One line to take away: **we didn't run an LLM on an OS — we taught the OS to host the LLM,
and every concept from this course is now something you can watch it do.** Thank you.

---

## Q&A prep — *(NON-SPOKEN: reference only)*

- **Why parse the model's JSON on the host, not in the kernel?** A parser bug in the kernel
  is a *panic*; Python's `json.loads` is battle-tested, and JSON's floats/nesting clash with
  xv6's no-float, no-heap rules. The kernel only validates a one-line `REQ|CMD|arg` format.
- **Why an array scan instead of a red-black tree for CFS?** Deliberate — the assignment's
  guidance. At our process count an O(n) leftmost-`vruntime` scan is cheap.
- **Why split dispatch into two stages?** The cache can call `begin_op()` and sleep; sleeping
  in the console ISR is illegal (`myproc()` is NULL there). Stage 1 enqueues in the
  interrupt; stage 2 routes in process context.
- **Can a jailed agent approve its own dangerous command?** That was audit finding #1 — it
  could, via `dispatch`. Fixed: `sys_dispatch` now rejects `is_agent` callers, so only the
  human/operator path can approve. The reproducer now reports SAFE.
- **What's still open?** Two: #2, `/cache.bin` resolving through the jail (`cache.c` is
  jail-unaware); and #5, the default deny-list not covering `SPAWN`. We report them rather
  than rush a risky fix — one tempting fix would have deadlocked with `clockintr`.
- **Does priority actually change agent behavior?** Yes (F8): before each tool, `agentd`
  calls `setpriority` from the tool table, so the whitelist's priority column is live
  scheduler policy.
- **Multi-agent fairness?** `agent_multi` runs four concurrent role-based agents interleaved
  by CFS over one shared kernel — fair scheduling plus per-role access control.

---

# Strict 4-minute safety cut

A tighter version for a hard 4-minute slot or a cautious pace. **Spoken length = 463 words**
(deterministically counted), which is **~3:34 at 130 wpm** and lands at **~3:57 with a quick
live demo + handoffs** — under 4:00 with margin. Same facts, same 4 speakers; one slide each.

> **Cautious-pace fallback:** at ~120 wpm the demo pushes you to ~4:14. If you're worried
> about pace, *describe* the cache hit instead of running it live (saves ~15 s), or cut the
> Speaker-2 line *"A minimum-vruntime floor still guarantees every process a turn."*

| Segment | Speaker | Slide | ~Words |
|---|---|---|---|
| Hook + big idea | 1 | 1 Title | 104 |
| CFS scheduler | 2 | 2 CFS | 115 |
| Sandbox + 2-stage dispatch | 3 | 3 Jail | 132 |
| Cache demo + numbers + close | 4 | 4 Demo/Numbers | 112 |

### Speaker 1 · **[SLIDE 1]**
One sentence: we made the operating system itself host, schedule, and sandbox an AI agent —
instead of bolting the AI on as just another program. Following the 2024 *AIOS* model, an LLM
operating system needs a scheduler and a bridge to the model — for us, **Solar**. We built it
inside **xv6**, the course's teaching kernel. So why is this an OS project, not an AI one?
Because the LLM is only the demo vehicle. The real content is scheduling, a sandbox, system
calls, and synchronization — and the agent makes every one visible. Headline: **sixty-five of
sixty-five** tests pass. *(handoff)* First, the scheduler.

### Speaker 2 · **[SLIDE 2]**
The first piece is a real **CFS — Completely Fair Scheduler —** in the kernel. We ported
Linux's priority-to-weight table exactly: forty-one weights, integer math only, because xv6
allows no floating point and no dynamic memory. CFS always runs whoever has used the CPU
least, so nothing starves. We find that process with a simple array scan, not a red-black
tree, as the assignment asked. To prove it's real, we raced six processes at different
priorities. The highest took about **fifty-seven percent** of the machine; the lowest, **under
two percent** — tracking the Linux table across all six levels. A minimum-vruntime floor still
guarantees every process a turn. *(handoff)* Now, how we keep the agent caged.

### Speaker 3 · **[SLIDE 3]**
Here's the design invariant — the whole point. A human at the shell runs with full privilege.
But every command the LLM issues runs only inside a **chroot jail** — a locked-down folder it
cannot escape — enforced by the kernel, not by trusting the model. The kernel rejects any
deny-list change from an agent, so the agent can't weaken its own walls; and the jail makes
everything outside that folder invisible and refuses any "dot-dot" that climbs above the root.
One OS subtlety: serving a command can *sleep*, which is illegal inside an interrupt handler —
so the interrupt only **enqueues** the request, and a process-context drain does the real
work. And any dangerous action waits for a **yes-or-no**, with a fifteen-second timeout that
defaults to deny. *(handoff)* Now, watch it run.

### Speaker 4 · **[SLIDE 4]**
*(live demo, ~15 s)* Watch the cache. I ask the agent a question; it calls Solar once — a
**MISS**. I ask again; it prints **cache HIT**, and Solar is never called. On our fifty-key
benchmark, the cold round all-misses and the warm round all-hits — a **fifty-percent** hit
rate. The numbers: **sixty-five of sixty-five** regression scenarios, plus **thirteen of
thirteen** cache tests. We red-teamed our *own* code, patched three sandbox escapes so their
exploit tests now report SAFE, and we're openly tracking two still open. One line to take
away: **we taught the OS to host the LLM — and every concept from this course is now something
you can watch it do.** Thank you.

*(Q&A: reuse the 7-question appendix above — it applies unchanged to this cut.)*
