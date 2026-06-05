# 평가 벤치마크 결과

`tools/bench_report.py`로 캡처한 **대표 측정값**(smp=1). 수치는 머신/부하에 따라
조금씩 달라지므로, 재생성은:

```bash
cd xv6-riscv && make kernel/kernel fs.img && cd ..
python3 tools/bench_report.py > docs/BENCHMARKS.md
```

측정 환경: QEMU riscv64, `-smp 1 -m 128M`, 대표 실행 2026-06-04.

---

## 1. CFS 우선순위 → CPU 점유율

6개 자식(`user/cfs_bench.c`)이 동일 wall-clock(150 ticks) 동안 각자 고정
우선순위로 경쟁한 루프 카운트와 점유율. 낮은 priority 값 = 높은 가중치
(`kernel/proc.c`의 Linux `cfs_weight[]`) = 더 많은 CPU. "기대 share"는 가중치
비율(`weight / Σweight`)이다.

| priority | 가중치(weight) | loop count | 측정 share | 기대 share |
|---:|---:|---:|---:|---:|
| 0  | 1024 | 352,688,000 | 56.9% | 59.1% |
| 4  |  423 | 148,146,000 | 23.9% | 24.4% |
| 8  |  172 |  60,202,000 |  9.7% |  9.9% |
| 12 |   70 |  32,111,000 |  5.2% |  4.0% |
| 16 |   29 |  15,648,000 |  2.5% |  1.7% |
| 19 |   15 |  11,581,000 |  1.9% |  0.9% |

_total = 620,376,000 iterations, 6 priority levels._

**관찰**: 측정 share가 Linux 가중치 기대치를 전 구간에서 단조로 추종한다
(고priority일수록 더 많은 CPU). 낮은 가중치 끝(prio 12~19)에서 측정값이 기대보다
약간 높은 것은, 아무리 낮은 우선순위라도 CFS가 starvation을 막기 위해 최소한의
스케줄링 턴을 보장하기 때문(`cfs_min_vruntime` 기반)으로 일관된다.

**문서화된 이슈(§11.8)와의 연결**: `priority_test` Test 3은 중간대(1~19)에서
*finish-order*로는 분리를 관측하기 어렵다고 기록돼 있었다. 위처럼 **고정 wall-clock
점유율**로 측정하면 중간대까지 깨끗이 분리되고 기대치와 일치한다 — 즉 가중치
매핑 자체는 정상이며, Test 3의 *측정 방식*(짧은 burn + 종료 순서)이 분해능이
낮았던 것이다. cfs_bench는 share 기반이라 이 한계를 우회한다.

---

## 2. LLM 응답 캐시 hit-rate

`eval cache 50` — 동일 키 50개를 cold 라운드(전부 신규=miss)와 warm
라운드(전부 재조회=hit)로 2회 조회.

| 라운드 | 조회 | 결과 |
|---|---:|---|
| 1 (cold) | 50 | miss 50 / hit 0 |
| 2 (warm) | 50 | miss 0 / hit 50 |
| 합계 | 100 | **hit-rate 50%** |

cold 라운드의 전미스 + warm 라운드의 전히트는 16-슬롯 RAM 테이블 + `/cache.bin`
디스크 오버레이가 의도대로 동작함을 보인다(50개 키가 RAM 용량을 넘겨도 디스크
promote로 모두 hit). 실 사용에서 hit는 Solar API 왕복을 생략하므로, hit-rate가
곧 절감된 LLM 호출 비율이다.
