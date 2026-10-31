# Coding Guidelines

**English** · [中文](coding-guidelines.zh-CN.md)

Quilla uses **MISRA C:2012** as its coding guideline for the code it authors. This page states what that means here, what it does not mean, and where the boundary lies — so integrators can tell which parts of the tree are held to the guideline and which are not.

## 1. What this is, and what it is not

**It is** a statement of the coding rules applied to Quilla's own source, and of the scope those rules cover.

**It is not** a claim that Quilla is MISRA C:2012 compliant, and it is not a safety or certification claim of any kind. The project does not hold a compliance statement, a deviation register, or a tool-qualification record in this public repository. Rules below are applied as engineering discipline; coverage is partial and is not audited for public release.

## 2. Scope

### In scope

Code authored by this project: the kernel and system services (`os/sys/`, `os/services/`), the Quilla-specific Arm and RISC-V layers (`arch/cpu/`, files carrying the `quilla-` prefix), and the per-MCU project trees under `user/<mcu>/`.

### Out of scope, deliberately

| Source | Reason |
|---|---|
| Upstream Contiki-NG (`os/net/**`, `arch/dev/**`, and other inherited trees) | Merge-cleanliness: rewriting it would have to be redone on every upstream merge. Treated as pre-existing software. |
| CMSIS and vendor HAL/SDK (PUYA, GigaDevice, WCH) | Treated as pre-existing software; local edits would break device-library traceability. |
| Build, test and verification scripts (Python, shell) | Not part of the shipped firmware. |

Exclusions are stated rather than left implicit, because a silent exclusion and a completed review look identical in a report.

## 3. Rules that shape the code

The ones most visible when reading the tree:

- **No dynamic memory allocation.** `malloc`/`free` and friends are removed at the link level; the allocator entry points are dead stubs. Stack watermark checking is used instead of heap usage.
- **No recursion.** Call depth is bounded so static stack sizing stays meaningful.
- **Fixed-width types** (`stdint.h`) throughout; no reliance on implicit integer promotion widths.
- **Single, explicit control flow** in security-relevant paths — in particular the boot handover sequence (NVIC clear, DMA stop, VTOR/MSP set, interrupt enable, then jump) is fixed and order-dependent.
- **Verify after write.** Flash programming is read back and compared, because the vendor HAL does not report non-erased-bit failures.
- **Explicit failure paths.** Security checks fail closed: an unverified image does not boot.

## 4. How conformance is pursued

Per change: manual review against the guideline, plus mechanized pattern checks over the rule subset that lends itself to text and AST matching (for example forbidden constructs, missing `default` clauses, and switch fall-through).

Not yet in place, and therefore not claimed: a qualified static analyzer covering the full rule set, a maintained deviation register, and an independent review role. Without those, a compliance statement would be unsupported, so none is made.

## 5. If you derive from this code

- Hold your **own** code to the same guideline; the in-scope boundary above is a statement about this repository's authorship, not a permission to treat inherited code as reviewed.
- Keep the upstream and vendor trees unmodified where possible — that property is what keeps merges cheap.
- Do not read the absence of a compliance claim as a defect report; read it as the current, accurate state.
