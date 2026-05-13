# Week 09 Project Session — Team Building & Topic Introduction

Starting this week, the third hour ("lab") becomes a **Team Project session** that runs through Week 14. This document covers (1) team formation rules, (2) the project theme, and (3) how to get started with the Upstage API.

---

## 1. Team Formation

- **Team size: 4 students** — this is the default and strongly preferred configuration.
- **Exception:** if total enrollment does not divide evenly into 4, **at most one team of 3** is permitted. All other teams must have 4 members.
- Teams are self-organized during this session. If you cannot find a team, raise your hand — the instructor will help pair you up.
- Each team chooses a **team lead** (single point of contact) and a **team name**.
- **Deliverable by end of class:** one team-roster row submitted to the shared sheet:
  - Team name, team lead, 3–4 member names + student IDs + contact
  - **Team GitHub repository URL** (Public — see §3 for repo requirements)
  - One-line preliminary direction (OS-for-LLM or LLM-for-OS + rough idea)

---

## 2. Project Theme — **LLM + OS**

The project asks you to explore the intersection of **Large Language Models** and **operating systems**. You pick **one** of the two directions below (you may also propose a hybrid — clear it with the instructor first).

### Direction A — **OS for LLM**
Build an OS, runtime layer, or agent platform that **hosts, serves, or orchestrates** LLMs. Examples of scope:
- An agent runtime in the spirit of **Openclaw**-style coding agents (tool-use sandbox, file/shell tool permissions, memory management, process isolation)
- Scheduling or memory policies tuned for LLM inference workloads (batching, KV-cache eviction, GPU/CPU hand-off)
- A mini-OS or userspace supervisor that manages multiple concurrent LLM "processes" (agents) with fair CPU/memory/tool-quota allocation
- A secure execution sandbox that the LLM can call into to run generated code

### Direction B — **LLM for OS**
Integrate an LLM **into** the OS or a classical OS problem. Examples of scope:
- A natural-language shell / command assistant that translates intent → system calls
- LLM-assisted **diagnosis** of OS state (log triage, deadlock hypothesizer, crash dump explainer)
- LLM as a **hint oracle** for a classical OS mechanism (paging, scheduling, file prefetch) — the LLM proposes hints, the OS mechanism decides whether to follow them
- Self-repairing configuration: an LLM that reads `dmesg` / `/proc` / configs and suggests or applies fixes
- An LLM-guided installer, package troubleshooter, or user-facing recovery tool

> **Mandatory constraint — OS concepts must be present.** Every project, regardless of direction, **must incorporate operating-system concepts from this course as a substantive part of the design and implementation** — at minimum one (preferably more) of: processes, threads, synchronization (locks/semaphores/monitors), scheduling, virtual memory / paging, file systems / storage, IPC, system calls. The OS component must be something **you design and implement**, not merely something the LLM is hosted on. Thin wrappers around an LLM API — or projects where the OS angle is only "it runs on Linux" — **do not qualify** and will not be accepted.

---

## 3. Code Repository Requirements

- Each team must create a **public GitHub repository** and submit its URL with the team roster.
- The repository **README** must include:
  - One-paragraph project summary + which direction (A or B)
  - Tech stack overview
  - **Setup instructions** (dependencies, environment variables, how to obtain Solar API key — but do **not** commit the key)
  - **How to run** the application (commands, expected entry points)
  - **Demo screenshots and/or a short demo video/GIF**
- All code, reports, and artifacts are pushed to this repo throughout the project.

---

## 4. LLM Backend — Upstage **Solar Pro 3**

- The project uses **Upstage Solar Pro 3** as the LLM backend, accessed via the **Upstage API**.
- API keys will be provided by the instructor (per team; do not commit keys to git).
- If you also want personal access for prototyping, you can apply through Upstage's program at <https://www.upstage.ai/events/ai-initiative-2025-ko>.
- **Docs:** https://console.upstage.ai/docs  
  (Chat Completions endpoint, model IDs, rate limits, and examples are all there. Solar models are OpenAI-API-compatible, so existing `openai` SDK code works by swapping the `base_url` and `api_key`.)

### Optional supplemental API — NVIDIA NIM
- Applying for the **NVIDIA NIM (NVIDIA Inference Microservices)** program gives free (somewhat slow) access to several model APIs.
- Useful when you need capabilities beyond Solar — e.g., embeddings for a retrieval/log-search component, an auxiliary smaller LLM for cheap classification, or multimodal models for screenshot/diagram input.
- Optional. Apply only if your design actually needs it.

### Development tool support — Claude Code 1-month license
- We will distribute **one 1-month Claude Code license per team**.
- Use it actively for coding, refactoring, debugging, kernel-level reading, test writing, and documentation.
- Distribution method, schedule, and account-registration steps will be announced separately. Each team should designate one representative to receive the license.

---

## 5. Final Deliverables

By the end of Week 14, the following four items must all be ready in the team's GitHub repository.

| # | Deliverable | Description |
|---|-------------|-------------|
| 1 | **Application** | A working product + complete source code in the team's public GitHub repository. README must cover setup, how-to-run, and demo. |
| 2 | **Technical Report** | System architecture (block diagram), tech stack, **which OS concepts are in play and where**, how the LLM is integrated, key implementation details, and limitations. |
| 3 | **Development Process Document** | Planning → scheduling → execution → retrospective. Includes meeting notes, weekly progress per role, issues encountered, and how they were resolved. |
| 4 | **Presentation Slides** | Slides for the final Week 14 presentation. **In English.** |

---

## 6. Timeline (tentative)

| Week | Project milestone |
|------|-------------------|
| 9    | Team formation, direction picked, one-paragraph proposal |
| 10   | Problem statement + system sketch (block diagram, which OS concept is in play) |
| 11   | Minimal working prototype (LLM call works end-to-end, OS component stubbed) |
| 12   | Integrated prototype + at least one evaluation metric defined |
| 13   | Evaluation results + dry-run of the final presentation |
| 14   | **Final presentation** (Professor 15% + Peer review 15%) |

> **Presentation language:** both the slides and the spoken presentation must be in **English**.

---

## Checklist — Before End of Week 09

- [ ] Team formed (4 members; at most one team of 3) and roster submitted
- [ ] Team name and team lead chosen
- [ ] **Public GitHub repository** created and URL submitted with the roster
- [ ] Direction picked (A: OS-for-LLM, or B: LLM-for-OS) and one-paragraph direction drafted for Week 10
- [ ] Three core features / scope bullets identified
- [ ] OS concept(s) the project will exercise identified (processes / threads / sync / scheduling / memory / storage)
- [ ] Team representative ready to receive the **Upstage Solar API key** (from instructor)
- [ ] Team representative ready to receive the **Claude Code 1-month license** (announcement to follow)
- [ ] (Optional) NVIDIA NIM applied for, if the design needs embeddings / auxiliary / multimodal models
