# NOTICE — vendored WCH vendor SDK

The `Peripheral/`, `Core/`, `Startup/`, `system_ch32v30x.*` and `Debug/`
components under this directory are WCH's vendor SDK, kept with their own
upstream headers untouched.

Usage scope (project ruling): the WCH source is used **only for WCH chips**
(the CH32V305VB). It carries no copyleft / viral licence, so including it
does not affect Quilla's own BSD-3-Clause code. Quilla only references these
files and adds no copyright or licence claim of its own over them.

Quilla's own port code in this tree is BSD-3-Clause (see the repository
`LICENSE`). Full detail: `doc/riscv-ch32v305-build.md` §2.
