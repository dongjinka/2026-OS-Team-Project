# Security — OS for LLM (xv6)

The project's central claim is that **the kernel is the trust boundary**: a human
at the shell is unrestricted, while every action the LLM takes flows through a
sandbox (`chroot` jail → syscall deny-list → confirm-escape `y/N` gate). This
document is the single security reference: an **English overview** (§1) of the
threat model, method, reproduced vulnerabilities, and fixes, followed by the
**full Korean finding register** (§2) and the red-team harness instructions (§3).

Companion artifacts: [`BENCHMARKS.md`](BENCHMARKS.md) (raw CFS / cache numbers),
`Technical_Report.md` §6 (evaluation method), and the reproducers
`tools/sec_audit.py`, `tools/sec_wire.py`.

---

## 1. Overview (EN)

### 1.1 Threat model
We audited whether a **malicious or hijacked LLM**, acting as the jailed `agentd`
runtime, can break out of or weaken the boundary. A human at the shell is trusted
and unrestricted; the LLM's wire commands (`REQ|CMD|arg` over the serial console)
must pass deny-list → jail → confirm-escape.

### 1.2 Method
A multi-dimension audit (kernel memory safety, concurrency, sandbox escape, input
parsing, resource exhaustion, error paths, host bridge), each finding adversarially
verified against the source. Of 20 raw findings, 16 were confirmed; 4 rejected or
downgraded (including one proposed fix that would have introduced an AB-BA deadlock
with `clockintr`, so *not* applied). A **second, full-codebase sweep (2026-06-09)**
— not diff-scoped — then found three more in previously-unchanged code. Only the
team's custom code is in scope; stock xv6 is excluded.

### 1.3 Reproduced vulnerabilities
Each is reproduced deterministically by a test that only calls existing syscalls
(no kernel modification), classifying the build as `VULNERABLE` (hole present) or
`SAFE` (fixed). **#1, #3, and #4 are patched** on the current build (#1/#3 via team
review PR #13/#14; #4's read/write-filename part via our `agent.py` fix), so the
reproducers report `SAFE`.

| # | Severity | Vulnerability | Reproducer |
|---|---|---|---|
| 1 | High | A jailed agent self-approves its own confirm-escape gate | `tools/sec_audit.py` (`secconfirm`) |
| 3 | Medium | A jailed agent renices arbitrary processes (scheduling DoS) | `tools/sec_audit.py` (`secnice`) |
| 4 | Medium | LLM output injects a forged command line over the wire | `tools/sec_wire.py` |

**#1 — Confirm-escape self-approval.** `sys_dispatch()` (`kernel/sysproc.c`) had
no `is_agent` guard, and `agent_dispatch_now()` routes the `CONFIRM_RES` reply
*before* the deny-list (`kernel/agentcmd.c`). Because `confirm_resolve()` matches
only the pending PID, a jailed process could answer its own prompt:
`dispatch("REQ|CONFIRM_RES|<pid>|y")`.

**#3 — Privilege escalation via NICE.** `sys_setpriority()` guarded the
kernel-class boundary but never restricted a jailed agent (user-class) to
reniceing *itself*, so a hijacked LLM could emit `REQ|NICE|<pid>:19` to starve the
human shell or `:0` to favour itself.

**#4 — Wire command injection.** `agent.py:wire_for()` escaped newlines for
`chat`/`write`-text/`print` but not for the `read`/`write` filename or `spawn`
bin/argv. Since the kernel frames the serial stream one `REQ|...` line per `\n`, a
newline in those fields injected a second, attacker-chosen command.

> **Second audit (2026-06-09).** The full-codebase sweep found and fixed three more
> issues in previously-unchanged code — a **deny-list bypass** through the
> `LLM_RESP` / `ASK` cache-hit forward paths (#10), **cache-value wire-line forgery**
> via an embedded newline (#11), and a **re-jail inode-ref leak** (#12), plus a
> host-side `WRITE` `:`-misparse (#13). Confirmed by code trace and regression
> (`ralph_battery` 26/26, `sec_audit` SAFE/SAFE, cache 50% hit), not by a dedicated
> reproducer yet. Full detail in §2-2 below.

### 1.4 Fixes (low-risk, surgical)
Each fix is small and preserves the regression suite; each flips its reproducer
from `VULNERABLE` to `SAFE`.

- **#3** — `sys_setpriority()` rejects `myproc()->is_agent && pid != self` (F8
  self-tuning still works). On `main` via PR #13/#14.
- **#1** — `sys_dispatch()` rejects `is_agent` callers; host confirm replies arrive
  over the console, not via `dispatch`. On `main` via PR #13/#14.
- **#4** — `wire_for()` wraps the `read`/`write` filename and `spawn` bin/argv in
  `_wire_escape`.
- **#10** — the deny-list check moves into the single `forward_wire_to_agentd`
  chokepoint, so every forward path (normal + `ASK`/`LLM_RESP` cache-hit) is checked.
- **#11** — cache values are sanitized at the source (`cache_set` for RAM,
  `disk_scan` for `/cache.bin`), plus `forward_wire_to_agentd` stops at `\n`/`\r`.
- **#12** — `sys_jail()` `iput`s the previous `jail_root` before re-jailing.
- **#13** — `wire_for()` rejects a `WRITE` filename containing `:`.

Two further fixes from cache-reliability work: **cache RESP atomicity**
(`handle_cache_get()` emits `RESP|HIT|...` in a single `printf` so it can't
interleave with other CPUs' console output at `smp>1`) and **smp=1 agent mode**
(`make qemu-agent` runs single-core to dodge an intermittent kernelvec
`scause=0xf` race).

Still open: **#2** (`/cache.bin` resolving through the jail — only the
forged-record-feedback half is mitigated by #11) and **#5** (the default
deny-list not covering `SPAWN`), plus a `CONFIRM_REQ` nonce as defence-in-depth.
**#6** (a benign lock-free read in `confirm_tick`) is intentionally left as-is —
the unconditional-wakeup hardening we tried aggravated the kernelvec race and was
reverted.

---

## 2. 전체 발견 등록부 (KR, 상세)

대상은 팀이 xv6 위에 추가한 **커스텀 코드**(에이전트 명령 경로·jail 샌드박스·
confirm-escape·F9 캐시·호스트 브릿지)만. stock xv6는 범위 외. 상태 표기:
`고정됨` = 수정 반영 + 하네스 SAFE / `라이브` = 미수정 / `부분완화` = 일부만 수정 /
`보류` = 위험·저가치라 의도적 미수정.

### 2.1 확정 발견 요약 (1차 감사, 기준 `143d0cc`)

| # | 심각도 | 문제 | 위치 | 상태 |
|---|---|---|---|---|
| 1 | HIGH | jailed agent가 `sys_dispatch`로 `CONFIRM_RES`를 보내 위험 syscall(exec/kill/mknod)을 호스트 동의 없이 self-승인 | `sysproc.c`·`agentcmd.c`·`confirm.c` | **고정됨** (PR#13, `sys_dispatch` is_agent 거부) |
| 2 | HIGH | `/cache.bin`이 jailed 컨텍스트에서 chroot로 resolve → 캐시 split-brain + agentd가 위조 레코드를 호스트에 되먹임 | `cache.c`·`fs.c`(namex) | **부분완화** (위조-되먹임은 #11 캐시값 정화로 차단; jail-root split-brain은 미수정) |
| 3 | MEDIUM | jailed agent의 `NICE`가 임의 user-class 프로세스 우선순위 변경(스케줄링 DoS) — self 제한 없음 | `sysproc.c`(sys_setpriority) | **고정됨** (PR#13, is_agent self-only 가드) |
| 4 | MEDIUM | 와이어 주입 — `read/write` 파일명·`spawn` argv가 `_wire_escape` 우회 → 개행으로 두 번째 `REQ\|` 위조 | `agent.py`(wire_for) | **고정됨** |
| 5 | MEDIUM | deny-list 기본값 `{KILL,EXEC}`가 실제 exec 표면인 SPAWN을 못 막음 | `agentcmd.c`(deny_default)·`agentd.c` | **라이브** (백로그) |
| 6 | LOW | `confirm_tick`이 `confirm_pending_pid`를 락 없이 read(양성 레이스) | `confirm.c`(confirm_tick) | **보류** (무조건 wakeup 수정이 캐시 패닉 유발 → revert) |
| 7 | LOW | 호스트 confirm 프롬프트가 raw-REPL과 stdin 경합 + `tcflush` 과다 | `agent.py`(_handle_confirm_req) | 보류 |
| 8 | LOW | 캐시 `DISK_MAX_BYTES=4MB`가 FS의 MAXFILE(~268KB)보다 커서 가드 도달 불가 | `cache.c` | 라이브 |
| 9 | LOW | stage-1 intake ring(8슬롯) 가득 시 무진단 silent drop | `agentcmd.c`(agent_dispatch) | 보류 |

> **추가 수정(감사 #1–9 외)**: smp>1에서 캐시 `RESP\|HIT` 콘솔 인터리브 → 캐시 미스
> 회귀 → `handle_cache_get` 단일 printf 원자화로 고정. `make qemu-agent`를 smp=1로
> 고정해 선재 kernelvec SMP race(캐시 패닉) 회피.

### 2.2 2차 감사 (2026-06-09 — 전체 코드 재검증)

1차 감사가 **변경분(diff) 중심**이었던 한계를 보완해, 프로젝트 커스텀 코드 전체를
서브시스템별 병렬 에이전트로 다시 sweep했다. **미변경 기존 코드**에서 신규 발견
3건(HIGH 2 + LOW 1)과 호스트측 LOW 1건을 확정하고 전부 같은 패스에서 수정·회귀
검증(`ralph_battery` 26/26, `sec_audit` SAFE/SAFE, 캐시 50% hit)했다.

| # | 심각도 | 문제 | 위치 | 상태 |
|---|---|---|---|---|
| 10 | HIGH | **deny-list 우회 (메타 포워딩 경로)** — `handle_llm_resp`·`handle_ask` 정확-캐시-히트가 `forward_wire_to_agentd`를 **deny-list 검사 없이** 호출. `REQ\|LLM_RESP\|<cmd>\|...` 또는 `CACHE_SET`→`ASK` 재생으로 deny된 명령이 agentd 도달 | `agentcmd.c` | **고정됨** |
| 11 | HIGH | **캐시값 → wire 라인 위조 (개행 주입)** — user-writable `/cache.bin`이나 ungated `set_cache`로 심은 개행 포함 값이 `RESP\|HIT\|%s` printf·agentd 포워딩에서 그대로 방출 → 두 번째 라인 위조 | `cache.c`·`agentcmd.c` | **고정됨** |
| 12 | LOW | **재-jail inode ref 누수** — `jail()`이 `agent_blocked` 비대상이라 재호출 가능. `sys_jail`이 이전 `jail_root`를 `iput` 없이 덮어써 호출마다 누수(탈옥 아님) | `sysfile.c`(sys_jail) | **고정됨** |
| 13 | LOW | **WRITE 파일명 `:` 오분할** — `wire_for`의 `WRITE\|<file>:<text>`에서 파일명에 `:`이 있으면 agentd가 첫 `:`로 오분할(LLM이 양 필드 제어 → 무결성 버그) | `agent.py`(wire_for) | **고정됨** |

#### #10 — deny-list 우회 (메타 포워딩 경로) (HIGH)
agentd로 가는 모든 enqueue는 `forward_wire_to_agentd`(단일 chokepoint)를 거치지만,
deny-list 검사는 `agent_dispatch_now`의 정상 경로에만 있었다. `ASK` 정확-히트와
`LLM_RESP`는 메타 명령으로 라우팅돼 deny 검사 **앞에서** `forward_wire_to_agentd`를
직접 호출 → F7 deny-list 우회. 바로 아래 semantic-hit 경로가 `CHAT\|`/`PRINT\|`
접두로 제한된 것이, 작성자도 이 불변식을 의도했으나 정확-히트/LLM_RESP 경로엔
누락됐음을 방증한다.
- 수정: deny-list 검사(+개행 차단)를 `forward_wire_to_agentd` chokepoint로 일원화.
  기본 deny-list가 `{KILL,EXEC}`라 정상 흐름 무영향.

#### #11 — 캐시값 wire 라인 위조 (HIGH)
캐시값은 "단일 라인 와이어 텍스트"라는 가정 하에 방출되나 이를 강제하는 곳이 없었다.
`/cache.bin`은 user-writable(#2)이고 `set_cache`/`get_cache` syscall엔 `is_agent`
게이트가 없어, 개행 포함 값을 심으면 `RESP\|HIT\|%s`(개행 미차단 sink)나 agentd
포워딩에서 두 번째 라인이 위조된다.
- 수정: 값을 **출처에서 정화** — `disk_scan`(디스크 출처)·`cache_set`(RAM 출처)에서
  제어문자(<0x20)를 공백으로 치환. 추가로 `forward_wire_to_agentd`에서 `\n`/`\r`
  복사 중단(이중 방어). `set_cache`/`get_cache`의 `is_agent` 게이트 부재는 값 정화
  이후 **하드닝 갭**(구체 취약점 아님)으로 백로그.

### 2.3 상세 (1차 상위 항목)

#### #1 — confirm self-승인 (HIGH)
`confirm_resolve(pid, allow)`는 `confirm_pending_pid == pid`만 보고 generation/nonce가
없다. `sys_dispatch`에 `is_agent` 가드가 없고 `CONFIRM_RES`가 deny-list 검사보다
**앞**에서 라우팅돼, jailed agent가 `dispatch("REQ|CONFIRM_RES|<self_pid>|y")` 한 번으로
자기 blocked syscall을 호스트 프롬프트 없이 통과.
- 수정(낮음): jailed 프로세스가 `sys_dispatch`로 `CONFIRM_RES`에 도달 못하게 거부.
  합법 응답은 serial/inline(`try_inline_confirm_res`)로 오므로 회귀 무영향.
- 재현: `tools/sec_audit.py` → `F1-confirm: VULNERABLE`(`user/secconfirm.c`). 수정 후 SAFE.

#### #2 — 캐시 jail-root resolution (HIGH)
`cache.c`는 jail을 전혀 인지하지 않는다(`is_agent`/`jail_root` 참조 0). `agent_drain`이
jailed agentd 컨텍스트에서 캐시 디스크 I/O를 하면 `disk_scan`/`disk_append`의
`/cache.bin`이 `/agentbox/cache.bin`으로 갈린다 → 지속성 split-brain + 위조 레코드
주입. **위조-되먹임 절반은 #11(캐시값 정화)로 차단**; split-brain 자체는 미수정.
- 수정 제안: `disk_scan`/`disk_append` 동안 jail 컨텍스트 임시 해제(save/restore) 또는
  `iget(ROOTDEV, ROOTINO)`에서 절대 walk.

#### #3 — NICE 권한 상승 (MEDIUM) — 재현됨
`sys_setpriority`는 음수/커널-클래스 가드는 있으나 `is_agent` 호출자를 self로 제한하지
않았다. jailed agent가 `REQ|NICE|<pid>:19`로 임의 user-class 프로세스 강등/우대 가능.
- 수정(~3줄): `if(myproc()->is_agent && pid != myproc()->pid) return -1;`
- 재현: `tools/sec_audit.py` → `VULNERABLE`(`jailed setpriority(victim,19) rc=0`). 수정 후 SAFE.

#### #4 — 와이어 명령 주입 (MEDIUM)
`wire_for`에서 `chat/write-text/print`는 `_wire_escape`되지만 `read`·`write` 파일명·
`spawn` bin/argv는 안 됐다. JSON `"\n"`이 실제 개행으로 디코드돼 두 번째 `REQ|` 라인을
위조(jail 어휘 내로 한정 — 무결성 버그).
- 수정(낮음): 해당 필드를 `_wire_escape`로 감싸기. guest `unescape_inplace`가 복원.
- 재현: `tools/sec_wire.py`(호스트측 순수 단위 테스트). 수정 후 3개 필드 모두 SAFE.

### 2.4 기각 / 문제 아님 (표면 커버 확인)

| 항목 | 위치 | 판정 | 이유 |
|---|---|---|---|
| `handle_cache_set` klen 부호 오버플로 | `agentcmd.c` | 기각 | `rem<klen` 런타임 가드 → OOB 없음 |
| `disk_record` magic 없음 → over-read | `cache.c` | 다운그레이드 | vlen이 `CACHE_VAL`로 clamp, in-bounds |
| confirm pid accumulator 오버플로 | `agentcmd.c` | 다운그레이드 | 단일 pending pid 정확매칭만, 우회 불가 |
| SPAWN per-command fork DoS | `agentd.c` | 기각 | 라인당 동기 + `wait()` reap → 동시 1개 |
| `allocproc` ticks 레이스 | `proc.c` | 양성(보류) | 무해. **제안된 "p->lock 보유 중 tickslock" 수정은 clockintr과 AB-BA 데드락 → 절대 금지** |

---

## 3. 레드팀 하네스 실행

```bash
# 커널측 시나리오 (qemu, smp=1·격리포트 5557·fs 복사본)
cd xv6-riscv && make kernel/kernel fs.img && cd ..
python3 tools/sec_audit.py     # #3 NICE(secnice) · #1 confirm self-승인(secconfirm)

# 호스트측 시나리오 (qemu 불필요)
python3 tools/sec_wire.py       # #4 와이어 주입(agent.py wire_for 단위 테스트)
```

각 시나리오는 기존 syscall/함수만 호출해 커널·`agent.py`를 수정하지 않으며,
`VULNERABLE`(현재 빌드) / `SAFE`(수정 후)로 분류한다. smp=1은 무관한 ASK 캐시-HIT
SMP 패닉을 회피하기 위함 — 여기서 패닉이 보이면 진짜 문제다. 커널측은
`tools/sec_audit.py`의 `SCENARIOS`, 호스트측은 `tools/sec_wire.py`의 `CASES`로 확장한다.

현재 재현 커버리지: #1(confirm self-승인) · #3(NICE 권한상승) · #4(와이어 주입).
2차 감사 발견 #10–#13은 전체-코드 sweep + 적대적 코드 트레이스로 확정하고 회귀로
무영향을 검증했다 — 전용 `VULNERABLE`/`SAFE` 재현기는 아직 없다(후속 작업).
정량 평가(CFS share·캐시 hit-rate)는 [`BENCHMARKS.md`](BENCHMARKS.md) 참조.
