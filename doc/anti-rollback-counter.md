# Hardware monotonic counter and the anti-rollback design decision

**[中文](anti-rollback-counter.zh-CN.md)**

This document answers two questions that come up regularly: why QBOOT's
anti-rollback does (or does not) need an OTP hardware monotonic counter, and
why QBOOT deliberately does **not** implement the common A/B slot swapping.
Both decisions follow from the same device threat model; read it together with
`doc/secure-boot.md`.

## 1. Where the anti-rollback floor lives today

The anti-rollback floor is persisted in the **wear-levelled META log**
(`quilla-qboot-meta.c`, ordinary rewritable flash). The firmware security
version is baked into the image (`.quilla_secver`, covered by the signature).
The firmware security version is set by the APP-side compile-time macro
`QUILLA_APP_SECURITY_VERSION`, defined in `os/sys/quilla-version.h` (default
`1U`); `QUILLA_APP_SECVER_PLACE()` in `quilla-app-secver.h` pins it into the
`.quilla_secver` section at `APP_START+0x400` (PY32F040xB: `0x08007400`), which
is covered by the image signature. The version is part of the signed image:
carried by the image itself, neither assigned by QBOOT nor sent by the host. To
change it, pass `-DQUILLA_APP_SECURITY_VERSION=<n>` when building the APP (QAPP)
only — rebuild the APP and re-sign; the QBOOT image is untouched (Keil: append
the macro to the QAPP project's `<Define>`).
and both the boot gate and the upgrade path compare

    version >= max(META floor, OTP counter)

where the OTP counter exists only when the hardware monotonic counter is
enabled (below). Because META is rewritable, the current mechanism cannot stop
two physical attacks:

1. **Replay**: erase the META region and write back any **historically signed**
   older manifest+image pair — the signature check passes.
2. **Key-holder rollback**: an authorized-key end user lowers the version and
   re-signs (the Layer 2 self-check is obfuscation only, see
   `doc/secure-boot.md` §1.1).

## 2. Hardware monotonic counter (OTP)

### 2.1 Design

- **Storage**: the MCU's OTP area (program once, never erasable). Each counter
  unit is one OTP byte; burning it (0xFF → 0x00) raises the counter by 1, and
  the counter value IS the minimum firmware security version every future
  image must meet.
- **Integration point**: `quilla_qboot_secure_min_version_get/set()` ratchet
  the META floor and the OTP counter together; the boot gate uses the larger
  of the two. Burn failures are handled best-effort — the signature check
  remains the primary gate, and a failed OTP write must not render the
  device unbootable.
- **Water level and fallback**: OTP capacity is finite. Before burning, the
  remaining capacity is checked: if the target fits, burn; otherwise burn the
  remaining cells to the top, then burn the **exhaustion marker** (the last
  byte of the region, read-back confirmed) and fall back to the META flash
  floor as the sole carrier. Already-burned counter bytes stay valid forever.
- **Build switches** (QBOOT build):
  - `SOC_OTP_AVAILABLE` (`arch/cpu/<arch>/<mcu>/soc.h`, MCU hardware property):
    GD32F502VE = 1 (OTP2 data block, 256 B @ `0x1FF20000`); PY32F040xB = 0
    (no OTP area in the manual, but configurable write protection exists,
    §2.4); CH32V305 = 0 (the 128-byte user selection-word area is rewritable
    flash that a full-chip programming erases — not OTP, §2.4).
  - `QUILLA_QBOOT_HW_COUNTER_LEVEL` (feature LEVEL, defined in Quilla's
    QBOOT policy header `os/services/quilla-qboot/quilla-qboot-secure.h`,
    NOT in the per-MCU `soc.h` and NOT in the build projects): one of three values —
    `QUILLA_QBOOT_HW_COUNTER_NONE` (no hardware counter; the floor lives solely
    in the rewritable META flash log), `QUILLA_QBOOT_HW_COUNTER_WPP`
    (a write-protected, programmable main-flash sector (NV counter), `SOC_WPP_AVAILABLE`),
    `QUILLA_QBOOT_HW_COUNTER_OTP` (true OTP, `SOC_OTP_AVAILABLE`). **The default
    level is derived in `quilla-qboot-secure.h` from the capability declared in
    `soc.h`**, with no project file or command-line `-D` needed: WRP-only parts
    (PY32F040xB, CH32V305RB) derive WPP; parts with true OTP (GD32F502VE)
    derive OTP while the validation switch `QUILLA_QBOOT_CONF_VALIDATE_TRUE_OTP`
    is 1, and NONE once it is set back to 0 for production (true-OTP cells are
    irreversible). Change the level by editing the policy header, not by
    passing `-D` from Keil/EIDE/Makefile. `soc.h` only declares the hardware
    capability (`SOC_OTP_AVAILABLE` / `SOC_WPP_AVAILABLE` and their
    BASE/SIZE/CELL_SIZE). The internal enable macro
    `QUILLA_QBOOT_OTP_COUNTER` is derived from that level and the hardware,
    and must not be defined directly by a port.
  - **Level and warnings** (single point in `quilla-qboot-otp.c`, exactly once
    per build):
    - NONE → `QBOOT: HW counter disabled (LEVEL=NONE)`, plus the highest
      level this MCU CAN configure (`QBOOT: LEVEL=OTP available on this MCU`
      or `QBOOT: LEVEL=WPP available on this MCU`; if neither carrier exists,
      `QBOOT: no counter carrier on this MCU`).
    - OTP level but no true OTP hardware → `QBOOT: LEVEL=OTP selected but this
      MCU has no OTP`; if the MCU also has a WRP sector, also
      `QBOOT: use LEVEL=WPP instead`.
    - WPP level but no write-protect sector → symmetric warning
      (`QBOOT: use LEVEL=OTP instead`).
    - level matches hardware → silent.
  - **Partition overlap and automatic APP shrink**: selecting the WPP level
    (write-protected NV counter) reserves the top WRP sector of main flash, so
    `QUILLA_QBOOT_APP_END` in `quilla-qboot-hal.h` **auto-shrinks to the
    counter-region base when the WPP level is selected** (PY32F040xB →
    `0x0801E000`, CH32V305RB → `0x0001F000`),
    and the repository's GCC linker scripts, WCH linker scripts and Keil
    scatter files (`.sct`) have been shrunk to match. **Integrators using the
    repository's own build configurations (Makefile / EIDE / Keil
    `QAPP.uvprojx` + `QAPP.sct`) need not adjust the partition by hand**; only
    a custom build chain must mirror the same base. If a chain is not shrunk
    while the switch is on, the build warns
    "QBOOT: APP_END overlaps WPP counter sector; shrink APP_END". Once the hardware write protection is in
    effect, any QIAP erase/write of that sector fails loudly (WRPERR/WRPRTERR)
    — the failure is explicit and detectable, never a silent corruption.
  - **APP image size limit**: with the WPP level (write-protected NV counter) selected, the **end address
    of the APP image must not exceed the counter-region base**
    (PY32F040xB = `0x0801E000`, CH32V305RB = `0x0001F000`). Because the APP
    partition already auto-shrinks to that base, images built from the
    repository configurations are naturally within bounds; QIAP erases only
    the sectors covered by written data, so an image below the limit upgrades
    normally, while one extending into the counter sector fails on erase due
    to the write protection. The factory packaging tool `qboot_image.py`
    enforces this: when the target QBOOT has the WPP level selected, pass
    `--otp-counter-base <base>` to `pack`/`verify` and the tool rejects
    over-limit images (do not pass it for a QBOOT built without the WPP level —
    the sector is then usable by the APP). GD32 true OTP lives outside the
    flash partition, so nothing needs to be passed there.

### 2.1.1 Configuration method (level defined at the system level in quilla-qboot-hal.h)

The feature level is lifted out of the per-MCU `soc.h` into the system-level
**`os/services/quilla-qboot/quilla-qboot-hal.h`** (default derived from the
`soc.h` capability), so GCC and Keil share one source of truth and the same MCU
behaves identically under both compilers; `soc.h` keeps only the hardware
capability declarations.

- **Default level derived by hal.h from the capability**:

  | MCU | System-level default (derived in hal.h) | Meaning |
  |---|---|---|
  | PY32F040xB | derives WPP (WRP-only, no true OTP) | write-protected NV counter enabled by default |
  | CH32V305RB | derives WPP (same) | write-protected NV counter enabled by default |
  | GD32F502VE | derives NONE (has true OTP, but expensive/irreversible, off by default) | true OTP off by default, enable explicitly at build time |

- **How to disable (opt-out, i.e. select NONE)**: define
  `QUILLA_QBOOT_HW_COUNTER_LEVEL` as `QUILLA_QBOOT_HW_COUNTER_NONE` in
  `quilla-qboot-secure.h` (PY32/CH32 derive WPP by default). Suited to shipping
  forms where "the device exposes no external programming interface and the
  Flash-based floor is sufficient" (see §2.3).
- **How to enable true OTP (GD32)**: set `QUILLA_QBOOT_CONF_VALIDATE_TRUE_OTP`
  to 1 in `quilla-qboot-secure.h`. True-OTP cells are one-time programmable and
  consumed irreversibly; set it back to 0 for production (M33 LTO code size
  also constrains SEALED/SECURED variants — see §2.1.1).
- **No manual APP-partition change**: with the write-protected NV counter on,
  `quilla-qboot-hal.h` pulls `QUILLA_QBOOT_APP_END` down to the counter base,
  and the repository's `.ld` / `.sct` are already shrunk; integrators only
  need the repository build configurations. A custom build chain must mirror
  this shrink (same base as above).
- **Enforce the limit at packaging**: when invoking `qboot_image.py pack/verify`
  on the production line, if the target QBOOT has the switch on, always pass
  `--otp-counter-base <base>` so the tool rejects over-limit APP images at both
  packaging and verification.

### 2.2 Counter storage granularity

The implementations differ mainly in "how much immutable storage each +1
consumes". The table below places this design next to other common schemes
to make the byte-granular OTP trade-off clear:

| Aspect | This design (Quilla QBOOT) | Other common schemes |
|---|---|---|
| Image-side counter | security version baked at a fixed image address, covered by the signature (no separate TLV) | the security counter is often a **4-byte** uint32 placed in a protected TLV of the image |
| Hardware-side increment | OTP cells are byte-granular one-time-programmable, so **1 OTP BYTE per +1**; capacity = reserved bytes − 1 (255 on GD32F502VE) | eFuse-based ports typically burn **1 fuse BIT per +1**; capacity = available fuse bits |
| Without hardware | fall back to the wear-levelled META log (ordinary rewritable flash) | emulate an NV counter in flash, same idea |
| On exhaustion | burn remaining cells + exhaustion marker → fall back to the flash floor | headroom-aware schemes flag capacity shortfall near the limit |

**Key difference**: eFuse ports burn bit-by-bit (1 fuse bit per increment),
whereas the GD32 OTP used here is byte-granular one-time-programmable (1 byte
per increment) and cannot burn bit-by-bit — which is precisely why this
design does not reuse an eFuse counter. Capacity counts security-version
releases, and 255 is ample for most product lifetimes. Version numbers must
increment strictly by 1; skipping numbers burns one byte per skipped value.

### 2.3 Customer guidance: pros and cons of enabling

**Enabling the hardware monotonic counter yields higher security**: replay
after a META erase and key-holder rollback are both rejected by the
non-erasable OTP count.

Two boundaries to be aware of:

1. **OTP capacity is finite.** After enough upgrades the counter may exceed
   the OTP capacity — at that point QBOOT burns the exhaustion marker and
   falls back automatically, and **only flash remains as the monotonic
   counter**, i.e. the same security level as not enabling at all. Version
   numbers that skip values exhaust the capacity faster.
2. **If the device exposes no programming interface beyond the QBOOT upgrade
   function** (SWD/RDP locked in production, no ISP or debug connector), an
   attacker has no means of erasing the META region at all, and the
   flash-based floor persistence is sufficient — **enabling OTP is optional in
   that case**. Whenever SWD or a debug connector may be physically reached,
   it should be enabled.

In summary: **OTP is the only control that moves anti-rollback from
"process discipline" to "silicon physics"**; whether to enable it depends on
the device's physical exposure and the expected number of upgrades over its
lifetime.

### 2.4 Hardware characteristics of the three MCUs (OTP / write protection)

| MCU | True OTP | Configurable write protection | Decision in this project |
|---|---|---|---|
| GD32F502VE | **Yes**: OTP2 data block, 256 B @ `0x1FF20000`, byte-granular one-time programming, never erasable | not needed | true-OTP carrier; LEVEL=OTP while `QUILLA_QBOOT_CONF_VALIDATE_TRUE_OTP=1` in `quilla-qboot-secure.h` (off for production; M33 LTO — see §2.1.1) |
| PY32F040xB | none (14 KB Information area = FT info0 / option bytes / UID / system BootROM, none of which is user OTP) | **Yes**: main-flash WRP with 8 KB granularity | **write-protected NV counter implemented** (derives WPP in `quilla-qboot-secure.h`; select NONE there to disable) |
| CH32V305RB | none (128 B system config area is factory-fixed; 128 B user selection-word area is rewritable) | **Yes**: 32 write-protect sectors in main flash (4 KB each) | same as PY32, **write-protected NV counter implemented** (derives WPP in `quilla-qboot-secure.h`; select NONE there to disable) |

**PY32F040xB (per the PY32F0-series flash IP reference manuals)**:

- **Making the Information area non-writable**: the option bytes (inside the
  Information area) are by default **readable and write-protected**; writing
  them requires the OPTKEY1/2 unlock sequence to `FLASH_OPTKEYR`, and the
  `OPTLOCK` bit can be set by software to block further erase/program
  operations. A wrong sequence raises a HardFault and keeps the block locked
  until reset. The datasheet lists this as "option byte write protection,
  special unlocking design".
- **Main-flash write protection (WRP)**: applies **only to main flash** (the
  Information area is not covered by WRP); the granularity is 8 KB (on the
  128 KB part, `WRP[15:0]`, 16 bits). Protected areas reject erase and
  program; with any bit set, mass erase is disabled, and violations set
  `WRPERR`.
- **Timing constraint (critical)**: modifying option bytes is a
  "erase the whole option-byte page, then reprogram it from the registers
  (complements computed automatically)" operation, and the new values take
  effect only after **POR/BOR or `OBL_LAUNCH` (which itself triggers a
  reset)** — toggling WRP therefore always involves a reset; it cannot be
  switched silently at run time.
- **Write-protected NV counter scheme (implemented, derives WPP in `quilla-qboot-secure.h`;
  select NONE there to disable)**: a dedicated WRP sector at the top
  of main flash serves as the counter region, with the lifecycle: **write
  protection is lifted only inside the window in which QBOOT programs the
  counter, and restored immediately afterwards; after programming, and while
  the APP runs, write protection stays on**. Implementation notes
  (`quilla-qboot-otp.c` + the per-MCU `quilla-qboot-hal.c`):
  - **Selecting NONE = zero cost**: the whole feature compiles to a no-op —
    no extra resets, no option-byte wear, no flash reserved (only a few
    derived constants in hal.h).
  - **Cell granularity follows the programming primitive**: GD32 OTP = 1
    byte (255 versions); PY32 = one 256-byte page (32 pages − 1 exhaustion
    marker = 31 versions); CH32 = one 32-bit word (1024 words − 1 marker =
    1023 versions).
  - **Flow and cost**: QIAP SIGN only raises the META floor;
    `quilla_qboot_otp_sync()` burns the cells at the next boot — if
    `counter < floor` and the sector is protected, lift the protection
    (option-byte rewrite + OBL_LAUNCH reset); in the open window burn the
    cells one by one with read-back confirmation, then re-protect (another
    reset). **Two extra resets per upgrade**, and only when the WPP level is
    selected.
  - **Idempotent and power-loss safe**: every boot resumes from the scan; an
    interrupted burn wastes at most one cell. The protection state is judged
    from the STORED option-byte configuration (CH32 reads the raw selection
    words, not a mirror register), which avoids a reset loop of repeated
    unlocks when the reload has not happened yet.
- **Security annotation (must be stated to customers as-is)**: WRP/OPTLOCK is
  **configurable hardware, not an OTP physical guarantee** — it stops the APP
  and runaway code from erasing or writing the counter area, but it does not
  stop code that holds the option-byte unlock capability or an external
  programmer (an attacker who can reconfigure WRP can likewise reset the
  counter). Security level: above a bare flash counter, below true OTP — the
  same tier as the common flash-simulated NV counter (as seen on STM32-class
  parts). Note also that
  every option-byte change erases and rewrites the whole page, and the
  option-byte page endurance is lower than main flash; frequent upgrades
  need an endurance assessment.
- **APP image limit once the write-protected NV counter is enabled** (the partition start/end
  addresses themselves are unchanged, see §2.1): for PY32F040xB the APP image
  end must stay ≤ `0x0801E000` (effective capacity 92 KiB, down from 100 KiB
  by the top 8 KiB counter sector); for CH32V305RB ≤ `0x0001F000` (effective
  capacity 124 KiB). `qboot_image.py pack/verify` enforces this on both the
  packaging and the verification side via `--otp-counter-base`.
- **Why "just write-protect the META partition" is not a substitute for the
  counter sector**: address-wise it would work (META at
  `0x00006000–0x00006FFF` is exactly one 4 KB WRP sector), but the scheme
  fails functionally —
  ① the QIAP upgrade flag lives in the same 4 KB as the manifest and the
  floor; a permanent protection also blocks QBOOT's own writes (`swqboot`
  breaks, QIAP commit breaks, the floor ratchet breaks), and keeping the
  feature alive would require an unlock/re-protect reset cycle per upgrade —
  no cheaper than the counter-sector scheme;
  ② the WRP bits themselves live in the rewritable user selection words; an
  attacker who reprograms the option bytes can clear the protection and then
  erase META and replay an old image as usual — the protection strength is
  the same tier as the counter sector, with zero added benefit;
  ③ the counter sector is written exclusively by QBOOT, only after SIGN and
  before the boot gate, with minimal write traffic — well suited to permanent
  protection; META is an actively written area on every upgrade and is a
  poor fit for it.

**CH32V305RB (per the CH32V303/305/307 datasheet and the WCH official
forum)**:

- **"No true OTP" stands**: the 128-byte system non-volatile configuration
  area is factory-fixed and not user-modifiable; the 128-byte user-defined
  information area (user selection words @ `0x1FFFF800`) consists of a 16-byte
  structure plus roughly 112 bytes of user data, programmed as halfwords with
  each data byte stored alongside its one's complement, and rewritten by
  read-modify-write of the whole region — **rewritable flash, not OTP**. A WCH
  community blog, "CH32V20x_30x series user selection-word area application
  and operation", demonstrates that this area can be read-modify-written after a
  FLASH unlock (the whole region is erased and the 16-byte structure rewritten
  first), confirming "rewritable, not OTP"; note that the blog operates on the
  remaining 112-byte user-data area of the selection words, which is a different
  carrier from this design's WRP main-flash sector, but it likewise shows the
  selection words can be reconfigured — supporting the "configurable hardware,
  not physical OTP" security annotation above.
- **Write-protectable region**: main flash is divided into 32 write-protect
  sectors (`FLASH_WRProt_Sectors0..30`, sectors 31–127 share one bit); the
  protection bits live in the user selection words and take effect at
  power-on / option-byte reload (`FLASH_EnableWriteProtection()`). The
  mechanism is the same tier as PY32's: **configurable hardware**, and the
  security annotation above applies unchanged.

## 3. Why QBOOT does not implement A/B swap

The common A/B slot design with automatic revert (swap & revert) is often seen
as the "modern" default. QBOOT deliberately does not implement it — for security
reasons, not cost:

The A/B swap semantics are "run the new image from slot B on trial; on failure
revert to slot A; the device **keeps running** the whole time". That provides
an attacker with an environment invisible to day-to-day operation for repeated
experimentation: tampered, decompiled and modified firmware can be attempted
**over and over** — each failure reverts silently and the device carries on
normally, so **the device administrator never notices, in day-to-day operation,
that the device is under continuous attack**. A/B absorbs the cost of each
failed experiment.

QBOOT's single-slot model turns the same attack into an **explicit failure**:
no
image that fails signature verification ever reaches the boot path, the device
stays in the QBOOT shell (serial QIAP recovery remains available), and every
tampered upgrade attempt is a visible event. For a safety-critical device,
"visible failure" beats "silent self-healing". Upgrade-interruption
reliability is already handled by the power-fail-resumable QIAP flag
(`flag=QIAP`) and the dual-slot META records; dual partitions are not needed
for that.

## 4. Verification status

- QBOOT builds pass on all three MCUs: GD32F502VE (CHECK/SEALED/SECURED variants),
  PY32F040xB and CH32V305RB (GNU builds).
- The warning matrix of §2.1 was verified in the actual builds; the
  define-enabled builds also pass with the write-protected NV counter code fully linked
  (QBOOT size: PY32 22 108 B, CH32 21 951 B, GD32 20 272 B — all inside the
  24 KB partition).
