# Documentation Index — LLM-OS (Direction A: OS for LLM)

An agent runtime on **xv6-riscv** that hosts, schedules, sandboxes, and caches
an LLM (Upstage Solar). Start from the root entry point:
[`README.md`](../README.md) (EN) / [`README.ko.md`](../README.ko.md) (KR).
This folder holds the detailed reports and the security/evaluation evidence.

## Graded deliverables
| # | Deliverable | Where |
|---|---|---|
| 1 | Application + source + how-to-run | root [`README.md`](../README.md) (EN, primary) + [`README.ko.md`](../README.ko.md) (KR) + repository |
| 2 | Technical Report (EN) | [`Technical_Report.md`](Technical_Report.md) |
| 3 | Development Process (EN) | [`Development_Process.md`](Development_Process.md) |
| 4 | Presentation slides (EN) | `slides/` *(to be added)* |

## Security & Evaluation (companion docs)
| Topic | Start here | Full detail |
|---|---|---|
| Security audit & fixes | [`SECURITY_AND_EVALUATION.md`](SECURITY_AND_EVALUATION.md) — EN summary | [`SECURITY_AUDIT.md`](SECURITY_AUDIT.md) — full audit (KR) |
| Quantitative benchmarks | [`BENCHMARKS.md`](BENCHMARKS.md) — raw CFS / cache numbers | `Technical_Report.md` §2 / §6 — method |

Reproduce the above with the red-team harnesses (`tools/sec_audit.py`,
`tools/sec_wire.py`) and the benchmark harness (`tools/bench_report.py`).
Findings #1 / #3 / #4 are **fixed** (team review, PR #13/#14) and the harnesses
now report `SAFE`; #2 (cache jail-root) and #5 (deny-list SPAWN) remain open.

## Korean reference docs (repo root)
- [`Implementation.md`](../Implementation.md) — module-level code reference (F1–F9, file:line)
- [`Project_Guide.md`](../Project_Guide.md) — educational walkthrough + debugging history
- [`CHANGELOG.md`](../CHANGELOG.md) — change ledger
- [`Project_requirements.md`](../Project_requirements.md) — the course assignment spec
