# LLM-OS — An Agent Runtime on xv6 (Direction A: *OS for LLM*)

> A tiny operating system (xv6-riscv) extended to **host, schedule, sandbox, and
> cache an LLM agent**. The kernel is the trust boundary; the LLM (Upstage Solar
> Pro) only ever acts through it.
>
> *(KR: xv6 커널을 LLM 에이전트를 **호스팅·스케줄링·샌드박싱·캐싱**하도록 확장한
> "OS for LLM" 프로젝트. 커널이 신뢰 경계이고, LLM은 항상 커널을 통해서만
> 동작한다.)*

> **Note on branches.** This document describes the implementation on the
> **`main`** branch (the canonical, fullest state, including the F9 cache and the
> evaluation harness). *(KR: 본 문서는 `main` 브랜치 기준 — F9 캐시·평가
> 하네스 포함.)*
>
> *(KR: 한국어 README는 [README.md](README.md)를 참고하세요. This is the English
> companion to the Korean README.)*

---

## 1. Project Summary

**Direction A — OS for LLM.** We took the three core components of the **AIOS**
("LLM Agent Operating System") paper and implemented each one *inside a real
kernel* — xv6-riscv — rather than as a userspace wrapper:

| AIOS component      | Our implementation                                                      |
| ------------------- | ----------------------------------------------------------------------- |
| **Agent Scheduler** | A **CFS scheduler** in the kernel, using Linux's `sched_prio_to_weight` weights |
| **Tool Manager**    | A **kernel command queue + jailed `agentd` worker** speaking a `REQ\|` wire protocol, with a configurable deny-list and an LLM-response cache |
| **LLM-Kernel Bridge** | **`agent.py`** — a host-side ReAct loop bridging Solar API ↔ the QEMU serial port |

The key design invariant: **a human's shell input runs unrestricted, but every
command the LLM issues is executed only inside a chroot jail with dangerous
syscalls blocked.** The privilege boundary between human and LLM is enforced by
the *code path itself*, not by convention.

*(KR: 핵심 불변식 — 사람이 친 셸 명령은 무제한, LLM이 만든 명령은 모두 chroot
jail + 위험 syscall 차단 안에서만 실행. 사람/LLM의 권한 경계가 코드 경로 자체로
분리된다.)*

This satisfies the course's mandatory constraint that **OS concepts we design
and implement** be a substantive part of the project — see
[§6 OS Concepts](#6-os-concepts-in-play) and the
[Technical Report](docs/Technical_Report.md).

---

## 2. Feature Status

| # | Feature | Status |
| - | ------- | ------ |
| F1 | `setpriority` / `getpriority` syscalls | ✅ |
| F2 | user/kernel priority classes (negative = kernel; `init` = −5); two-way escalation guard | ✅ |
| F3 | CFS scheduler — Linux weight table + vruntime + array scan | ✅ |
| F4 | CFS details — fork inheritance, I/O wakeup bonus, global `cfs_min_vruntime` | ✅ |
| F5 | QEMU ↔ Upstage Solar Python bridge (`.env` auto-load) | ✅ |
| F6 | LLM JSON deserialization (host-side parsing — see report for rationale) | ✅ |
| F7 | Sandboxing — chroot jail + `exec`/`kill`/`mknod` block + tool whitelist + configurable deny-list | ✅ |
| F8 | Per-tool priority customization (`SETPRIO` / `LIST`) | ✅ |
| — | ReAct autonomous agent loop + conversation memory | ✅ (bonus) |
| F9 | LLM response cache — `cache.c` (RAM + `/cache.bin` disk overlay + MinHash/Jaccard) + kernel `ASK` orchestration + `agent.py :ask` path (hit ⇒ skip Solar) | ✅ |
| F10 | Idle-time LoRA training | ❌ out of scope (Future Work) |

---

## 3. Tech Stack

| Layer            | Technology                                                              |
| ---------------- | ---------------------------------------------------------------------- |
| Kernel / OS      | **xv6-riscv** (MIT teaching OS), C, RISC-V (rv64)                       |
| Emulator         | **QEMU ≥ 7.2** (`qemu-system-riscv64`), serial exposed over TCP        |
| Cross-toolchain  | `riscv64` GCC / binutils                                                |
| Host bridge      | **Python 3** (`agent.py`), `openai` SDK (Solar is OpenAI-API-compatible) |
| LLM backend      | **Upstage Solar Pro** (`solar-pro2`), via `https://api.upstage.ai/v1`   |
| Build / test     | GNU Make; `tools/regression.py` regression harness                      |

No floating point and no dynamic allocation are used in any kernel-side
addition — both are restricted in xv6.

---

## 4. Repository Layout

```
.
├── README.md                  ← you are here (Deliverable #1: Application)
├── agent.py                   ← host-side ReAct bridge (LLM-Kernel bridge); :ask = F9 cache path
├── .env.example               ← copy to .env, add your Solar API key (gitignored)
├── docs/
│   ├── Technical_Report.md     ← Deliverable #2: architecture, OS concepts, LLM integration
│   └── Development_Process.md   ← Deliverable #3: planning → execution → retrospective
├── tools/regression.py        ← multi-test regression harness (build + boot + checks)
├── Implementation.md          ← deep, code-referenced architecture notes (KR)
├── plan.md                    ← feature breakdown & status tracking (KR)
├── CHANGELOG.md               ← chronological change log (KR)
├── Project_Guide.md           ← extended internal walkthrough (KR)
└── xv6-riscv/
    ├── kernel/
    │   ├── proc.c  proc.h     ← CFS weights, vruntime, is_agent / jail_root fields
    │   ├── trap.c console.c   ← per-tick vruntime accrual; agent_drain() hook
    │   ├── agentcmd.c deny.h  ← 2-stage dispatch, configurable deny-list, ASK/cache meta-cmds
    │   ├── cache.c            ← F9 LLM response cache (RAM + /cache.bin + MinHash/Jaccard)
    │   ├── confirm.c          ← confirm-escape prototype (currently disabled — see Limitations)
    │   ├── fs.c sysfile.c     ← chroot jail (namex), jail() syscall
    │   ├── syscall.c          ← blocks exec/kill/mknod for agent processes
    │   ├── sysproc.c          ← setpriority guard; agent_recv/set_deny/get_deny/cache/dispatch syscalls
    │   └── procinfo.h         ← process-snapshot struct (PS self-observation)
    └── user/
        ├── agentd.c           ← jailed agent worker (tool table + per-fn priority + PS/HELP/CHAT)
        ├── denyctl.c          ← shell tool to manage the deny-list (list/add/rm/reset/save/load)
        ├── agentdemo.c        ← F2/F7 sandbox demo
        ├── cfs_share.c        ← quantitative CFS fairness benchmark
        ├── priority_test.c    ← priority + scheduler test (auto-verified finish order)
        ├── cache_test.c       ← F9 cache unit test (13/13)
        ├── eval.c             ← evaluation harness: cache / acl / fair / semantic
        ├── agent_multi.c      ← 4 concurrent role-based agents (concurrency demo)
        └── write_race.c       ← inode-sleeplock write serialization demo
```

---

## 5. Setup

### 5.1 Host dependencies *(KR: 호스트 의존성)*

```bash
# macOS
brew install qemu riscv-software-src/riscv/riscv-tools
pip3 install openai

# Debian / Ubuntu / WSL2
sudo apt install qemu-system-misc gcc-riscv64-linux-gnu python3-pip make
pip3 install openai
```

> `openai` is only needed for live LLM mode; without it the bridge falls back to
> a rule-based mock so you can still exercise the kernel path.

### 5.2 Solar API key *(KR: API 키 — 절대 커밋하지 말 것)*

The Upstage Solar key is supplied by the instructor (per team). **Never commit
it.** `.env` is gitignored; `.env.example` is the committed template.

```bash
cp .env.example .env
# then edit .env:
#   UPSTAGE_API_KEY=up_xxxxxxxxxxxxxxxxxxxxxxxxxxxx
#   UPSTAGE_MODEL=solar-pro2
```

API docs: <https://console.upstage.ai/docs>. Personal prototyping access:
<https://www.upstage.ai/events/ai-initiative-2025-ko>.

---

## 6. How to Run

Two run modes: a plain shell (to run kernel tests directly) and an agent mode
(kernel listens on a TCP serial port for the Python bridge).

### 6.1 Shell mode — run the OS tests *(KR: 셸 모드 — 커널 테스트 직접 실행)*

```bash
cd xv6-riscv
make clean
make qemu          # boots xv6 to an interactive shell

# at the xv6 '$' prompt:
$ priority_test        # F1/F2/F3/F4: priority syscalls + scheduler finish order
$ agentdemo            # F2/F7: jail isolation + privilege guard + blocked syscalls
$ cache_test           # F9: cache RAM-hit / evict / disk-promote (13/13)
$ eval cache 50        # F9: round-1 miss vs round-2 hit rate
$ eval acl 1           # F7: per-role ACL deny rate
$ eval fair 30000000   # F3/F4: prio=0 vs prio=20 completion time
$ eval semantic 1      # F9: exact / paraphrase / unrelated semantic matching
$ agent_multi          # concurrency: 4 role-based agents interleaved by CFS
$ write_race           # synchronization: inode sleeplock serializes writers
$ denyctl list         # F7: show the effective kernel deny-list
```

For the fairness benchmark, single-core makes time-sharing clearest:

```bash
make clean && make qemu CPUS=1
$ cfs_share
```

> Quit QEMU with `Ctrl-a x`.

### 6.2 Agent mode — talk to the LLM *(KR: 에이전트 모드 — LLM과 대화)*

**Terminal 1** — boot xv6 with its serial port on TCP `127.0.0.1:4444`:

```bash
cd xv6-riscv
make qemu-agent
```

**Terminal 2** — start the host bridge (from the repo root):

```bash
python3 agent.py
```

The bridge prints `mode: solar (solar-pro2)` (key present) or `mode: mock`
(no key) and drops into a REPL. Ask in plain language — it plans, runs sandbox
tools, observes their output, and remembers the conversation:

```
you ▸ make a file plan.txt with three TODO items, then read it back
you ▸ what files have I created so far?
you ▸ summarise everything written to files
you ▸ :ask what is a vruntime?       ← routes through the kernel F9 cache (hit ⇒ skips Solar)
you ▸ :role reader                   ← tag subsequent requests with a role
```

> On boot, `init` automatically launches `agentd` (which jails itself into
> `/agentbox`) and applies the persisted deny-list (`denyctl load`). Every tool
> the LLM calls executes inside the jail.

---

## 7. OS Concepts in Play

*(KR: 적용한 OS 개념과 위치 — 자세한 설명은 [Technical Report](docs/Technical_Report.md))*

| OS concept              | Where it lives                                                        |
| ----------------------- | -------------------------------------------------------------------- |
| **Scheduling (CFS)**    | `kernel/proc.c` (weights, vruntime, leftmost pick), `kernel/trap.c` (per-tick accrual) |
| **Processes & priority**| `setpriority`/`getpriority`; user vs. kernel class; two-way escalation guard |
| **System calls**        | New `jail`, `agent_recv`, `set_deny`/`get_deny`, `procinfo`, `set_cache`/`get_cache`, `dispatch` |
| **Protection / sandbox**| chroot jail in `namex()`; `exec`/`kill`/`mknod` blocked for agents; configurable deny-list (humans-only) |
| **Synchronization**     | inode sleeplock (`write_race`), cache + deny-list spinlocks, log transactions (`begin_op`) for the cache disk overlay |
| **Concurrency / IPC**   | `agentcmd.c` 2-stage queue (ISR enqueue → process-context drain); `agent_multi` runs 4 concurrent agents |
| **File system**         | jailed file tools in `/agentbox`; `/cache.bin` cache overlay; `/denylist.conf` persistence |

---

## 8. Demo

*(KR: 데모 — 아래 스크린샷 자리표시자에 실제 캡처/GIF를 채워 넣을 것.)*

### 8.1 Example session (live transcript)

A real multi-step run in `solar` mode (the LLM plans `write` → `read` → answer):

```text
================================================================
  xv6 LLM agent   ·   mode: solar (solar-pro2)   ·   max 8 steps/request
================================================================
you ▸ make a file todo.txt with two tasks, then summarise it for me
   💭 I'll create the file first.
   ▶ step 1 · write  file=todo.txt  text=1) write report\n2) test cfs
   xv6 ┃ [agentd] wrote todo.txt (28 bytes)
   💭 Now read it back to confirm and summarise.
   ▶ step 2 · read  file=todo.txt
   xv6 ┃ [agentd] 1) write report
   xv6 ┃ [agentd] 2) test cfs
╭─ answer ──────────────────────────────────────╮
│ I created todo.txt with two tasks: (1) write the report,
│ (2) test CFS. It now contains both items.
╰───────────────────────────────────────────────╯

you ▸ try to delete the init process
   💭 The user wants to kill a process, but kill is not available.
   ▶ step 1 · print  msg=kill is not permitted in the sandbox
   xv6 ┃ [agentd] kill is not permitted in the sandbox
╭─ answer ──────────────────────────────────────╮
│ I can't do that — the sandbox blocks process termination
│ (kill/exec). I can only work with files inside my jail.
╰───────────────────────────────────────────────╯

you ▸ :ask what is a vruntime?
   xv6 ┃ [cache] MISS → asking host
   ... (Solar answers; response cached) ...
you ▸ :ask what is a vruntime?
   xv6 ┃ [cache] HIT (semantic)        ← no Solar call this time
```

### 8.2 Screenshots / GIF *(to be added)*

> **TODO (team):** capture these and drop the files in `docs/media/`, then the
> links below will render. *(KR: 아래 캡처를 찍어 `docs/media/`에 넣으면 링크가
> 렌더링됨.)*

| What to capture | Suggested command | Placeholder |
| --------------- | ----------------- | ----------- |
| Agent REPL doing a multi-step task | `make qemu-agent` + `python3 agent.py` | `![agent demo](docs/media/agent-demo.gif)` |
| F9 cache HIT vs MISS via `:ask` | agent mode, repeat an `:ask` | `![cache hit](docs/media/cache-hit.png)` |
| `priority_test` all PASSED | `make qemu` → `priority_test` | `![priority test](docs/media/priority-test.png)` |
| `cfs_share` CPU-share table | `make qemu CPUS=1` → `cfs_share` | `![cfs share](docs/media/cfs-share.png)` |
| `eval cache` / `eval acl` numbers | `make qemu` → `eval cache 50` | `![eval](docs/media/eval.png)` |

**How to record a terminal GIF:** install
[`asciinema`](https://asciinema.org) + [`agg`](https://github.com/asciinema/agg),
then `asciinema rec demo.cast` → run the demo → `agg demo.cast docs/media/agent-demo.gif`.

---

## 9. Verification at a Glance

*(KR: 검증 요약 — 상세 표는 [Implementation.md §6](Implementation.md))*

| Case                                  | Result |
| ------------------------------------- | ------ |
| Kernel boot (CPUS=1 and CPUS=3)       | ✅ no panic |
| `priority_test` Test 1 / 2 / 3        | ✅ PASSED (Test 3 auto-checks finish order HIGH→MED→LOW via pipe) |
| `agentdemo` (sandbox/privilege checks)| ✅ all pass |
| `cache_test`                          | ✅ 13/13 (RAM hit / evict / disk promote) |
| `denyctl add WRITE` → `REQ\|WRITE\|`  | ✅ blocked in kernel (never reaches `agentd`) |
| `denyctl save` → reboot → `list`      | ✅ `/denylist.conf` auto-loaded, entries survive |
| Jail escape via `..` / outside paths  | ✅ DENIED |
| Agent `exec` / `kill` / `mknod`       | ✅ return −1 |
| User → negative-priority escalation / kernel-class demotion | ✅ DENIED |
| `REQ\|KILL\|…` deny-list              | ✅ blocked before reaching `agentd` |
| F9 `:ask` repeated                    | ✅ MISS then HIT (Solar skipped on hit) |
| Live Solar ReAct multi-step + memory  | ✅ ls→read×N→summary; follow-up uses prior context |

---

## 10. Limitations & Future Work

*(KR: 한계·향후 작업 — 상세는 [Technical Report §Limitations](docs/Technical_Report.md))*

- **F6 (JSON deserialization)** is done on the **host** (`agent.py`), not in the
  kernel. The kernel only accepts a validated minimal `REQ|<CMD>|<arg>` format.
  Rationale (kernel safety, no float/heap in xv6, layer separation) is in the
  report — a deliberate departure from the original proposal wording.
- **Confirm-escape** (`kernel/confirm.c`): a prototype that turns the hard
  `exec`/`kill`/`mknod` block into a one-time host-confirmed allow. It is
  **currently disabled** (a suspected `kerneltrap` panic); the stable behavior is
  unconditional blocking.
- **F10 (idle-time LoRA training)** is out of scope for the xv6 environment.
- `cfs_share` shares are environment-dependent; use `CPUS=1` for stable numbers.

---

## 11. Documents (Deliverables)

| # | Deliverable | File |
| - | ----------- | ---- |
| 1 | **Application** (this README + source) | `README.md`, `xv6-riscv/`, `agent.py` |
| 2 | **Technical Report** | [docs/Technical_Report.md](docs/Technical_Report.md) |
| 3 | **Development Process** | [docs/Development_Process.md](docs/Development_Process.md) |
| 4 | **Presentation Slides** (English) | *to be added before Week 14* |

Supporting (Korean, internal): [Implementation.md](Implementation.md) ·
[plan.md](plan.md) · [CHANGELOG.md](CHANGELOG.md) ·
[Project_Guide.md](Project_Guide.md)

---

## 12. License & Credits

- xv6-riscv is MIT-licensed (`xv6-riscv/LICENSE`); our changes are released
  under the same license.
- Design motif: Kai Mei et al., *AIOS: LLM Agent Operating System*, 2024.

---

## 13. Team

| Member | Focus (derived from git history) |
| ------ | -------------------------------- |
| Se-Joong Kim | xv6 integration, scheduler foundation, jail/sandbox rewrite, F9 cache, regression harness |
| SeungBeom Kim | Core LLM-OS features: CFS, sandboxing, `agentd`, `agent.py` loop, security guards, PS/HELP |
| June Kong | Evaluation automation (`cfs_share`, Test 3), documentation, design decisions |
| Dongjin Ka | Repository / review |

> *(KR: 역할은 git 이력에서 추론한 것 — 실제와 다르면 수정 바람.)*
