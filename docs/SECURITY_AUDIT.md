# 보안/버그 감사 — OS for LLM (xv6)

본 문서는 팀이 xv6 위에 추가한 **커스텀 코드**(에이전트 명령 경로·jail 샌드박스·
confirm-escape·F9 캐시·호스트 브릿지)를 대상으로 한 보안/버그 감사 결과와,
각 발견을 재현하는 레드팀 테스트 하네스를 정리한다.

- 대상: 커스텀 코드만. stock xv6 코드는 범위 외.
- 진행: 감사는 레드팀 하네스(신규 파일)로 additive하게, 수정은 `Dongjin` 브랜치에
  통합. 핵심 수정 **#1·#3·#4는 팀 코드리뷰(PR #13/#14)로 main에도 반영됨**.
- 기준 커밋: `143d0cc` (`Dongjin` = 신규 main, 자동 회귀 65/65 GREEN).

---

## 1. 감사 방법

7개 차원(커널 메모리 안전 · 동기화/레이스 · 샌드박스/권한 · 입력 파싱/정수 ·
자원 고갈 · 에러 경로/누수 · 호스트 브릿지)을 병렬로 감사하고, 각 발견을 **독립
검증자가 적대적으로 재확인**(실코드 대조)했다. 원시 20건 중 **16건 확정**,
4건은 기각/다운그레이드. 모든 인용은 실제 `file:line` 기준.

상태 표기: `고정됨` = 수정 반영(main/Dongjin) + 하네스가 SAFE로 확인 /
`라이브` = 여전히 취약(미수정) / `보류` = 위험·저가치라 의도적 미수정.

---

## 2. 확정 발견 요약

| # | 심각도 | 문제 | 위치 | 수정위험 | 상태 |
|---|---|---|---|---|---|
| 1 | HIGH | jailed agent가 `sys_dispatch`로 `CONFIRM_RES`를 보내 자기 위험 syscall(exec/kill/mknod)을 호스트 동의 없이 self-승인 | `sysproc.c`(sys_dispatch), `agentcmd.c`, `confirm.c` | 낮음 | **고정됨** (PR#13, `sys_dispatch` is_agent 거부) |
| 2 | HIGH | `/cache.bin`이 jailed 컨텍스트에서 chroot로 resolve → 캐시 split-brain + agentd가 위조 레코드를 호스트에 되먹임 | `cache.c`(disk_scan/append), `fs.c`(namex) | 낮음 | **라이브** (미수정) |
| 3 | MEDIUM | jailed agent의 `NICE`가 임의 user-class 프로세스 우선순위 변경(스케줄링 DoS) — self 제한 없음 | `sysproc.c`(sys_setpriority) | 낮음 | **고정됨** (PR#13, is_agent self-only 가드) |
| 4 | MEDIUM | 와이어 주입 — `read/write` 파일명·`spawn` argv가 `_wire_escape` 우회 → 개행으로 두 번째 `REQ\|` 위조 | `agent.py`(wire_for) | 낮음 | **고정됨** (spawn=PR / read·write 파일명=우리 agent.py) |
| 5 | MEDIUM | deny-list 기본값 `{KILL,EXEC}`가 실제 exec 표면인 SPAWN을 못 막음(운영자 기만) | `agentcmd.c`(deny_default), `agentd.c` | 낮음(문서) | **라이브** (백로그) |
| 6 | LOW | `confirm_tick`이 `confirm_pending_pid`를 락 없이 read(양성 레이스) | `confirm.c`(confirm_tick) | 낮음 | **보류** (무조건 wakeup 수정이 캐시 패닉 유발 → revert; 양성이라 미수정) |
| 7 | LOW | 호스트 confirm 프롬프트가 raw-REPL과 stdin 경합 + `tcflush` 과다 | `agent.py`(_handle_confirm_req) | 낮음 | 보류 |
| 8 | LOW | 캐시 `DISK_MAX_BYTES=4MB`가 FS의 MAXFILE(~268KB)보다 커서 가드 도달 불가 | `cache.c` | 낮음 | 라이브 |
| 9 | LOW | stage-1 intake ring(8슬롯) 가득 시 무진단 silent drop | `agentcmd.c`(agent_dispatch) | 낮음 | 보류 |

> **추가 수정(우리 기여, 감사 #1–9 외)**: smp>1에서 캐시 `RESP\|HIT` 콘솔 인터리브 → 캐시 미스 회귀 → `handle_cache_get` 단일 printf 원자화로 고정. `make qemu-agent`를 smp=1로 고정해 선재 kernelvec SMP race(캐시 패닉) 회피.

---

## 3. 상세 (상위 항목)

### #1 — confirm self-승인 (HIGH)
`confirm_resolve(pid, allow)`(confirm.c:115)는 `confirm_pending_pid == pid`만
보고 generation/nonce가 없다. 게다가 `sys_dispatch`(sysproc.c:352)에 `is_agent`
가드가 없고, `CONFIRM_RES`가 deny-list 검사(agentcmd.c:514)보다 **앞**(511)에서
라우팅된다. → jailed agent(또는 그 fork 자식)가 `dispatch("REQ|CONFIRM_RES|<self_pid>|y")`
한 번으로 자기 자신의 blocked syscall을 호스트 프롬프트 없이 통과.
- 1차 수정(낮음): jailed 프로세스가 `sys_dispatch`로 `CONFIRM_RES` 경로에 도달하지
  못하게 거부. 합법 응답은 serial/inline 경로(`try_inline_confirm_res`)로 오므로
  65/65 무영향.
- 2차(중간, 별도 PR): `CONFIRM_REQ|pid|gen|...` nonce 바인딩. 와이어 포맷 변경이라
  `agent.py:_handle_confirm_req` 동시 수정 필요.
- 재현: `tools/sec_audit.py` → `F1-confirm: RESULT=VULNERABLE`
  (`user/secconfirm.c`: jailed 부모가 자식의 mknod 게이트를 `dispatch("REQ|CONFIRM_RES|<child>|y")`로
  self-승인 → `child mknod APPROVED via self-dispatch (rc=0)`). 수정 후 SAFE.

### #2 — 캐시 jail-root resolution (HIGH)
`cache.c`는 jail을 전혀 인지하지 않는다(`is_agent`/`jail_root` 참조 0). `agent_drain`이
모든 usertrap 말미에 돌아 jailed agentd 컨텍스트에서 캐시 디스크 I/O가 일어나면,
`disk_scan`/`disk_append`의 `/cache.bin`이 `/agentbox/cache.bin`으로 갈린다 →
지속성 split-brain + jailed agentd의 WRITE로 위조 레코드 주입 → 호스트 read-back.
- 수정(낮음): `disk_scan`/`disk_append` 동안 jail 컨텍스트 임시 해제(save/restore,
  모든 에러 경로 포함) 또는 `iget(ROOTDEV, ROOTINO)`에서 절대 walk. 같은 PR에
  `T_FILE` 타입 가드(#E)도 추가.

### #3 — NICE 권한 상승 (MEDIUM) — 재현됨
`sys_setpriority`(sysproc.c:115-153)는 음수/커널-클래스 가드는 있으나 `is_agent`
호출자를 self로 제한하지 않는다(line 146). jailed agent가 `REQ|NICE|<pid>:19`로
임의 user-class 프로세스를 강등/우대 가능. NICE는 whitelisted·deny-list 부재·
confirm 비대상이라 프롬프트 없이 실행됨.
- 수정(낮음, ~3줄): `if(myproc()->is_agent && pid != myproc()->pid) return -1;`
  `priority_test`는 self만, agentd F8 self-retune도 self → 회귀 안전.
- 재현: `tools/sec_audit.py` → `RESULT=VULNERABLE`
  (`jailed setpriority(victim,19) rc=0 prio 10 -> 19`). 수정 후 `RESULT=SAFE`.

### #4 — 와이어 명령 주입 (MEDIUM)
`agent.py:wire_for`에서 `chat/write-text/print`는 `_wire_escape`되지만
`read`(303)·`write` 파일명(304)·`spawn` bin/argv(312,318)는 안 된다. JSON `"\n"`이
실제 개행으로 디코드돼 두 번째 `REQ|` 라인을 위조(jail 어휘 내로 한정 — RCE/탈옥
아님, 무결성 버그). 형제 경로 `_handle_llm_req`(796)는 이미 `.replace("\n"," ")`로
막고 있어 누락이 실수임을 방증.
- 수정(낮음): 해당 4개 필드를 `_wire_escape`로 감싸기. guest `unescape_inplace`가
  복원, 정상 입력엔 no-op.
- 재현: `tools/sec_wire.py`(호스트측 순수 단위 테스트, qemu 불필요) → read 파일명·
  write 파일명·spawn argv 3개 필드에서 위조 2번째 `REQ|` 라인 주입 확인. chat/
  write-text는 SAFE. 수정 후 3개 모두 SAFE.

---

## 4. 기각 / 문제 아님 (표면 커버 확인)

| 항목 | 위치 | 판정 | 이유 |
|---|---|---|---|
| `handle_cache_set` klen 부호 오버플로 | `agentcmd.c:362` | 기각 | `rem<klen` 런타임 가드가 독립 bound와 비교 → OOB 없음 |
| `disk_record` magic 없음 → over-read | `cache.c:53` | 다운그레이드 | vlen이 `CACHE_VAL`로 clamp, in-bounds. 정보성 |
| confirm pid accumulator 오버플로 | `agentcmd.c:383` | 다운그레이드 | 단일 pending pid 정확매칭만, 공격자 우회 불가 |
| SPAWN per-command fork DoS | `agentd.c:299` | 기각 | 라인당 동기 + `wait()` reap → 동시 1개 |
| `allocproc` ticks 레이스 | `proc.c:189` | 양성(수정 보류) | 무해. **제안된 "p->lock 보유 중 tickslock" 수정은 clockintr과 AB-BA 데드락 → 절대 금지** |

---

## 5. 레드팀 하네스 실행

```bash
# 커널측 시나리오 (qemu, smp=1·격리포트 5557·fs 복사본)
cd xv6-riscv && make kernel/kernel fs.img && cd ..
python3 tools/sec_audit.py     # #3 NICE(secnice) · #1 confirm self-승인(secconfirm)

# 호스트측 시나리오 (qemu 불필요)
python3 tools/sec_wire.py       # #4 와이어 주입(agent.py wire_for 단위 테스트)
```
각 시나리오는 기존 syscall/함수만 호출해 커널·`agent.py`를 수정하지 않으며,
`VULNERABLE`(현재 main) / `SAFE`(수정 후)로 분류한다. smp=1은 무관한 ASK 캐시-HIT
SMP 패닉을 회피하기 위함 — 여기서 패닉이 보이면 진짜 문제다. 커널측 시나리오는
`tools/sec_audit.py`의 `SCENARIOS` 리스트로, 호스트측은 `tools/sec_wire.py`의
`CASES`로 확장한다.

현재 재현 커버리지: #1(confirm self-승인) · #3(NICE 권한상승) · #4(와이어 주입).
