# Technical Report — LLM-OS: An Agent Runtime on xv6

**Project:** LLM-OS (Direction A — *OS for LLM*)
**Course:** 2026 Operating Systems, Team Project (Weeks 9–14)
**Backend LLM:** Upstage Solar Pro (`solar-pro2`)
**Implementation branch described here:** `main` (canonical / fullest state)

> *(KR: 본 보고서는 영어를 본문으로 하고, 핵심 개념에는 짧은 한국어 주석을
> 덧붙인다. 코드 라인 단위의 상세 설명은 [Implementation.md](../Implementation.md)
> 를 참고. 본 보고서는 `main` 브랜치 구현(F9 캐시·평가 하네스 포함)을 기술한다.)*

---

## 1. Problem Statement & Goal

Modern "agentic" LLM systems (coding agents, tool-using assistants) need an
execution substrate that can **schedule** concurrent agent work fairly,
**mediate tool calls**, **contain** what generated code may touch, and **cache**
repeated work. The AIOS paper frames the first three as an *Agent Scheduler*, a
*Tool Manager*, and an *LLM-Kernel bridge*. Most student projects implement this
as a thin Python wrapper on Linux — which the course explicitly disqualifies.

**Our goal:** implement those responsibilities as *real kernel mechanisms*
inside **xv6-riscv**, so the operating-system content is something we designed
and built, not merely "it runs on Linux."

The guiding invariant throughout:

> **Human shell commands run with full privilege; every command the LLM issues
> runs only inside a sandbox.** This boundary is enforced by separate code paths
> in the kernel, not by policy.

*(KR: 사람 명령은 풀 권한, LLM 명령은 샌드박스 안에서만 — 두 경로를 커널에서
물리적으로 분리한다.)*

---

## 2. System Architecture

### 2.1 Block diagram

```
 ┌──────────────────┐  natural language
 │  User (REPL)     │ ─────────────────────────►
 └────────┬─────────┘
          │
          ▼
 ┌────────────────────┐  HTTPS   ┌────────────────────────┐
 │  agent.py          │─────────►│  Upstage Solar Pro     │
 │  (ReAct loop)      │◄─────────│  api.upstage.ai/v1     │
 │  • conversation     │   JSON   └────────────────────────┘
 │    memory          │
 │  • REQ| wire encode │
 │  • :ask → F9 cache  │
 └────────┬───────────┘
          │ TCP 4444  (QEMU serial)
          ▼
 ┌──────────────────────────────────────────────────────────────────┐
 │                          xv6 kernel                                │
 │                                                                    │
 │  human input ─► sh (shell)                                         │
 │                                                                    │
 │  REQ| line ─► consoleintr ─► agent_dispatch  (ISR: enqueue only)   │
 │                                   │                                │
 │  agent_drain (process context) ─► agent_dispatch_now: route        │
 │    ├─ ASK ─► F9 cache lookup                                       │
 │    │         · HIT  → forward answer to agentd (Solar skipped)     │
 │    │         · MISS → LLM_REQ to host ─► LLM_RESP ─► cache_set     │
 │    └─ else ─► deny-list check (configurable) ─► agentq enqueue     │
 │                                   │ sys_agent_recv (is_agent only) │
 │                                   ▼                                │
 │   ┌────────────────────────────────────────────────────────────┐ │
 │   │  user/agentd   (jailed: chroot=/agentbox, is_agent=1)       │ │
 │   │   tools: PRINT CHAT READ WRITE LS NICE LIST SETPRIO PS HELP │ │
 │   │   exec / kill / mknod → denied                             │ │
 │   │   before each tool: setpriority(self, fn.priority)          │ │
 │   └────────────────────────────────────────────────────────────┘ │
 │                                                                    │
 │   ┌────────────────────────────────────────────────────────────┐ │
 │   │  CFS scheduler  — cfs_weight[41] · vruntime ·              │ │
 │   │  cfs_min_vruntime · fork inheritance · wakeup bonus         │ │
 │   └────────────────────────────────────────────────────────────┘ │
 │                                                                    │
 │   cache.c  (RAM table + /cache.bin overlay + MinHash/Jaccard)      │
 │   deny-list (spinlock-guarded RAM + /denylist.conf persistence)    │
 └──────────────────────────────────────────────────────────────────┘
```

### 2.2 Three mapped responsibilities *(AIOS → our kernel)*

| AIOS responsibility | Our mechanism | Files |
| ------------------- | ------------- | ----- |
| **Agent Scheduler** | In-kernel CFS with Linux weights; new processes seeded at global `min_vruntime` | `kernel/proc.c`, `kernel/trap.c` |
| **Tool Manager** | 2-stage command dispatch + configurable deny-list + ring-buffer queue + jailed `agentd` with a tool whitelist + F9 cache orchestration | `kernel/agentcmd.c`, `kernel/deny.h`, `kernel/cache.c`, `kernel/sysproc.c`, `user/agentd.c` |
| **LLM-Kernel bridge** | Host ReAct loop over the QEMU serial port; `REQ\|` wire protocol; observation markers; `:ask` cache path | `agent.py` |

---

## 3. Tech Stack

| Layer | Technology | Why |
| ----- | ---------- | --- |
| Kernel | xv6-riscv (C, rv64) | Small, readable, fully modifiable teaching kernel |
| Emulator | QEMU `qemu-system-riscv64` (≥ 7.2) | Standard xv6 target; serial can be redirected to TCP |
| Transport | QEMU `-serial tcp:127.0.0.1:4444` | Lets a host process speak to the kernel console |
| Host bridge | Python 3 + `openai` SDK | Solar is OpenAI-API-compatible (swap `base_url`/`api_key`) |
| LLM | Upstage Solar Pro (`solar-pro2`) | Course-provided backend |
| Build / test | GNU Make; `tools/regression.py` | xv6 build + a multi-case regression harness |

No floating point and no dynamic allocation are used in any kernel-side
addition — both are restricted in xv6, so all new kernel code uses fixed-size
buffers and integer-only arithmetic.

*(KR: 커널 측 추가 코드는 부동소수·동적할당을 일절 쓰지 않음 — xv6 제약 준수.)*

---

## 4. OS Concepts in Play (and exactly where)

This section is the heart of the course requirement: **which OS concepts we
designed and implemented, and where they live.**

### 4.1 Scheduling — a CFS-style scheduler *(F3/F4)*

**Concept:** fair-share CPU scheduling by virtual runtime, the way Linux's CFS
works.

- **Weight table** — `kernel/proc.c` ports Linux's `sched_prio_to_weight` as
  `cfs_weight[41]` covering priority −20..20. Higher weight (lower "nice") ⇒
  `vruntime` grows more slowly ⇒ more CPU.
- **Per-tick accrual** — on every timer interrupt, `kernel/trap.c` adds
  `cfs_vdelta(prio) = (2^20 · NICE_0_LOAD) / weight` to the running process's
  `vruntime`. Integer-only; `uint64` makes overflow a non-issue.
- **Pick policy** — the scheduler scans the process table and picks the
  **leftmost** (minimum `vruntime`) RUNNABLE process; ties broken by
  `creation_tick` (older first). We deliberately use an **array scan, not a
  red-black tree**, per the assignment's "no RB-tree" guidance.
- **Global `min_vruntime`** — a monotone global `cfs_min_vruntime` guarded by
  `cfs_lock`, mimicking Linux's `cfs_rq->min_vruntime`. New processes start at
  `cfs_min()` so they neither starve nor unfairly dominate.
- **Fork inheritance (F4)** — a child inherits the parent's `vruntime`.
- **Wakeup compensation (F4)** — a process waking from I/O is set to
  `max(vruntime, min_vruntime) − BONUS`: the `max` floors it (so a long sleeper
  can't monopolize the CPU with a tiny `vruntime`), the small bonus rewards
  interactivity.

### 4.2 Processes & priority classes *(F1/F2)*

**Concept:** per-process priority + a privilege distinction between system and
user processes.

- Priority range extended from 0..20 to **−20..20**; **negative = kernel
  class.** `procdump` (`^P`) marks kernel-class as `[K]` and agents as `[A]`.
- New syscalls **`setpriority(pid, prio)`** and **`getpriority(pid)`**.
- **Two-way privilege guard** (`kernel/sysproc.c`):
  1. *No escalation* — a user-class process (priority ≥ 0) cannot grant a
     *negative* priority to anyone: `if(prio < 0 && myproc()->priority >= 0) return -1;`
  2. *Kernel-class protection* — a user-class caller cannot **demote** a target
     that is already kernel-class (e.g. `init`); only a kernel-class caller can.
     This stops a jailed agent from using `NICE` to drag `init` into the user
     range.
- `init` (pid 1) boots as kernel-class (priority −5) because it reaps orphans
  and restarts the shell — it is xv6's supervisor. Forked children revert to the
  user range (10) so kernel class does not propagate.

### 4.3 System calls

New syscalls added and registered in `kernel/syscall.{c,h}` + `user/usys.pl`:
`setpriority`, `getpriority`, `jail`, `agent_recv`, `set_deny`/`get_deny`,
`procinfo`, `set_cache`/`get_cache`, and `dispatch`. Each follows xv6's standard
argument-fetch + dispatch path.

### 4.4 Protection & isolation — a chroot jail + configurable deny-list *(F7)*

**Concept:** filesystem confinement + capability reduction + a policy list (like
`chroot` + seccomp + an allow/deny policy).

- `struct proc` gains `int is_agent` and `struct inode *jail_root`
  (`kernel/proc.h`).
- New **`jail(path)`** syscall (`kernel/sysfile.c`) confines the caller to
  `path` and sets `is_agent = 1`. It is **irreversible** by design.
- **Path resolution** — `namex()` in `kernel/fs.c` maps an agent's `/` to its
  `jail_root` and refuses `..` traversal above the jail root.
- **Dangerous-syscall block** — `kernel/syscall.c` returns −1 for `exec`,
  `kill`, and `mknod` when the caller `is_agent`.
- **Configurable deny-list** — no longer hardcoded. A spinlock-guarded kernel
  RAM list (default `{KILL, EXEC}`) gates wire commands; `set_deny`/`get_deny`
  syscalls plus the `denyctl` shell tool manage it (list/add/rm/reset/save/load),
  persisted to `/denylist.conf` and auto-loaded by `init` at boot. Crucially,
  `set_deny` **refuses `is_agent` processes** — only a human can change the
  policy, so a jailed agent cannot weaken its own boundary (same philosophy as
  the priority escalation guard). The one kernel list governs both hard-boundary
  commands and `agentd` tools, so denying e.g. `WRITE` stops it before it ever
  reaches `agentd`; `agentd`'s `LIST` shows the effective policy.
- **Opt-in** — only a process that called `jail()` (and its children) is
  affected; ordinary shells and commands are untouched.

### 4.5 Synchronization *(new on main)*

**Concept:** mutual exclusion across interrupt/process contexts and concurrent
processes.

- **Spinlocks** guard the deny-list, the CFS global `min_vruntime`, and the
  cache table.
- **Sleeplocks** guard the command queue dequeue (`agentq_get`) and, via the
  file system, every jailed file operation. `write_race` demonstrates this
  directly: four children issue `WRITE` to the same file at nearly the same
  tick, but `ilock()` serializes them, so their completion ticks step up
  like a staircase.
- **Log transactions (`begin_op`/`end_op`)** wrap the cache's `/cache.bin` disk
  overlay — which is precisely *why* command dispatch had to be split into two
  stages (see §6.1): a transaction can sleep, and sleeping is illegal in the
  console interrupt handler.

### 4.6 Concurrency / IPC — the command channel & multi-agent *(Tool Manager)*

**Concept:** producer/consumer queue across interrupt and process context, plus
concurrent agents.

- `consoleintr()` runs in **interrupt context**, where fork, file I/O, and sleep
  are illegal. So `agent_dispatch()` (`kernel/agentcmd.c`) only **enqueues** into
  an intake ring buffer (stage 1); actual routing happens in process context via
  `agent_drain()` → `agent_dispatch_now()` (stage 2), called from `usertrap` and
  `consoleread`.
- The deny-listed commands are blocked in stage 2; survivors go into a **16-slot
  ring buffer** (`agentq`). `agentq_get()` is sleeplock-guarded and its **only**
  caller is `sys_agent_recv`, which serves **`is_agent` processes only** — so no
  non-jailed process can drain the agent queue.
- `agent_multi` spawns **four concurrent role-based agents** that the CFS
  scheduler interleaves, demonstrating fair multi-agent execution and per-role
  ACL enforcement over a shared kernel.

### 4.7 File system

The agent's `READ`/`WRITE`/`LS` tools use the normal xv6 VFS path but resolve
inside `/agentbox` because the worker is jailed. The cache's `/cache.bin` and the
deny-list's `/denylist.conf` both use the real file system for **persistence
across reboots**, exercising the storage layer.

---

## 5. How the LLM Is Integrated

### 5.1 The bridge (`agent.py`)

The host process connects to the QEMU serial port (TCP 4444) and runs an
autonomous **ReAct loop** on top of Solar:

```
user request
  → model reasons; emits ONE JSON object: a tool call OR a final answer
  → if a tool: encode to REQ|<CMD>|<arg>, send to xv6
  → capture the tool's real output from xv6
  → feed it back as an OBSERVATION
  → repeat (≤ 8 steps) until the model answers
```

- **Mode selection:** no `UPSTAGE_API_KEY` ⇒ `mock` (one-step rule-based
  stand-in); key + `openai` SDK ⇒ `solar` (full loop). The key is read from a
  `.env` next to the script (real env vars take precedence).
- **Response schema:** every step the model replies with exactly one JSON
  object — `{"thought","tool","args"}` to act, or `{"thought","answer"}` to
  finish. `extract_json()` tolerates code fences and stray prose.
- **Special inputs:** `:ask <prompt>` routes through the **kernel F9 cache path**
  (host answers a `LLM_REQ` only on a cache miss); `:role <name>` tags subsequent
  requests with a role for the multi-agent ACL.

### 5.2 Wire protocol & observation capture

- Tools are encoded to a **minimal text format**: `REQ|LS|`,
  `REQ|READ|<file>`, `REQ|WRITE|<file>:<text>`, `REQ|PRINT|<msg>`,
  `REQ|NICE|<pid>:<prio>`, `REQ|LIST|`, `REQ|SETPRIO|<FN>:<prio>`,
  `REQ|PS|`, `REQ|HELP|`, with an optional `agent:<role>|` prefix.
- **Marker-based capture:** right after the real command, the bridge sends
  `REQ|PRINT|__OBS<n>__`. Because `agentd` drains the queue in order, every line
  printed *before* the marker's echo is exactly that tool's output — so the
  bridge slices the observation precisely and hands clean text back to the model.

### 5.3 Conversation memory

`agent.py` keeps the full `system + dialogue` history in `self.messages`, so a
follow-up ("now summarise them") builds on earlier steps; history is trimmed to
the most recent 24 messages to bound context.

### 5.4 F9 — the response cache *(implemented on main)*

`kernel/cache.c` is a **16-slot RAM table with a `/cache.bin` disk overlay**:
when RAM is full, `set` evicts an LRU slot to disk (append); a disk hit is
promoted back to RAM. Keys are compressed with a 64-bit FNV-1a hash; everything
is a static array (no dynamic allocation). On top of exact matching, **MinHash +
Jaccard semantic matching** (threshold 0.7) catches paraphrases / reordering /
partial substitutions. The cache is reachable via `set_cache`/`get_cache`
syscalls and is exercised standalone by `cache_test` (13/13).

The cache is wired into the command path: the kernel handles the `ASK` meta
command — on a **hit** it forwards the cached answer straight to `agentd`
(skipping Solar entirely); on a **miss** it emits `LLM_REQ` to the host, and when
the host returns `LLM_RESP` the kernel does `cache_set` before forwarding. The
`dispatch` syscall lets user programs (`eval`, `agent_multi`, `write_race`) drive
the same path.

### 5.5 Where the LLM's authority stops

The LLM never executes anything directly. Its JSON becomes a `REQ|` line; the
kernel deny-list can reject it before it is queued; if queued, only the jailed
`agentd` (chroot + blocked `exec`/`kill`/`mknod`) runs it. The LLM's "actions"
are therefore bounded by independent kernel mechanisms.

*(KR: LLM은 무엇도 직접 실행하지 않는다 — JSON → REQ| 라인 → (거부목록) →
(jail+위험 syscall 차단) agentd. 다중 경계.)*

---

## 6. Key Implementation Details

### 6.1 Two-stage dispatch (why it exists)

The F9 cache can touch the disk, and disk I/O uses log transactions
(`begin_op`) that may **sleep**. Sleeping is illegal in the console interrupt
handler. So `agentcmd.c` splits dispatch in two: stage 1 (`agent_dispatch`, in
the ISR) only enqueues raw lines; stage 2 (`agent_dispatch_now`, run from
`agent_drain` in process context) strips the optional `agent:<role>|` prefix,
handles cache meta-commands (`ASK`/`LLM_RESP`/`CACHE_GET`/`CACHE_SET`), applies
the deny-list, and routes the rest to the agent queue. The F7 deny check lives on
this forward path so the boundary is preserved.

### 6.2 The `agentd` tool table (whitelist + per-function priority, F7/F8)

`user/agentd.c` holds the only tools the LLM can invoke. Before each tool runs,
`agentd` calls `setpriority(getpid(), table[i].priority)` — so **F8's
"per-function priority" actually reaches the scheduler**, and the LLM can retune
it via `SETPRIO <FN>:<prio>`.

| FN      | allowed | priority | note |
| ------- | ------- | -------- | ---- |
| PRINT   | 1       | 10       | |
| READ    | 1       | 8        | |
| WRITE   | 1       | 12       | |
| LS      | 1       | 8        | |
| NICE    | 1       | 5        | needs target pid — use `PS` first |
| LIST    | 1       | 0        | shows F8 priorities + effective deny-list |
| SETPRIO | 1       | 5        | |
| PS      | 1       | 8        | **AI self-observation** — proc snapshot (pid/state/prio/name) |
| HELP    | 1       | 0        | **AI self-observation** — usage catalogue |
| CHAT    | 1       | —        | prints a cache-sourced answer |

**AI self-observation.** `PS` uses a new `procinfo(buf,max)` syscall
(`kernel/procinfo.h`) to return a process-table snapshot, so the LLM can discover
a pid before calling `NICE`; `HELP` prints per-command usage so the LLM can check
how to call a tool at runtime. Data comes from the kernel, formatting happens in
`agentd` (same split as `get_deny`).

### 6.3 Defense in depth *(KR: 다중 방어)*

1. **Configurable deny-list at the kernel** — default `KILL`/`EXEC` blocked
   before reaching `agentd`; humans can extend it, agents cannot.
2. **Agent syscall layer** — an `is_agent` process is refused
   `exec`/`kill`/`mknod` regardless.
3. **chroot jail** — files outside `/agentbox` are not even *visible*; `..`
   escape is refused in `namex()`.

### 6.4 Evaluation harness

- **`priority_test` Test 3** spawns HIGH(1)/MED(10)/LOW(19) CPU burners and
  **programmatically verifies the finish order** (HIGH→MED→LOW) over a pipe;
  wrong order exits FAIL. Burn count is 30M so scheduling dominates startup noise.
- **`cfs_share`** races N children at different priorities for a fixed
  wall-clock window and prints each one's CPU **share %** (run with `CPUS=1`).
- **`eval`** provides four sub-benchmarks that each exercise OS concepts:
  `eval cache <N>` (round-1 miss vs round-2 hit rate), `eval acl <N>` (per-role
  ACL deny rate against a live child), `eval fair <I>` (prio 0 vs 20 completion
  time), `eval semantic <N>` (exact / paraphrase / unrelated matching).
- **`agentdemo`** checks jail read, `..`/outside-path denial, escalation denial,
  and `exec` blocking. **`cache_test`** validates RAM-hit / evict / disk-promote
  (13/13). **`write_race`** shows inode-sleeplock serialization.
- **`tools/regression.py`** builds, boots, and runs a multi-case regression
  suite (it caught 3 latent bugs before the Week-13 dry-run).

---

## 7. Limitations & Future Work

### 7.1 F6 — JSON parsing lives on the host (deliberate)

The original proposal said the kernel should deserialize the LLM's JSON. We
**parse JSON on the host (`agent.py`)** and feed the kernel only a validated
`REQ|<CMD>|<arg>` format. Rationale:

1. **Kernel safety** — a dynamic parser's memory bugs become kernel panics;
   Python's `json.loads` is battle-tested standard library.
2. **xv6 constraints** — JSON's numbers (float) and nested objects (heap) clash
   with xv6's no-float / no-dynamic-allocation rules.
3. **Layer separation** — LLM-response interpretation (agent "thinking") is kept
   separate from kernel command execution; future LLM-side format changes don't
   touch the kernel.
4. **Validation simplicity** — the kernel's check is one line (`REQ|` prefix +
   command whitelist + arg length), which composes cleanly with the deny-list /
   jail / blocked-syscall defenses.

This is recorded explicitly because it departs from the proposal's wording. A
kernel-side mini-parser (`kernel/json.c`) is noted as optional future work with
low priority (security/maintenance cost outweighs learning value).

### 7.2 Confirm-escape is prototyped but disabled

`kernel/confirm.c` implements a "confirm-escape": instead of hard-denying an
agent's `exec`/`kill`/`mknod`, it prints `CONFIRM_REQ|...` to the console so the
host (`agent.py`) can prompt the user for a one-time `y/N` allow (5 s timeout).
It is **currently disabled** (`syscall.c` keeps the unconditional block) because
of a suspected `kerneltrap` panic in that path. Re-enabling it after root-causing
the panic, and extending it from single-pending to a queue, is future work.

### 7.3 Out of scope

- **F10 — idle-time LoRA training:** infeasible in xv6 (RISC-V, no float, tiny
  memory/disk). At most a conceptual stub ("detect idle ticks → signal the host
  to train"). Out of scope.

### 7.4 Measurement caveats

- `cfs_share` / `eval fair` percentages are environment-dependent (host load,
  QEMU timing); use them as relative evidence of the weight effect, not absolute
  SLAs.
- With `CPUS ≥ number of runnable processes`, time-sharing largely disappears,
  so fairness measurements should use `CPUS=1`.
- The stage-2 buffer bounds very long `ASK` payloads (the console input buffer
  was raised 256 → 2048 to accommodate them).

---

## 8. References

- AIOS: LLM Agent Operating System (Kai Mei et al., 2024) — the three-component
  framing we mapped.
- xv6-riscv, MIT PDOS — base kernel.
- Linux `sched_prio_to_weight` table (ported verbatim for CFS weights).
- Upstage Solar API docs: <https://console.upstage.ai/docs>.

> For line-referenced detail of every change, see
> [Implementation.md](../Implementation.md); for status/scope tracking see
> [plan.md](../plan.md); for chronological history see
> [CHANGELOG.md](../CHANGELOG.md).
