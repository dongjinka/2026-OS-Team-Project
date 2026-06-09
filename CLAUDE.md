# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this project is

A 2026 Operating Systems term project ("OS for LLM"): the **xv6-riscv** teaching kernel is
extended to host, schedule, sandbox, and cache an LLM agent (Upstage Solar). The three AIOS
components are implemented **inside the kernel**, not as a userspace wrapper:

- **Agent Scheduler** → a CFS scheduler in `kernel/proc.c` (Linux `sched_prio_to_weight`
  weights, `vruntime`, `cfs_min_vruntime`, leftmost-vruntime pick).
- **Tool Manager** → a kernel command queue + jailed `agentd` worker speaking a `REQ|` wire
  protocol, with a configurable deny-list and an LLM-response cache.
- **LLM–Kernel Bridge** → `agent.py`, a host-side ReAct loop bridging the Solar API ↔ the
  QEMU serial port over TCP.

**The load-bearing design invariant:** a human's shell input runs unrestricted, but *every*
command the LLM issues passes three kernel gates — **deny-list → jail → confirm-escape** —
before reaching the CPU scheduler. That asymmetry is the security model, and it is enforced
by the code path itself, not by convention. When changing the agent command path, preserve
this: a human shell command must bypass these gates; an LLM command must not.

## Build & run

All kernel work happens under `xv6-riscv/`. Default toolchain prefix is auto-detected
(`riscv64-unknown-elf-` or `riscv64-linux-gnu-`); override with `make TOOLPREFIX=...`.

```bash
cd xv6-riscv
make qemu                 # boot to interactive shell (smp=3 by default)
make qemu CPUS=1          # single core — makes CFS time-sharing observable
make qemu-agent           # boot with serial on TCP 127.0.0.1:4444 for agent.py (forced smp=1)
make clean
```

**`make qemu-agent` is forced to single-core on purpose** — the F9 cache + confirm-escape
paths trip a known `scause=0xf` kernelvec trap-entry race at smp>1. Do not "fix" this by
re-enabling SMP on the agent target. `make qemu` (shell mode) boots multi-core fine.

Quit QEMU with `Ctrl-a x`. GDB debugging: `make qemu-gdb` (port is `id -u % 5000 + 25000`).

After editing a user program or syscall, it must be wired into `xv6-riscv/Makefile`
`UPROGS` and (for syscalls) the usual xv6 chain (`syscall.h`, `syscall.c`, `user.h`,
`usys.pl`). New user programs are referenced as `$U/_name`.

## The agent (host side)

```bash
python3 agent.py          # from repo root, with make qemu-agent already running
```

Configured entirely by env (`.env` is auto-loaded; copy from `.env.example`, gitignored):

- `UPSTAGE_API_KEY` — set → **Solar mode**; unset/empty → **mock mode** (a rule-based
  stand-in so the kernel path runs with no network/key). Tests rely on mock mode.
- `UPSTAGE_MODEL` (default `solar-pro2`), `XV6_HOST` (`127.0.0.1`), `XV6_PORT` (`4444`).

REPL inputs: a natural-language request runs the ReAct loop; `:ask <prompt>` exercises the
kernel F9 cache path (skips Solar on a hit); `:role <name>` tags following requests.

## Testing

In-kernel tests are user programs run at the xv6 `$` prompt: `priority_test` (F1/F3/F4),
`agentdemo` (jail + privilege guards), `cache_test` (F9, 13/13), `cfs_share`
(CPU-share by priority — races 3 fixed priorities, run under `CPUS=1`), `cfs_bench`
(priority→CPU-share sweep that feeds `tools/bench_report.py`),
`eval cache|acl|fair|semantic N`, `agent_multi`, `write_race`, `denyctl list`.

Automated regression harnesses live in `tools/`. Each boots its **own** isolated QEMU on a
**dedicated port** with a per-run `fs.img` copy, so they run alongside a live 4444 session:

```bash
./tools/regression.sh              # 9-test unit regression (~4 min); --fast, --ci, --only=<test>
python3 tools/ralph_battery.py     # 26 shell/syscall scenarios   (port 5555)
python3 tools/ralph_natlang.py     # 39 natural-language scenarios, mock (port 6666)
python3 tools/sec_audit.py         # red-team reproducers          (port 5557)
python3 tools/sec_wire.py          # host-only unit test of agent.py wire_for() (no qemu)
python3 tools/bench_report.py      # CPU-share + cache hit-rate Markdown report (port 5558)
```

See [`docs/UNIT_IO_MATRIX.md`](docs/UNIT_IO_MATRIX.md) for the full
unit → input → expected-output → harness table across the agent command path.

Build the kernel + `fs.img` first (`cd xv6-riscv && make`) — the harnesses do not build.
Single-test example: `./tools/regression.sh --only=cache_test`.

The red-team scripts (`sec_audit`, `sec_wire`) and their xv6 reproducers (`secnice.c`,
`secconfirm.c`) report `VULNERABLE`/`SAFE` against the *current* kernel; a `VULNERABLE`
verdict is the expected outcome on an unpatched build, not a harness failure (they exit
non-zero only on harness/kernel errors).

## How one natural-language turn flows (the critical path)

Spans `agent.py` → `kernel/agentcmd.c` → `kernel/cache.c` / `user/agentd.c`, so it needs
multiple files to understand:

1. `agent.py` sends the prompt to Solar, parses JSON into a `{tool, args}` step, encodes it
   as one `REQ|CMD|arg` line, and writes it to the serial port.
2. `consoleintr` (`kernel/console.c`) spots the `REQ|` line and, **in interrupt context**,
   only *enqueues* it via `agent_dispatch` (keeps the ISR short), then wakes the console
   reader so the **next trap drains the queue in process context**. It also suppresses
   echoing the payload to avoid a wire byte-race.
3. `agent_drain` (`kernel/agentcmd.c`, process context — cache handlers may `begin_op()` /
   sleep, illegal in the ISR) strips the role tag and routes:
   - **`ASK`** → F9 cache (`kernel/cache.c`): exact or MinHash/Jaccard-paraphrase **hit**
     returns the answer with **no Solar call**; a **miss** does host `LLM_REQ → LLM_RESP`
     then `cache_set` (RAM + `/cache.bin` disk overlay via log transactions).
   - **tool command** → configurable **deny-list** (default `{ KILL, EXEC }`) → `agentq`.
4. Only a jailed `agentd` (`is_agent = 1`) reads the queue via `sys_agent_recv` and runs
   every tool inside its `/agentbox` chroot. Before each tool it calls
   `setpriority(self, tool.priority)` (F8 per-tool tuning).
5. A tool needing **`exec`/`kill`/`mknod`** hits **confirm-escape** (`kernel/confirm.c`):
   it sleeps on `&confirm_wait_chan` until the host answers `y/N`, with a 15 s
   `clockintr`-driven timeout defaulting to **deny**. The `CONFIRM_RES` reply is processed
   **inline in the interrupt**, bypassing the queue — necessary because the agent is asleep
   and no user trap would otherwise drain it.
6. Everything (human and agent) is scheduled by CFS on its `vruntime`.

## Kernel constraints (xv6, RISC-V)

- **No floating point and no dynamic allocation** in any kernel-side addition — both are
  restricted in xv6. CFS weights are integer tables; the cache uses fixed-size buffers.
- The host↔kernel wire is a strict `REQ|<CMD>|<arg>` line protocol; newlines in field
  values must be escaped (`agent.py` `_wire_escape`) or they inject a second command line.
  JSON deserialization (F6) is deliberately done host-side in `agent.py`; the kernel only
  accepts the validated minimal `REQ|` format.

## Key files

- `kernel/proc.{c,h}` — CFS weights, `vruntime`, `is_agent` / `jail_root`, allocproc.
- `kernel/agentcmd.c` — 2-stage dispatch queue, deny-list, ASK/cache meta-commands.
- `kernel/cache.c` — F9 response cache (RAM + `/cache.bin` + MinHash/Jaccard).
- `kernel/confirm.c` — confirm-escape v2 (sleep/wakeup, clockintr timeout).
- `kernel/sysproc.c` — priority guard (user/kernel class), `agent_recv`/`cache`/`dispatch` syscalls.
- `kernel/fs.c` / `sysfile.c` — chroot jail in `namex()`, `jail()` syscall.
- `user/agentd.c` — jailed worker: tool whitelist, per-function priority, `spawn`.
- `agent.py` — host ReAct bridge; `:ask` = F9 cache path; mock vs Solar mode.

Deeper, code-referenced detail lives in `Implementation.md`, `docs/Technical_Report.md`,
and the security doc `docs/SECURITY.md` (EN overview + full KR finding register).

## Conventions

- Commits/pushes use git identity **server3342 / server3342@gmail.com**.
- README is English-primary (`README.md`) with a Korean companion (`README.ko.md`); several
  in-repo design docs (`Implementation.md`, `Project_Guide.md`, harness docstrings) are in
  Korean. Match the language of the file you are editing.
