# Security & Evaluation

Report-ready sections (English) covering (1) a security audit of the project's
custom code with reproduced vulnerabilities and fixes, and (2) quantitative
evaluation of the CFS scheduler and the LLM response cache.

Companion artifacts: [`SECURITY_AUDIT.md`](SECURITY_AUDIT.md) (full finding
list), [`docs/BENCHMARKS.md`](BENCHMARKS.md) (raw numbers), and the reproducers
under `tools/sec_audit.py`, `tools/sec_wire.py`, `tools/bench_report.py`.

---

## 1. Security

### 1.1 Threat model

The project's central security claim is that **the kernel is the trust boundary**:
a human at the shell is unrestricted, while every action the LLM takes flows
through a sandbox — a `chroot` jail, a syscall deny-list, and a *confirm-escape*
gate that asks a human `y/N` before a jailed agent may run `exec`/`kill`/`mknod`.
We therefore audited whether a **malicious or hijacked LLM**, acting as the
jailed `agentd` runtime, can break out of or weaken that boundary.

### 1.2 Method

We ran a multi-dimension audit (kernel memory safety, concurrency, sandbox
escape, input parsing, resource exhaustion, error paths, host bridge) and
adversarially verified each finding against the source. Of 20 raw findings, 16
were confirmed; 4 were rejected or downgraded (documented in `SECURITY_AUDIT.md`,
including one proposed fix that would have introduced an AB-BA deadlock with
`clockintr` and was therefore *not* applied). Only the team's custom code is in
scope; stock xv6 is excluded.

### 1.3 Reproduced vulnerabilities

Each is reproduced deterministically by a test that only calls existing
syscalls (no kernel modification), classifying the build as `VULNERABLE` (hole
present) or `SAFE` (fixed). **#1, #3, and #4 are now patched** on the current
`Dongjin` build (the repo owner's working branch on the new `main`; #1/#3 via team
review PR #13/#14 — also on `main`; #4's read/write-filename part via our `agent.py`
fix), so the reproducers below report `SAFE` on it.

| # | Severity | Vulnerability | Reproducer |
|---|---|---|---|
| 1 | High | A jailed agent self-approves its own confirm-escape gate | `tools/sec_audit.py` (`secconfirm`) |
| 3 | Medium | A jailed agent renices arbitrary processes (scheduling DoS) | `tools/sec_audit.py` (`secnice`) |
| 4 | Medium | LLM output injects a forged command line over the wire | `tools/sec_wire.py` |

**#1 — Confirm-escape self-approval.** `sys_dispatch()` (`kernel/sysproc.c`) has
no `is_agent` guard, and `agent_dispatch_now()` routes the `CONFIRM_RES` reply
*before* the deny-list (`kernel/agentcmd.c`). Because `confirm_resolve()` matches
only the pending PID, a jailed process can answer its own prompt:
`dispatch("REQ|CONFIRM_RES|<pid>|y")`. The reproducer jails a parent, forks a
child that trips the `mknod` gate, and has the parent self-approve it — with no
host involved the child's `mknod` returns 0.

**#3 — Privilege escalation via NICE.** `sys_setpriority()` guards the
kernel-class boundary but never restricts a jailed agent (user-class) to
reniceing *itself*. `NICE` is whitelisted and not confirm-gated, so a hijacked
LLM can emit `REQ|NICE|<pid>:19` to starve the human shell or `:0` to favour
itself. Measured: a jailed agent moved a peer process from priority 10 to 19.

**#4 — Wire command injection.** `agent.py:wire_for()` escapes newlines for
`chat`/`write`-text/`print` but not for the `read` filename, the `write`
filename, or `spawn` bin/argv. Since the kernel frames the serial stream one
`REQ|...` line per `\n`, a newline in those fields (from LLM output or copied
user text) injects a second, attacker-chosen command. The sibling cache path
already strips newlines, confirming the omission is an oversight.

### 1.4 Fixes (low-risk, surgical)

Each fix is small and preserves the 65/65 regression suite; each flips its
reproducer from `VULNERABLE` to `SAFE`.

- **#3** — `sys_setpriority()` rejects `myproc()->is_agent && pid != self`, so a
  jailed agent retunes only itself (its F8 self-tuning still works). On `main`
  via team review PR #13/#14.
- **#1** — `sys_dispatch()` rejects `is_agent` callers: the dispatch channel is
  the host/operator command path and host confirm replies arrive over the
  console, not via `dispatch`. On `main` via PR #13/#14.
- **#4** — `wire_for()` wraps the `read`/`write` filename and `spawn` bin/argv in
  `_wire_escape` (`spawn` on `main`; the `read`/`write` filenames in our `agent.py`).

Two further fixes from cache-reliability work (ours):

- **Cache RESP atomicity** — `handle_cache_get()` emits `RESP|HIT|...` in a single
  `printf`, so the value no longer interleaves with other CPUs' console output at
  `smp>1` (which had silently turned cache hits into misses).
- **smp=1 agent mode** — `make qemu-agent` runs single-core to dodge a known,
  intermittent kernelvec trap-entry race (`scause=0xf`) that panics the kernel —
  and thus the cache — only at `smp>1`.

Still open (tracked in [`SECURITY_AUDIT.md`](SECURITY_AUDIT.md)): **#2**
(`/cache.bin` resolving through the jail) and **#5** (the default deny-list not
covering `SPAWN`), plus a `CONFIRM_REQ` nonce as defence-in-depth. **#6** (a
benign lock-free read in `confirm_tick`) is intentionally left as-is — the
unconditional-wakeup hardening we tried aggravated the kernelvec race and was
reverted.

---

## 2. Evaluation

### 2.1 CFS scheduler — priority to CPU share

Six children (`user/cfs_bench.c`) pinned to a spread of priorities race for one
fixed wall-clock window under `-smp 1`; each child's loop count is the CPU it
won. "Expected share" is the Linux weight ratio (`weight / Σweight`) from the
ported `cfs_weight[]` table in `kernel/proc.c`.

| priority | weight | loop count | measured share | expected share |
|---:|---:|---:|---:|---:|
| 0  | 1024 | 352,688,000 | 56.9% | 59.1% |
| 4  |  423 | 148,146,000 | 23.9% | 24.4% |
| 8  |  172 |  60,202,000 |  9.7% |  9.9% |
| 12 |   70 |  32,111,000 |  5.2% |  4.0% |
| 16 |   29 |  15,648,000 |  2.5% |  1.7% |
| 19 |   15 |  11,581,000 |  1.9% |  0.9% |

The measured share tracks the weight-derived expectation monotonically across
the whole range. The small excess at the low-weight end (priority 12–19) is the
expected effect of `cfs_min_vruntime` guaranteeing even the lowest-priority
process a minimum scheduling turn, i.e. no starvation.

This also resolves a documented open question (`Project_Guide.md §11.8`): the
`priority_test` Test 3 could not separate mid-range priorities by *finish order*.
Measuring **CPU share over a fixed window** separates them cleanly and matches
the expected weights — so the weight mapping is correct; the finish-order metric
simply lacked resolution.

### 2.2 LLM response cache — hit-rate

`eval cache 50` probes 50 keys cold (all new → miss) then warm (all re-queried
→ hit):

| round | probes | result |
|---|---:|---|
| 1 (cold) | 50 | 50 miss / 0 hit |
| 2 (warm) | 50 | 0 miss / 50 hit |
| total | 100 | **50% hit-rate** |

All warm probes hit even though 50 keys exceed the 16-slot RAM table, showing
the `/cache.bin` disk overlay promotes evicted entries back on hit. In
production each hit skips a Solar API round-trip, so the hit-rate is the fraction
of LLM calls avoided.

### 2.3 Reproducing

```bash
cd xv6-riscv && make kernel/kernel fs.img && cd ..
python3 tools/sec_audit.py        # kernel-side vulns (#1, #3): VULNERABLE/SAFE
python3 tools/sec_wire.py         # host-side wire injection (#4)
python3 tools/bench_report.py     # CFS share table + cache hit-rate
```
All harnesses use `-smp 1` and an isolated port + private `fs.img` copy, so they
run alongside a live agent session. (`bench_report.py` needs ~150 MB free RAM
for the `-m 128M` guest.)
