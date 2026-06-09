# OS for LLM — an agent runtime on xv6-riscv

**2026 Operating Systems · team term project · Direction A (OS for LLM).**
Natural language is the keyboard; the kernel is the trust boundary.

> 한국어 README: [README.ko.md](README.ko.md)

We extend the **xv6-riscv** teaching kernel so it can **host, schedule, sandbox, and
cache an LLM agent** (Upstage Solar). The three components of the *AIOS: LLM Agent
Operating System* paper are implemented **inside a real kernel** rather than as a
userspace wrapper:

| AIOS component | Our implementation |
| --- | --- |
| **Agent Scheduler** | a **CFS scheduler** in the kernel using Linux's `sched_prio_to_weight` weights (vruntime, `cfs_min_vruntime`) |
| **Tool Manager** | a **kernel command queue + jailed `agentd` worker** speaking a `REQ\|` wire protocol, with a configurable deny-list and an LLM-response cache |
| **LLM–Kernel Bridge** | **`agent.py`** — a host-side ReAct loop bridging the Solar API ↔ the QEMU serial port |

**The design invariant:** a human's shell input runs unrestricted, but *every command
the LLM issues executes only inside a chroot jail with dangerous syscalls gated.* The
privilege boundary between human and LLM is enforced by the **code path itself**, not by
convention — which is what makes the OS concepts below load-bearing rather than
decorative.

---

## 1. Architecture

A command the LLM produces reaches the CPU scheduler only after passing three kernel
gates — **deny-list → jail → confirm-escape**. A human's shell command bypasses all of
them. That asymmetry *is* the security model, and it is enforced by the code path below,
not by convention.

```
   Human · shell ───────────────────────────────────────────────────────►  CFS   (unrestricted)

   User · natural language
        │
        ▼
   agent.py  ◄──────HTTPS / JSON──────►  Upstage Solar Pro · api.upstage.ai/v1
     host bridge — ReAct loop + conversation memory · parse Solar JSON → {tool, args}
                   · encode one  REQ|CMD|arg  line per step
        │
        │  REQ| line  ·  TCP 4444  ·  QEMU serial
        ▼
   ── xv6 kernel · the trust boundary ─────────────────────────────────────────
   consoleintr        detect the REQ| line; skip echoing its payload (no wire byte-race)
        │
        ▼
   agent_dispatch     interrupt context — enqueue the line only (keeps the ISR short)
        │
        ▼
   agent_drain        process context — strip the role tag, then route:
        ├─ ASK  →  F9 cache (cache.c)
        │            hit  → answer handed straight to agentd          (Solar NOT called)
        │            miss → host LLM_REQ → Solar → LLM_RESP → cache_set, then answer
        └─ cmd  →  deny-list  (default { KILL, EXEC }, configurable)  →  agentq
                        │
                        ▼   sys_agent_recv   (only is_agent processes may read the queue)
   agentd · jailed worker     chroot = /agentbox,  is_agent = 1
        · whitelisted tools:  PRINT · CHAT · READ · WRITE · LS · PS · NICE · SETPRIO · LIST · HELP · SPAWN
        · before each tool:   setpriority(self, tool.priority)          (F8 per-tool tuning)
        · exec / kill / mknod  →  confirm-escape v2  →  host y/N  (15 s timeout → deny)
        │
        ▼
   CFS scheduler (proc.c)      cfs_weight[41] · vruntime · cfs_min_vruntime
                               fork inheritance · wakeup bonus · leftmost-vruntime pick
```

**The lifecycle of one natural-language turn**

1. `agent.py` sends the prompt to Solar, receives JSON, and parses it into a `{tool, args}` step.
2. It encodes the step as a single `REQ|CMD|arg` line and writes it to the QEMU serial port (TCP 4444).
3. `consoleintr` spots the `REQ|` line and, in **interrupt context**, only *enqueues* it — `agent_dispatch` keeps the ISR short; it also wakes the console reader so the **next trap drains the queue** in process context.
4. In **process context**, `agent_drain` strips the role tag and routes. An **`ASK`** is served from the F9 cache: a hit — exact, or a paraphrase matched by MinHash/Jaccard — returns the answer with **no Solar call**; a miss calls Solar once (`LLM_REQ → LLM_RESP`) and stores it (`cache_set`). A **tool command** is checked against the configurable **deny-list** (default `{ KILL, EXEC }`) and queued.
5. Only a jailed **`agentd`** (`is_agent = 1`) may read the queue via `sys_agent_recv`, and it runs every tool inside its `/agentbox` **chroot jail**.
6. Before each tool `agentd` calls `setpriority(self, tool.priority)` (F8). A tool needing **`exec`/`kill`/`mknod`** is suspended on a **confirm-escape**: it sleeps on a dedicated channel until the host answers `y/N` (a 15 s `clockintr`-driven timeout defaults to deny). That reply (`CONFIRM_RES`) is processed **inline in the interrupt**, bypassing the queue — necessary because the agent is asleep and no user trap would otherwise drain it.
7. Every process — human and agent alike — is scheduled by **CFS** on its `vruntime`, so a process's priority maps to a measurable share of the CPU.

The two-stage queue (steps 3–4) keeps interrupts short; the cache (4) means a repeated
question never leaves the machine; the jail plus confirm-escape (5–6) is the privilege
boundary; CFS (7) is where the scheduling concept becomes observable and measurable.

---

## 2. OS concepts in play

This is a real operating-systems project: the LLM only makes the concepts below
*visible*. Each is something we designed and implemented in the kernel.

| OS concept | Where it lives |
| --- | --- |
| **Scheduling (CFS)** | `kernel/proc.c` (weights, vruntime, leftmost pick), `kernel/trap.c` (per-tick accrual) |
| **Processes & priority** | `setpriority` / `getpriority`; user vs. kernel class; two-way escalation guard (`init` = −5) |
| **System calls** | new `jail`, `agent_recv`, `set_deny`/`get_deny`, `procinfo`, `set_cache`/`get_cache`, `dispatch` |
| **Protection / sandbox** | chroot jail in `namex()`; agent `exec`/`kill`/`mknod` go through **confirm-escape v2** (sleep on `&confirm_wait_chan`, woken by `clockintr` or host `CONFIRM_RES`); configurable deny-list (humans only) |
| **Synchronization** | inode sleeplock (`write_race`), cache + deny-list spinlocks, log transactions (`begin_op`) for the cache disk overlay |
| **Concurrency / IPC** | `agentcmd.c` 2-stage queue (ISR enqueue → process-context drain); `agent_multi` runs 4 concurrent agents |
| **File system** | jailed file tools in `/agentbox`; `/cache.bin` cache overlay; `/denylist.conf` persistence |

Full rationale and block diagram: [docs/Technical_Report.md](docs/Technical_Report.md).
No floating point and no dynamic allocation are used in any kernel-side addition — both
are restricted in xv6.

---

## 3. Quick start

### 3.1 Dependencies

```bash
# Debian / Ubuntu / WSL2
sudo apt install qemu-system-misc gcc-riscv64-linux-gnu python3-pip make
pip3 install openai

# macOS
brew install qemu riscv-software-src/riscv/riscv-tools
pip3 install openai
```

`openai` is only needed for live LLM mode; without it the bridge falls back to a
rule-based **mock** so the kernel path can still be exercised.

### 3.2 Solar API key — never commit it

The Upstage Solar key is supplied by the instructor (per team). `.env` is gitignored;
`.env.example` is the committed template.

```bash
cp .env.example .env
# then edit .env:
#   UPSTAGE_API_KEY=up_xxxxxxxxxxxxxxxxxxxx
#   UPSTAGE_MODEL=solar-pro2
```

API docs: <https://console.upstage.ai/docs>. (The assignment §4 names *Solar Pro 3*; we
default to `solar-pro2` for availability — change `UPSTAGE_MODEL` to switch.)

---

## 4. How to run

Two modes: a plain **shell** (run kernel tests directly) and **agent mode** (the kernel
listens on a TCP serial port for the Python bridge).

### 4.1 Shell mode — run the OS tests

```bash
cd xv6-riscv
make clean
make qemu                  # boots xv6 to an interactive shell

# at the xv6 '$' prompt:
$ priority_test            # F1/F3/F4 — priority syscalls + scheduler
$ agentdemo                # F2/F7 — jail isolation + privilege guards + blocked syscalls
$ cache_test               # F9 — cache RAM-hit / evict / disk-promote (13/13)
$ eval cache 50            # F9 — round-1 miss vs round-2 hit rate
$ eval acl 1               # F7 — per-role ACL deny rate
$ eval fair 30000000       # F3/F4 — prio=0 vs prio=19 completion time
$ agent_multi              # concurrency — 4 role-based agents interleaved by CFS
$ write_race               # synchronization — inode sleeplock serializes writers
$ denyctl list             # F7 — show the effective kernel deny-list
```

For the fairness benchmark, a single core makes time-sharing clearest:

```bash
make clean && make qemu CPUS=1
$ cfs_bench                      # per-priority CPU-share sweep (numbers in docs/BENCHMARKS.md)
$ cfs_share                      # the 3-priority fairness race {1,10,19}
```

Quit QEMU with `Ctrl-a x`.

### 4.2 Agent mode — talk to the LLM

Open two terminals.

```bash
# Terminal 1 — boot xv6 with its serial port on TCP 127.0.0.1:4444 (smp=1)
cd xv6-riscv && make qemu-agent
```

```bash
# Terminal 2 — start the host bridge (from the repo root)
python3 agent.py
```

The bridge prints `mode: solar (solar-pro2)` (key present) or `mode: mock` and drops into
a REPL. Ask in plain language — it plans, runs sandbox tools, observes their output, and
remembers the conversation.

| Input | Behavior |
| --- | --- |
| `<natural-language request>` | ReAct loop — call tools / observe, then answer |
| `:ask <prompt>` | kernel F9 cache path — skips the Solar call on a hit |
| `:role <name>` | tag following requests with a role |

On boot, `init` automatically launches `agentd`, which jails itself into `/agentbox` and
applies the persisted deny-list. Every tool the LLM calls executes inside that jail.

---

## 5. Demo

> **Full end-to-end demo video (Google Drive)** — <https://drive.google.com/file/d/14ruIXM-Lg6nP3BLTjsIWOrV68E5yUNgK/view?usp=drive_link>
> One live `solar-pro2` agent-mode session covering the natural-language → kernel
> path: cache hit, `spawn` confirm-escape allow/deny, jailed `NICE` denied, file
> WRITE/READ with conversation memory, and `PS` with `[K]`/`[A]` class markers.

Every transcript below is **real run output** (`solar-pro2`, smp=1).

### 5.1 Repeated question → kernel cache hit

Ask the same question twice; the second answer comes from the kernel F9 cache and Solar is
never called.

```text
you ▸ In one short sentence, what is a system call?
   💭 The user is asking for a concise definition of a system call.
╭─ answer ─────────────────────────────────────────────────────────────────────╮
│ A system call is an interface between a program and the operating system for │
│ requesting services.                                                         │
╰──────────────────────────────────────────────────────────────────────────────╯

you ▸ In one short sentence, what is a system call?          ← same request again
[cache HIT] answer reused from kernel F9 cache (Solar not called)
╭─ answer ─────────────────────────────────────────────────────────────────────╮
│ A system call is an interface between a program and the operating system for │
│ requesting services.                                                         │
╰──────────────────────────────────────────────────────────────────────────────╯
```

![F9 cache hit on a repeated arithmetic question (2nd call: `[cache HIT] Solar not called`)](docs/assets/cache-hit.png)

### 5.2 Sandbox — confirm-escape gate and jail denials

Spawning a process asks the host `y/N`; reading outside the jail or renicing another
process is refused by the kernel.

```text
you ▸ spawn a process that prints hello
   ▶ step 1 · ls                                    (agentd confirms /echo exists in the jail)
   ▶ step 2 · spawn  bin=/echo  argv=['echo', 'hello']
[jail] pid=5 requests dangerous syscall 'exec' — allow within 15s? (y/N)  y    ← host approves (the gate prompt matches the question's language)
   xv6 ┃ echo hello
   xv6 ┃ [agentd] SPAWN /echo done (status=0)

you ▸ read the contents of /etc/passwd
   ▶ step 1 · read  file=/etc/passwd
   xv6 ┃ [agentd] READ: '/etc/passwd' not reachable inside jail      ← denied outside the jail

you ▸ lower the priority of process 1 to 19
   ▶ step 1 · ps
   ▶ step 2 · nice  pid=1  priority=19
   xv6 ┃ [agentd] NICE: denied (pid=1 prio=19)                       ← a jailed agent cannot renice others
```

| `spawn` allow (host answers `y`) | `spawn` deny (host answers `N`) |
| --- | --- |
| ![spawn echo allowed — `SPAWN /echo done (status=0)`](docs/assets/confirm-escape-allow.png) | ![spawn echo denied — `denied (confirm-escape)`](docs/assets/confirm-escape-deny.png) |

![jailed `NICE` against pid 1 refused — `init` is kernel-class and only a kernel-class caller can demote it](docs/assets/nice-init-denied.png)

### 5.3 Natural-language → kernel tool — real session captures

The four captures below sit alongside the three already inlined in §5.2 (`spawn`
allow/deny + `NICE` on `init`) and the one in §5.1 (cache hit) — eight in total,
all real `solar-pro2` runs through `agent.py`, captured 2026-06-08. The asciinema
recording recipe and the still-missing GIF / `priority_test` / `cfs_share`
candidates are listed in [`docs/assets/README.md`](docs/assets/README.md).

| Prompt the user typed | What the kernel did | Capture |
| --- | --- | --- |
| *"List the files in /agentbox"* | `agentd` runs `LS`; the listing shows the `echo` / `cat` / `ls` / `grep` / `sh` / … that `populate_jail()` hard-linked at boot so `SPAWN` can actually `exec` something | [![jail ls](docs/assets/jail-populate.png)](docs/assets/jail-populate.png) |
| *"Show me the currently running processes"* | `agentd` runs `PS` (`procinfo` syscall); output marks `init` as `[K]` (kernel class, prio −5) and `agentd` as `[A]` (agent class) — the LLM can now discover pids before calling `NICE` | [![ps](docs/assets/agent-ps.png)](docs/assets/agent-ps.png) |
| *"Write 'TODO 1\nTODO 2' into a file called /plan.txt"* | `WRITE` reaches the jailed FS; the `\n` survives the wire because `agent.py:_wire_escape` swaps it for `\\n` and `agentd:unescape_inplace` restores it | [![write](docs/assets/agent-write.png)](docs/assets/agent-write.png) |
| *"Re-read the file you just created and summarize it"* | follow-up turn — `agent.py` keeps the dialogue in `self.messages`, so the model knows *which* file; `READ` returns the two lines and the model summarises | [![read + memory](docs/assets/agent-read-memory.png)](docs/assets/agent-read-memory.png) |

---

## 6. Verification at a glance

| Check | Result |
| --- | --- |
| Kernel boot (smp=1 and smp=3) | no panic |
| `priority_test` Test 1 / 2 / 3 | PASSED |
| `agentdemo` 5 checks (jail read/write, `..` blocked, negative-priority denied, exec confirm allow/deny) | all pass |
| `cache_test` | 13/13 (RAM hit / evict / disk promote) |
| `denyctl add WRITE` → `REQ\|WRITE\|` | blocked in kernel (never reaches `agentd`) |
| `denyctl save` → reboot → `list` | `/denylist.conf` auto-loaded, entries survive |
| Jail escape via `..` / outside paths | denied |
| Agent `exec` / `kill` / `mknod` | host `y/N` gate (timeout = default deny) |
| User → negative-priority escalation / kernel-class demotion | denied |
| `REQ\|SPAWN\|/echo\|...` | fork+exec inside jail, confirm-escape on exec |
| F9 `:ask` repeated | MISS then HIT (Solar skipped on hit) |
| **`tools/ralph_battery.py`** — 26 shell/syscall `record()` checks (port 5555) | 26/26 PASS |
| **`tools/ralph_natlang.py`** — 39 natural-language `record()` checks (mock, port 6666) | 39/39 PASS |
| Live Solar ReAct multi-step + memory | ls → read (×N) → summary; the follow-up uses prior context |

Cumulative regression **65/65 GREEN** (the 65 counts `record()` assertions, grouped into
≈16 + ≈17 scenario groups; a few gate on "no panic" rather than behavioral correctness). Both harnesses use
isolated ports + per-run `fs.img` copies, so they run alongside a live 4444 session.
Security findings & fixes: [docs/SECURITY.md](docs/SECURITY.md); quantitative eval numbers: [docs/BENCHMARKS.md](docs/BENCHMARKS.md).

---

## 7. Feature status

| # | Feature | Status |
| --- | --- | --- |
| F1 | `setpriority` / `getpriority` syscalls | done |
| F2 | user/kernel priority classes (negative = kernel; `init` = −5); two-way escalation guard | done |
| F3 | CFS scheduler — Linux weight table + vruntime + array scan | done |
| F4 | CFS details — fork inheritance, wakeup bonus, global `cfs_min_vruntime` | done |
| F5 | QEMU ↔ Upstage Solar Python bridge (`.env` auto-load) | done |
| F6 | LLM-response JSON deserialization (host-side parsing — rationale in report) | done |
| F7 | Sandboxing — chroot jail + confirm-escape (`y/N`) + tool whitelist + configurable deny-list | done (v2 sleep/wakeup) |
| F8 | per-tool priority customization (`SETPRIO` / `LIST`) | done |
| Bonus | ReAct autonomous loop + conversation memory + `spawn` tool (NL → process → confirm-escape) | done |
| F9 | LLM-response cache — RAM + `/cache.bin` disk overlay + MinHash/Jaccard + kernel `ASK` orchestration | done |
| F10 | idle-time LoRA training | out of scope (Future Work) |

---

## 8. Repository layout

```
.
├── README.md / README.ko.md   entry (EN / KR)
├── agent.py                   host-side ReAct bridge; :ask = F9 cache path
├── .env.example               copy to .env, add the Solar key (gitignored)
├── docs/                      reports, security/eval, benchmarks, media  (→ docs/README.md)
├── tools/                     regression harnesses · red-team · bench scripts
└── xv6-riscv/
    ├── kernel/
    │   ├── proc.{c,h}         CFS weights, vruntime, is_agent / jail_root
    │   ├── agentcmd.c         2-stage dispatch, deny-list, ASK/cache meta-commands
    │   ├── cache.c            F9 response cache (RAM + /cache.bin + MinHash/Jaccard)
    │   ├── confirm.c          confirm-escape v2 (sleep/wakeup, clockintr timeout)
    │   ├── fs.c / sysfile.c   chroot jail (namex), jail() syscall
    │   └── sysproc.c          priority guard; agent_recv / cache / dispatch syscalls
    └── user/
        ├── agentd.c           jailed agent worker — tool table + per-fn priority + spawn
        ├── priority_test.c · cfs_bench.c · cfs_share.c · cache_test.c · eval.c   tests / benchmarks
        └── agent_multi.c · write_race.c                            concurrency / sync demos
```

Deep, code-referenced detail: [Implementation.md](Implementation.md).

---

## 9. Limitations & future work

- **F6 (JSON deserialization)** runs on the host (`agent.py`); the kernel only accepts a
  validated minimal `REQ|<CMD>|<arg>` format. Rationale (kernel safety, no float/heap in
  xv6, layer separation) is in the report — a deliberate departure from the proposal wording.
- **Security follow-up** — red-team findings #1·#3·#4 are fixed; **#2** (cache `/cache.bin`
  resolving through the jail) and **#5** (deny-list default not covering `SPAWN`) are open.
  See [docs/SECURITY.md](docs/SECURITY.md) (EN overview + full finding register #1–#9).
- **SMP** — `make qemu-agent` runs single-core to avoid a known kernelvec trap-entry race
  (`scause=0xf`); shell mode (`make qemu`) boots with smp>1.
- **Solar tokenizer boundary** — a Korean particle abutting a number (e.g. `"22 + 45는?"`)
  can drop a token; the wire path is byte-perfect, so the workaround is to insert a space or drop the quotes.
- **F10 (idle-time LoRA training)** is infeasible on xv6 (RISC-V, no float, tiny memory/disk).

---

## 10. Deliverables & documents

| # | Deliverable | Location |
| --- | --- | --- |
| 1 | **Application** + source + how-to-run | this README + [README.ko.md](README.ko.md) + repository |
| 2 | **Technical Report** | [docs/Technical_Report.md](docs/Technical_Report.md) |
| 3 | **Development Process** | [docs/Development_Process.md](docs/Development_Process.md) |
| 4 | **Presentation Slides** (English) | `slides/` *(to be added)* |

Full documentation map: [docs/README.md](docs/README.md) — routes to the reports, the
security audit, the benchmarks, and the Korean reference docs (Implementation · Project_Guide ·
CHANGELOG).

---

## 11. Team

| Member | Focus (from git history) |
| --- | --- |
| Se-Joong Kim | xv6 integration, scheduler foundation, jail/sandbox rewrite, F9 cache, regression harness |
| SeungBeom Kim | core features: CFS, sandboxing, `agentd`, `agent.py` loop, security guards, PS/HELP |
| June Kong | evaluation automation (`cfs_share`, Test 3), documentation, design decisions |
| Dongjin Ka | repository / review |

> Roles are inferred from git history — correct if inaccurate.

---

## 12. License & credits

- xv6-riscv is MIT-licensed (`xv6-riscv/LICENSE`); our changes are released under the same license.
- Design motif: Kai Mei et al., *AIOS: LLM Agent Operating System*, 2024.
