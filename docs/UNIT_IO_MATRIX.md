# Unit I/O Matrix — agent command path

에이전트 명령 경로의 **단위 기능별로 입력→기대 출력 쌍을 사전 정의**하고, 각각을
검증하는 하네스를 매핑한 표. "검증" 열은 실제 실행으로 확인된 것만 ✅.

스코프는 사용자가 신뢰 경계로 지정한 경로 — deny-list → jail → confirm-escape,
F9 캐시, 와이어 이스케이프 라운드트립, CFS 점유율 — 에 한정한다(전체 커널 함수가
아니라 에이전트가 실제로 통과하는 게이트들).

빌드 선행: `cd xv6-riscv && make kernel/kernel fs.img`.

| # | 단위 기능 | 입력 | 기대 출력 | 검증 하네스 | 검증 |
|---|---|---|---|---|---|
| U1 | **confirm-escape 허용** | jailed agent `exec`/`kill`/`mknod` 시도 → 호스트 `y` | syscall 실행됨 (`status=0`, `SPAWN /echo done`) | `ralph_natlang.py` N8 | ✅ N8 PASS |
| U2 | **confirm-escape 거부/타임아웃** | 호스트 `n` 또는 15초 무응답 | `denied (confirm-escape)`, syscall 차단 | `ralph_natlang.py` N16; `sec_audit.py` secconfirm | ✅ N16 PASS, secconfirm SAFE |
| U3 | **confirm self-승인 차단 (#1)** | jailed가 `sys_dispatch`로 `CONFIRM_RES` self 전송 | 거부 (is_agent 가드) → `SAFE` | `sec_audit.py` (secconfirm) | ✅ F1-confirm SAFE |
| U4 | **deny-list 차단** | agent wire `KILL`/`EXEC` (기본 deny `{KILL,EXEC}`) | agentd 도달 전 드롭 | `ralph_battery.py`; `agent_multi` ACL DENY | ✅ battery 26/26, agent_multi DENY KILL ×4 |
| U5 | **jail chroot** | jailed agent가 `/` 밖(`../etc`) read 시도 | `not reachable inside jail` | `ralph_natlang.py`; `agentdemo` | ✅ natlang green, agentdemo PASS(9/9) |
| U6 | **NICE 권한 상승 차단 (#3)** | jailed `setpriority(victim,19)` | `rc=-1` (is_agent self-only) → `SAFE` | `sec_audit.py` secnice | ✅ F3-nice SAFE |
| U7 | **F9 캐시 exact hit** | 동일 `:ask` 프롬프트 2회 | 2회차 Solar 호출 없이 `RESP\|HIT` | `cache_test`; `ralph_natlang.py` N6 | ✅ cache_test 13P/0F, N6 PASS |
| U8 | **F9 캐시 semantic hit** | 패러프레이즈 프롬프트 (Jaccard ≥ 0.40) | `SEMANTIC HIT score=k/64` | `cache_test` (13/13) | ✅ cache_test 13P/0F |
| U9 | **F9 캐시 miss → set** | 신규 프롬프트 | `LLM_REQ` 발생 → `cache_set` | `ralph_natlang.py` N5 | ✅ N5 PASS |
| U10 | **와이어 이스케이프 라운드트립 (#4)** | read/write 파일명·spawn argv에 `\n` | `_wire_escape`→단일 라인, 게스트 `unescape_inplace` 복원 | `tools/sec_wire.py` | ✅ 0 injectable fields |
| U11 | **CFS 우선순위 점유율** | `cfs_bench` (CPUS=1) | 낮은 nice가 더 많은 tick (가중치 1024/423/172/70/29/15 비례) | `regression.sh` eval fair; `bench_report.py` | ✅ regression eval fair PASS (bench_report 미실행) |

## 실행

```bash
cd xv6-riscv && make kernel/kernel fs.img && cd ..
python3 tools/sec_wire.py            # U10 (qemu 불필요)
python3 tools/sec_audit.py           # U2/U3/U6 (포트 5557)
python3 tools/ralph_battery.py       # U4/U5 외 (포트 5555)
python3 tools/ralph_natlang.py       # U1/U2/U5/U7/U9 외 (포트 6666, mock)
./tools/regression.sh                # cache_test(U7/U8) 포함 9 단위
python3 tools/bench_report.py        # U11 (포트 5558)
```

> 참고: `ralph_*`는 mock 타이밍 의존이라 개별 시나리오가 간헐적으로 흔들릴 수 있다
> (재실행 시 통과). 결정적 검증은 U10(와이어 라운드트립)과 `sec_audit`/`cache_test`.
