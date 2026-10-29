# Upstream Contiki-NG documentation

Everything under `doc/upstream/` is inherited verbatim from the upstream
[Contiki-NG](https://github.com/contiki-ng/contiki-ng) project. It describes
**Contiki-NG, not Quilla**. It is kept because the parts of Quilla that still
build on Contiki-NG (build system, network stack, platform infrastructure)
remain documented there.

Quilla's own documentation lives directly under `doc/` — start at
[`doc/Home.md`](../Home.md).

| Directory | Content |
| --- | --- |
| `getting-started/` | Toolchain installation, Docker / Vagrant, build & configuration system |
| `programming/` | Processes, timers, RPL, TSCH, CoAP, IPv6, Coffee, Antelope, … |
| `tutorials/` | Step-by-step Contiki-NG tutorials |
| `platforms/` | Upstream boards (cc2538dk, nrf, simplelink, sky, …) — **not** Quilla's MCU ports |

## Caveats

- `platforms/` describes upstream hardware. Quilla's supported MCUs are
  documented in `doc/cmsis-layout.md`, `doc/port-layout.md` and the per-MCU
  build guides instead.
- Files inside `doc/project/` whose name starts with `CONTIKI-` are likewise
  upstream project material (roadmap, release notes, organisation, code style,
  contribution flow) — they describe the upstream project, not Quilla.
- Where an upstream document and a Quilla document disagree, **Quilla's own
  documentation wins**.
