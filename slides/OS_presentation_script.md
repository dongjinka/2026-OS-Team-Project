# Final Presentation — Speaking Script v2.2 (English, 5:00 = 4:00 talk + 1:00 demo video)

**Project:** OS for LLM, an agent runtime on xv6-riscv (Direction A · 2026 Operating Systems team term project)

**Big idea (one sentence):** We extended the xv6 teaching kernel so the operating system
*itself* schedules, sandboxes, and caches a live LLM agent, and 65/65 regression tests prove it.

> v2.2: trimmed for a strict 4:00 — benchmark recitals and demo transcripts are out
> (the video shows the agent working; slides explain the mechanisms). v2.1: humanizer
> pass. Audit story matches `origin/main` (two audits, seven fixes via PR #13/#14 and
> PR #16). Everything marked *(stage direction)* and the **Q&A appendix** are **NOT spoken**.

## Timing — 4 speakers + 1-minute demo video, 5:00 total

| Segment | Speaker | Time | Slide(s) |
|---|---|---|---|
| Direction + our idea + headline result | S1 | 0:00–0:50 | 1 Cover · 2 Thesis · 3 What we built |
| How CFS works | S2 | 0:50–1:35 | 4 §01 · 5 CFS mechanism |
| How the sandbox + dispatch work | S3 | 1:35–2:30 | 6 §02 · 7 Jail · 8 Dispatch |
| **Demo video (1:00)**, S4 points over it | S4 | 2:30–3:30 | 9 §03 (video cue) |
| Honest audit + close | S4 | 3:30–4:40 | 10 Audit · 11 Close |

**Spoken length ≈ 430 core words outside the video**, about 3:25 at 135 wpm including
handoffs, with real margin for a careful pace. Sentences average under 10 words, one
breath each. Lines in *italics* are pace buffers: speak them only if you're running fast.

---

## Speaker 1 — Direction, our idea, headline  · **[SLIDE 1 → 2 → 3]**

Hi everyone. Our team picked Direction A: "OS for LLM." Instead of bolting an
AI on top of an operating system, we make the operating system itself host
the AI.

Here's where our idea came from. A 2024 paper called AIOS says an LLM
operating system needs three parts: an agent scheduler, a tool manager, and a
bridge to the model. For us, that model is Upstage's Solar. Most teams would
build those in Python, on Linux. We built all three inside xv6, the tiny
teaching kernel from this course.

And it works. Sixty-five out of sixty-five regression tests pass. Here's what
we built. First, the scheduler.

*(hand off to Speaker 2)*

---

## Speaker 2 — "Who gets the CPU?" → a real CFS  · **[SLIDE 4 → 5]**

So, what happens when several agents want the CPU at the same time? That's
why we built our first piece: a real CFS, a Completely Fair Scheduler,
inside the kernel.

Every timer tick, the running process is charged vruntime, weighted by its
priority. The weights come from Linux's table, all forty-one of them, in
integer math only, because xv6 has no floating point and no heap. To pick the
next process, we scan for the lowest vruntime, just like the assignment
asked. *And a floor on vruntime means even the lowest priority always gets a
turn.* So nothing ever starves. Next question: why would we trust this thing?

*(hand off to Speaker 3)*

---

## Speaker 3 — "Why trust the AI?" → jail + two-stage dispatch  · **[SLIDE 6 → 7 → 8]**

We don't.

A human at the shell runs with full privilege. But every command the LLM
issues runs inside a chroot jail, a locked folder it can't escape. And that's
enforced by the kernel's code path, not by asking the model to behave. Three
layers: a deny-list the agent can't edit, blocked syscalls like exec and
kill, and the jail itself, which refuses any dot-dot that tries to climb out.

Building this taught us a real OS lesson. Serving a command can touch the
disk and sleep. And sleeping inside an interrupt handler is illegal. So we
split dispatch in two: the interrupt just queues the request, and a normal
process drains it later, where sleeping is fine.

Now, watch it run.

*(The confirm-escape gate, where dangerous actions wait for a human "yes" and
time out to deny, is covered by Speaker 4's narration over the video, so it's
not spoken here. If the video is cut, restore one line: "And any dangerous
action waits for a yes from the operator, and no answer means no.")*

*(hand off to Speaker 4; start the demo video)*

---

## Speaker 4 — over the demo video (1:00)  · **[SLIDE 9 + video]**

*(speak sparsely, synced to the video; these lines don't count against the 4:00)*

- "This is our kernel, live. I ask a question, and it calls Solar once. That's a miss."
- "The same question again: cache HIT. Solar is never called. The cache is a sixteen-slot kernel table, backed by a file on disk."
- "Here, the jail blocks a read outside the agent's folder."
- "And a dangerous request with no answer: fifteen seconds, then denied by default."

---

## Speaker 4 — honesty, close  · **[SLIDE 10 → 11]**

One more thing, because honesty counts. We attacked our own kernel. Twice.
Those two audits fixed seven real issues, including three sandbox escapes.
Two are still open. We name them in the repo, instead of rushing a risky fix.
And through all of it, the sixty-five tests stayed green.

So, one line to take away. We didn't run an LLM on an OS. We taught the OS
to host the LLM, and every concept from this course is now something you can
watch it do. Thank you.

---

## Q&A prep — *(NON-SPOKEN: reference only)*

- **Why parse the model's JSON on the host, not in the kernel?** A kernel parser
  bug is a *panic*; Python's `json.loads` is battle-tested, and JSON's floats and
  nesting clash with xv6's no-float, no-heap rules. The kernel validates only a
  one-line `REQ|CMD|arg` format.
- **Why an array scan instead of a red-black tree for CFS?** Deliberate; the
  assignment's guidance. At our process count an O(n) lowest-`vruntime` scan is cheap.
- **Why split dispatch into two stages?** The cache can call `begin_op()` and sleep;
  sleeping in the console ISR is illegal (`myproc()` is NULL there). Stage 1 enqueues
  in the interrupt; stage 2 routes in process context.
- **What exactly did the two audits fix?** First audit (PR #13/#14): confirm-escape
  self-approval, renice privilege self-scope, wire injection. Second, full-codebase
  audit (PR #16): a deny-list bypass on cache hits, cache wire-line forgery, a re-jail
  inode leak, and a filename misparse. Total seven fixed.
- **What's still open?** Two named items: #2, `/cache.bin` resolving differently inside
  the jail (split-brain); and #5, `SPAWN` missing from the default deny-list. Four more
  low-risk items (#6–#9) are documented as accepted/deferred with reasons in
  `docs/SECURITY.md`; we report rather than rush.
- **Does priority actually change agent behavior?** Yes (F8): before each tool,
  `agentd` calls `setpriority` from the tool table, so the whitelist's priority column
  is live scheduler policy.
- **Multi-agent fairness?** `agent_multi` runs four concurrent role-based agents
  interleaved by CFS over one shared kernel: fair scheduling plus per-role access control.
- **Where's F10 (idle-time training)?** Out of scope; infeasible on xv6 (RISC-V,
  no float, tiny memory). Documented in the README.

## Fallback — if the video fails to play

Speaker 4 describes it instead (~20 s): "Live, it looks like this: I ask a question and
the kernel calls Solar once, a miss. I ask the same question again and it answers from
its own sixteen-slot cache. Solar is never called. And a dangerous request with no
answer times out to deny." Then continue with the audit as written.
