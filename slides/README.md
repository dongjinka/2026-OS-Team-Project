# Presentation Slides (English)

Deliverable #4 — the English presentation deck. This is the working outline; the deck
itself is built from the content under [`../docs/`](../docs/) and exported into this folder.

## Outline

1. **Title** — OS for LLM: an agent runtime on xv6-riscv (Direction A · team · 2026 OS)
2. **Problem** — let an LLM operate a real OS safely; the AIOS three components
3. **Architecture** — human vs. LLM data path; `deny-list → jail → confirm-escape` (README §1)
4. **OS concepts** — CFS scheduling, syscalls, sandbox, synchronization, IPC, file system (README §2)
5. **Demo** — kernel cache hit, confirm-escape gate, jail denials (README §5)
6. **Evaluation** — 65/65 regression, CFS share vs Linux weights, 50% cache hit-rate ([../docs/BENCHMARKS.md](../docs/BENCHMARKS.md))
7. **Security** — 3-layer defense + audit (3 fixed / 2 open) ([../docs/SECURITY_AND_EVALUATION.md](../docs/SECURITY_AND_EVALUATION.md))
8. **Limitations & future work** — host-side JSON parsing, F10 out of scope (README §10)
9. **Roles & wrap-up**

## Source material

- [Technical_Report.md](../docs/Technical_Report.md)
- [Development_Process.md](../docs/Development_Process.md)
- [BENCHMARKS.md](../docs/BENCHMARKS.md)
- [SECURITY_AND_EVALUATION.md](../docs/SECURITY_AND_EVALUATION.md)

Export the final deck (PDF / PPTX) into this folder.
