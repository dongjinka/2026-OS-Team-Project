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
| Security audit & fixes | [`SECURITY.md`](SECURITY.md) §1 — EN overview | [`SECURITY.md`](SECURITY.md) §2 — full finding register #1–#13 (KR) |
| Quantitative benchmarks | [`BENCHMARKS.md`](BENCHMARKS.md) — raw CFS / cache numbers | `Technical_Report.md` §2 / §6 — method |
| Demo media | [`assets/README.md`](assets/README.md) — index of the 8 PNG captures inlined by the root READMEs / Technical Report | `assets/*.png` — `solar-pro2` live runs (2026-06-08) |

Reproduce the above with the red-team harnesses (`tools/sec_audit.py`,
`tools/sec_wire.py`) and the benchmark harness (`tools/bench_report.py`).
Findings #1 / #3 / #4 are **fixed** (team review, PR #13/#14) and the harnesses
now report `SAFE`. A second full-codebase audit (2026-06-09) fixed three more —
#10 (deny-list bypass), #11 (cache wire-forgery), #12 (re-jail leak). Still open:
#2 (cache jail-root, half-mitigated) and #5 (deny-list SPAWN). Full register: [`SECURITY.md`](SECURITY.md) §2.

## Korean reference docs (repo root)
- [`Implementation.md`](../Implementation.md) — module-level code reference (F1–F9, file:line)
- [`Project_Guide.md`](../Project_Guide.md) — educational walkthrough + debugging history
- [`CHANGELOG.md`](../CHANGELOG.md) — change ledger
- [`Project_requirements.md`](../Project_requirements.md) — the course assignment spec
