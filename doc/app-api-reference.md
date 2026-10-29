**[English](app-api-reference.md) · [中文](app-api-reference.zh-CN.md)**

# Quilla OS-Layer Public API Reference (for Application Developers)

> This is the only OS-layer API manual an application developer needs in order to implement product features on the Quilla system.
> Companion reading: `doc/qboot-app-architecture.md` (dual-partition architecture), `doc/upstream/programming/Processes-and-events.md`, and `doc/upstream/programming/Timers.md` (Contiki process and timer programming guides).

## 1. Audience and Design Principles

**Audience**: application developers who call system APIs in `user/<mcu>/Src/main.c` and other APP sources. The scope is limited to which function to call, what parameters to pass, and how to use it; OS scheduling and accounting internals are out of scope.

**Design principles**:

1. **Public API only.** The functions, macros, and structs listed here are stable interfaces exported from the public headers of `os/` and `arch/`. An APP must not access kernel internal data structures directly, nor rely on symbols not listed here.
2. **APP decoupled from OS internals.** Timers, processes, shell, watchdog, and low-power management are all exposed through unified interfaces. When switching MCUs (e.g., PY32F040xB ↔ GD32F502VE), APP code needs no changes as long as the same-named public APIs are used.
3. **QBOOT internals out of scope.** `quilla-qiap`, `quilla-qboot-hal`, `quilla-qboot-flag`, etc. belong to the bootloader side. This document covers only the APP-side support for remote upgrade: `quilla_app_qboot_init`, the `swqboot` command, and `shell_set_partition_tag("app1")`.

All interfaces returning `kern_status_t` use the unified status codes of `os/sys/kern-status.h`: `KERN_OK` (0, success), plus `KERN_ERR_NULL / KERN_ERR_ALREADY / KERN_ERR_NOT_READY / KERN_ERR_FULL / KERN_ERR_RANGE / KERN_ERR_CONTEXT / KERN_ERR_INTERNAL / KERN_ERR_STACK / KERN_ERR_CLOCK`. Return values may be ignored without affecting compilation; production code should still check critical paths such as `qwdt_*` and `qclk_*`.

---

## 2. Process Model and Events (Contiki process)

Every background task of an APP is a **process** (coroutine/protothread): not a thread, but a lightweight coroutine driven by kernel events. It yields the CPU at `PROCESS_WAIT_*` / `PROCESS_YIELD` and resumes at the yield point when an event arrives.

> Source files: `os/sys/process.h`, `os/sys/autostart.h`

### Quick Reference

| API | Description |
|-----|-------------|
| `PROCESS_THREAD(name, ev, data)` | Defines the function body of a process |
| `PROCESS(name, strname)` | Declares and initializes a process variable (in a C file) |
| `PROCESS_NAME(name)` | Forward-declares a process name in a header file |
| `PROCESS_BEGIN()` / `PROCESS_END()` | Fixed macros at the start/end of a process body |
| `PROCESS_YIELD()` | Yields the CPU; resumes when the next event arrives |
| `PROCESS_WAIT_EVENT()` | Waits for any event (equivalent to `PROCESS_YIELD`) |
| `PROCESS_WAIT_EVENT_UNTIL(c)` | Waits for an event and continues only once condition `c` holds |
| `PROCESS_EXIT()` | Exits the current process |
| `PROCESS_PAUSE()` | Yields for a moment so other processes run first |
| `PROCESS_CURRENT()` | Gets the pointer to the currently running process |
| `PROCESS_BROADCAST` | Broadcast event target (delivered to all processes) |
| `process_start(p, data)` | Starts a process |
| `process_post(p, ev, data)` | Asynchronously posts an event to a process (or broadcast) |
| `process_poll(p)` | Requests an immediate poll of a process (can be called in an ISR) |
| `process_alloc_event()` | Allocates a global event number (>127) |
| `AUTOSTART_PROCESSES(...)` | Declares the list of processes started automatically at power-on |
| `autostart_start(list)` | Starts the given list of processes |

### Key Entries

**`PROCESS_THREAD(name, ev, data)`** — defines the process entry point.
- Purpose: declares a static process thread function; must be paired with `PROCESS_BEGIN()` / `PROCESS_END()`.
- Typical usage:
```c
PROCESS_THREAD(led_blink_process, ev, data)
{
  static struct etimer timer;
  PROCESS_BEGIN();
  etimer_set(&timer, CLOCK_SECOND);          /* see §3 Timers */
  while(1) {
    PROCESS_WAIT_EVENT_UNTIL(ev == PROCESS_EVENT_TIMER);
    /* toggle LED ... */
    etimer_reset(&timer);                     /* periodic trigger */
  }
  PROCESS_END();
}
```

**`PROCESS(name, strname)`** — creates a process instance in a `.c` file.
```c
PROCESS(led_blink_process, "LED blink");
```

**`process_start(struct process *p, process_data_t data)`**
- Purpose: starts a process. Returns `KERN_OK` on success.
- Typical usage: called during the startup phase in `main.c`, or dynamically launched from within another process. `data` is passed as the `data` argument after `PROCESS_BEGIN()`.

**`process_post(p, ev, data)`**
- Purpose: posts an event asynchronously; the target can be a process or `PROCESS_BROADCAST`. Returns `PROCESS_ERR_OK` / `PROCESS_ERR_FULL` (queue full).
- Typical usage: when an ISR or timer callback cannot do complex work directly, `process_post` a custom event to a process for handling.

**`process_poll(p)`**
- Purpose: has a process "polled" at the next scheduling point (`PROCESS_EVENT_POLL` event). Can be called in interrupt context, used to hand off ISR work to a process.

**`AUTOSTART_PROCESSES(...)` + `autostart_start()`**
- Purpose: lists the processes to be started automatically at power-on, launched uniformly by the system startup code.
- Typical usage:
```c
AUTOSTART_PROCESSES(&led_blink_process, &shell_process);
/* The system startup code calls autostart_start(autostart_processes); */
```
- Note: system processes such as `shell_process` may already be included in the platform-side `autostart` list; the APP only needs to append its own.

---

## 3. Timers and the System Clock

### 3a. System Clock (clock)

> Source file: `os/sys/clock.h`

| API | Description |
|-----|-------------|
| `clock_time_t clock_time(void)` | Current system tick count |
| `unsigned long clock_seconds(void)` | Seconds since startup |
| `void clock_set_seconds(unsigned long sec)` | Sets the seconds count |
| `clock_time_t clock_wait(clock_time_t t)` | Busy-waits for a number of ticks |
| `void clock_delay_usec(uint16_t dt)` | Busy-waits for a number of microseconds (affected by interrupts) |
| `CLOCK_SECOND` | Number of ticks per second (macro of type `clock_time_t`) |
| `clock_time_t` | Tick count type (4 or 8 bytes, set by `CLOCK_CONF_SIZE`) |

- Initialization functions such as `clock_init()` / `rtimer_init()` / `process_init()` are already called by the platform during boot; **application developers normally do not call them**.
- `CLOCK_SECOND` is the conversion basis for etimer/ctimer time parameters; for example, `etimer_set(&t, 2 * CLOCK_SECOND)` means 2 seconds.

### 3b. Event Timers (etimer)

> Source file: `os/sys/etimer.h`

An etimer posts a `PROCESS_EVENT_TIMER` event to the process that set it when it expires, and must therefore be used inside a process.

| API | Description |
|-----|-------------|
| `struct etimer` | Event timer object (requires `etimer_set` after declaration) |
| `kern_status_t etimer_set(struct etimer *et, clock_time_t interval)` | Starts the timer; expires after `interval` ticks |
| `kern_status_t etimer_reset(struct etimer *et)` | Resets with the original interval (counted from the expiry time, no drift, suitable for periodic use) |
| `kern_status_t etimer_restart(struct etimer *et)` | Restarts from the current time (drifts) |
| `kern_status_t etimer_adjust(struct etimer *et, int td)` | Fine-tunes the expiry time |
| `bool etimer_expired(struct etimer *et)` | Whether it has expired (returns true if it has fired) |
| `kern_status_t etimer_stop(struct etimer *et)` | Cancels a pending timer |
| `clock_time_t etimer_expiration_time(struct etimer *et)` | Expiry time (ticks) |
| `int etimer_pending(void)` | Whether any timer is pending |
| `clock_time_t etimer_next_expiration_time(void)` | Absolute time of the next expiry (for idle sleeping) |

- **Note**: `etimer_set` must be called in process context; a return of `KERN_ERR_CONTEXT` means it was called in an ISR, which is not allowed.
- Typical usage (periodic blinking; see the §2 example). One-shot delay:
```c
static struct etimer t;
etimer_set(&t, CLOCK_SECOND / 10);
PROCESS_WAIT_EVENT_UNTIL(ev == PROCESS_EVENT_TIMER);
/* 100 ms elapsed, do something */
```

### 3c. Callback Timers (ctimer)

> Source file: `os/sys/ctimer.h`

When a ctimer expires it **directly calls the registered C function**; no process waits for an event. Use it to run a piece of code at a scheduled time.

| API | Description |
|-----|-------------|
| `struct ctimer` | Callback timer object |
| `kern_status_t ctimer_set(struct ctimer *c, clock_time_t t, void (*f)(void *), void *ptr)` | Sets the timer to call `f(ptr)` after `t` ticks |
| `kern_status_t ctimer_reset(struct ctimer *c)` | Resets with the original interval (no drift) |
| `kern_status_t ctimer_restart(struct ctimer *c)` | Restarts from the current time (drifts) |
| `kern_status_t ctimer_stop(struct ctimer *c)` | Cancels the timer |
| `bool ctimer_expired(struct ctimer *c)` | Whether it has expired |

- Typical usage:
```c
static struct ctimer sampling_timer;
static void sample_now(void *ptr) { /* sample once ... */ }
ctimer_set(&sampling_timer, 5 * CLOCK_SECOND, sample_now, NULL);
```
- Note: ctimers rely on the etimer layer underneath; `ctimer_set` cannot be called in an ISR.

---

## 4. High-Resolution Timing (rtimer)

> Source file: `os/sys/rtimer.h`

rtimer schedules microsecond-level, ultra-low-jitter tasks independently of the system tick clock. Typical uses are precise timing such as bit-banging and sampling triggers.

| API | Description |
|-----|-------------|
| `struct rtimer` | rtimer task object (contains `time`, `func`, `ptr`) |
| `int rtimer_set(struct rtimer *task, rtimer_clock_t time, rtimer_clock_t duration, rtimer_callback_t func, void *ptr)` | Executes `func(ptr)` at absolute time `time`; returns `RTIMER_OK` / `RTIMER_ERR_FULL` / `RTIMER_ERR_TIME` / `RTIMER_ERR_ALREADY_SCHEDULED` |
| `RTIMER_NOW()` | Gets the current rtimer count (macro) |
| `RTIMER_SECOND` | Number of rtimer ticks per second (= `RTIMER_ARCH_SECOND`) |
| `RTIMER_TIME(task)` | Gets the time at which a task last executed |
| `RTIMER_BUSYWAIT_UNTIL(cond, max_ticks)` | Busy-waits until condition `cond` holds, for at most `max_ticks` (macro) |
| `RTIMER_BUSYWAIT(duration)` | Busy-waits for a fixed `duration` of rtimer ticks |
| `rtimer_clock_t` | rtimer count type (2/4/8 bytes, set by `RTIMER_CONF_CLOCK_SIZE`) |
| `rtimer_callback_t` | Callback type: `void (*)(struct rtimer *t, void *ptr)` |

- Typical usage (precise 100 µs delay):
```c
/* For a given MCU: US_TO_RTIMERTICKS is available, see rtimer-arch.h */
RTIMER_BUSYWAIT(US_TO_RTIMERTICKS(100));
```
- Typical usage (scheduling at an absolute time):
```c
static struct rtimer my_rt;
static void tick(void *ptr) { /* precise action */ }
rtimer_set(&my_rt, RTIMER_NOW() + RTIMER_SECOND / 100, 0, tick, NULL);
```
- Note: `time` is an **absolute time**, not a relative delay; margin for `RTIMER_GUARD_TIME` must be left to avoid a "negative delay". `RTIMER_BUSYWAIT_*` busy-waits and occupies the CPU; use it only for short delays.

---

## 5. Interactive Shell

> Source file: `os/services/shell/quilla-shell.h`

The shell is a hardware-independent command interpreter. An APP exposes product features on the serial port / console by registering commands; input is pushed in by the platform HAL via `shell_input()`, and output goes through the callback registered with `shell_set_writer()`.

### Quick Reference

| API | Description |
|-----|-------------|
| `int shell_init(void)` | Initializes the shell core (ring buffer, command table, built-in `help`/`part`/`kernel`) |
| `void shell_set_writer(shell_write_t writer)` | Registers the output callback (called by the platform HAL) |
| `void shell_input(const uint8_t *data, uint16_t len)` | Pushes received bytes into the shell (called by the HAL in interrupt context) |
| `int shell_register_cmd(const shell_cmd_t *cmd)` | Registers a custom command |
| `void shell_output(const char *fmt, ...)` | printf-style formatted output |
| `void shell_puts(const char *s)` | Outputs a raw string line |
| `void shell_set_partition_tag(const char *tag)` | Sets the current partition tag displayed by the `part` command |

### Key Entries

**`shell_cmd_t` struct + handler prototype**
```c
typedef void (*shell_cmd_handler_t)(const char *args);

typedef struct shell_cmd {
  const char *name;          /* command name, no spaces */
  shell_cmd_handler_t handler;/* handler function, must be non-NULL */
  const char *help;          /* one-line help text */
} shell_cmd_t;
```

**`shell_register_cmd(const shell_cmd_t *cmd)`** — registers a command.
- Returns `0` on success; `-1` means the command table is full or the descriptor is invalid.
- Typical usage:
```c
static void cmd_status(const char *args)
{
  shell_output("uptime=%lu s\n", clock_seconds());
}
static const shell_cmd_t cmds[] = {
  { "status", cmd_status, "print uptime" },
};
/* In the APP startup phase: */
shell_register_cmd(&cmds[0]);
```
- Note: the command name / help strings must be resident strings (string literals are recommended).

**`shell_output` / `shell_puts`**
```c
shell_output("temp=%d.%02d C\n", t/100, t%100);
shell_puts("OK\r\n");
```

**`shell_set_partition_tag("app1")`**
- Purpose: sets the partition tag displayed by the built-in `part` command. The APP calls `shell_set_partition_tag("app1")` at startup; QBOOT is tagged `"qboot"`.
- Must be called after shell initialization; the default is already `"app1"`.

> `shell_init()`, `shell_set_writer()`, and `shell_input()` are normally called by the platform startup code. The APP needs only `shell_register_cmd`, `shell_output` / `shell_puts`, and `shell_set_partition_tag`.

---

## 6. Independent Watchdog (qwdt)

> Source file: `arch/cpu/arm/<mcu>/dev/qwdt.h` (e.g., `arch/cpu/arm/py32f040xb/dev/qwdt.h`; GD32F502VE has a same-named `dev/qwdt.h`)

The watchdog is driven by the LSI (RC oscillator). Because its timeout range shows dispersion, the module exposes minimum/maximum bounds for safety analysis. The `QWDT_CONF_*` macros configure the target timeout, the LSI rate assumption, and the process safety time at compile time.

### Quick Reference

| API | Description |
|-----|-------------|
| `kern_status_t qwdt_init(void)` | Computes the prescaler/reload from configuration (does not touch hardware) |
| `kern_status_t qwdt_start(void)` | Starts the watchdog (idempotent) |
| `kern_status_t qwdt_refresh(void)` | Feeds the dog (returns `KERN_ERR_ALREADY` when not started) |
| `void qwdt_reboot(void)` | Immediately triggers a watchdog reset; never returns |
| `unsigned int qwdt_is_started(void)` | Whether it has been started (1/0) |
| `unsigned int qwdt_timeout_nominal_ms(void)` | Nominal timeout (ms) |
| `unsigned int qwdt_timeout_min_ms(void)` | Minimum timeout (basis for the upper bound on feeding frequency) |
| `unsigned int qwdt_timeout_max_ms(void)` | Maximum timeout (compared against the safety time) |
| `unsigned long qwdt_prescaler(void)` | Actually selected prescaler value |
| `unsigned int qwdt_reload(void)` | Actually selected reload value (0x000–0xFFF) |
| `kern_status_t qwdt_calibrate(unsigned long measured_hz)` | Feeds in the measured LSI frequency (see §7) |
| `unsigned long qwdt_measured_lsi_hz(void)` | Currently effective measured frequency (0 when uncalibrated) |
| `unsigned int qwdt_describe(char *dst, unsigned int len)` | Writes a configuration description into the caller's buffer |

- Typical startup flow:
```c
qwdt_init();                 /* after kern_stack_init, before starting the watchdog */
qwdt_start();
```
- Feed the watchdog in the main loop or a low-priority process, at a rate faster than `qwdt_timeout_min_ms()`:
```c
qwdt_refresh();
```
- Notes:
  - `qwdt_init()` must run **after stack monitoring `kern_stack_init()` and before the watchdog starts**; filling the stack takes time.
  - The latest permissible feed time for `qwdt_refresh()` is set by `qwdt_timeout_min_ms()`, not by the nominal value.
  - `qwdt_describe()` does not depend on printf and is safe to use in size-constrained images; it returns the number of bytes written, excluding `\0`.
  - For measured calibration, see §7: `qclk_calibrate()` → `qwdt_calibrate()`.

---

## 7. Clock Cross-Checking (qclk)

> Source file: `arch/cpu/arm/<mcu>/dev/qclk.h` (e.g., `arch/cpu/arm/py32f040xb/dev/qclk.h`)

qclk measures the actual LSI frequency against the system clock and feeds the result to qwdt, narrowing the timeout bounds; it also checks at runtime whether the LSI has lost lock. It is included in the APP image only, since it depends on LPTIM1 with the rtimer already running.

### Quick Reference

| API | Description |
|-----|-------------|
| `kern_status_t qclk_init(void)` | Starts the reference timer and verifies the usable rate |
| `kern_status_t qclk_calibrate(void)` | Blocking measurement of the LSI frequency once (about one window, 50 ms by default) |
| `kern_status_t qclk_poll(void)` | Continuously compares in the idle loop (no action within the window) |
| `unsigned long qclk_measured_lsi_hz(void)` | LSI frequency measured in the most recently completed window |
| `unsigned long qclk_reference_hz(void)` | Actual running frequency of the reference timer |
| `unsigned int qclk_window_count(void)` | Number of windows completed since startup |
| `unsigned int qclk_violation_count(void)` | Number of windows falling outside the allowed band |
| `unsigned long qclk_last_window_hz(void)` | Most recent window frequency (whether out of band or not) |
| `unsigned int qclk_describe(char *dst, unsigned int len)` | Writes a status description into the caller's buffer |

- Typical flow:
```c
qclk_init();
qclk_calibrate();                       /* blocks for about 50 ms */
qwdt_calibrate(qclk_measured_lsi_hz()); /* calibrate the watchdog with the measured value */
```
- Runtime monitoring (idle loop):
```c
if (qclk_poll() == KERN_ERR_CLOCK) {
  /* clock out of bounds; the safety state has been entered via kern_raise(KERN_MOD_CLOCK) */
}
```
- Ordering: `qclk_calibrate()` must run **before** `qwdt_init()`, so that the watchdog selects the prescaler/reload from the measured value. Before `qclk_init()`, the rtimer (LPTIM1) must already be running. Out-of-band conditions are reported through `kern_raise()` as `KERN_MOD_CLOCK`.

---

## 8. Low Power Management (lpm)

> Source file: `arch/cpu/arm/<mcu>/lpm.h` (note: it is under the MCU directory, **not** `dev/`, e.g., `arch/cpu/arm/py32f040xb/lpm.h`; for GD32F502VE: `arch/cpu/arm/gd32f502ve/lpm.h`)

lpm provides two power-saving levels: Sleep (PM0) and Stop 0 (PM1, PY32F040xB only). Entry decisions are made automatically; the APP starts the module, optionally caps the maximum level, and registers peripherals that must not enter PM1.

| API | Description |
|-----|-------------|
| `void lpm_init(void)` | Initializes LPM; starts from PM0 by default |
| `void lpm_enter(void)` | Called in the idle loop (after all events are handled); enters the lowest currently allowed power level |
| `void lpm_exit(void)` | Called at the start of the ISR that wakes from Stop; restores the clocks / software clock |
| `void lpm_set_max_pm(uint8_t pm)` | Limits the maximum enterable level (`LPM_PM0`/`LPM_PM1`) |
| `void lpm_register_peripheral(lpm_periph_permit_pm1_func_t f)` | Registers a peripheral callback for "whether PM1 is allowed" |
| `unsigned int lpm_max_pm(void)` | Currently allowed maximum level |
| `unsigned long lpm_stop_refusals(void)` | Number of times PM1 was refused |
| `const char *lpm_stop_refusal_reason(void)` | Reason for the most recent refusal (static string) |
| `LPM_PM0` / `LPM_PM1` | Level constants (0=Sleep, 1=Stop0) |

- Typical usage (idle loop):
```c
lpm_init();
while(1) {
  process_run();          /* handle all events */
  lpm_enter();            /* automatically decides PM0 or PM1 */
}
```
- Notes (**important**):
  - **`lpm_exit()` must be called at the start of every ISR that can wake the kernel from Stop** (PY32F040xB PM1). It restores the system clock and resynchronizes the software clock (SysTick stops during Stop). Under PM0 it is a no-op, so calling it is harmless.
  - Under PM1 the IWDG keeps counting, still driven by the LSI. `lpm_enter()` constrains the longest sleep time with `qwdt_timeout_min_ms()`; the APP does not compute this itself.
  - Peripherals that rely on clocks stopped during Stop (e.g., USART + DMA) must register a callback returning `false` via `lpm_register_peripheral()`. This pins the system to PM0 and avoids data loss.
  - GD32F502VE **implements PM0 only** (Sleep). Any `LPM_CONF_MAX_PM > 0` fails at compile time, so the Stop-restore logic of `lpm_exit()` is irrelevant on this platform.
  - The module can be disabled wholesale with `LPM_CONF_ENABLE=0`, in which case `lpm_*` degenerates into no-ops / inlines.

---

## 9. Supporting Remote Upgrade (QIAP / swqboot)

> Source file: `os/services/quilla-qboot/quilla-app-qboot.h`

The APP does not need to understand the upgrade protocol. Two things are required: (1) make the `swqboot` command available; (2) set the partition tag to `"app1"` during startup, so that the `part` command and the upgrade flow identify the current image correctly.

| API | Description |
|-----|-------------|
| `void quilla_app_qboot_init(void)` | Registers the shell command `swqboot` (must be called once after the shell starts) |
| `shell_set_partition_tag("app1")` | Sets the current partition tag (see §5) |

- Typical usage (in the APP startup phase):
```c
shell_init();                  /* usually done by the platform */
shell_set_partition_tag("app1");
quilla_app_qboot_init();      /* afterwards the user can enter swqboot in the console to trigger an upgrade */
```
- `swqboot` behavior: sets the QBOOT/APP interaction flag to QIAP and resets into QBOOT, where the bootloader completes the serial upgrade. The APP **does not implement** this flow and must not touch bootloader modules such as `quilla-qiap`.
- `quilla_app_qboot_init()` is compiled into the APP image only and must be called after the shell has started.

---

## 10. Version Numbers

> Source file: `os/sys/quilla-version.h`

All version strings derive from this single file, which prevents inconsistencies between multiple copies.

| Macro | Meaning |
|----|---------|
| `QUILLA_VERSION` | Packaged OS version `(major<<8)|minor` (16-bit, display/diagnostic only) |
| `QUILLA_VERSION_MAJOR` / `QUILLA_VERSION_MINOR` / `QUILLA_VERSION_BUILD` | OS version number (major.minor.build; build is incremented during development only) |
| `QUILLA_VERSION_STRING` | OS version string `"major.minor.build"` |
| `QUILLA_PROJECT_NAME` | Product name (string, **can be overridden with `-D`**, default `"Project"`) |
| `QUILLA_PROJECT_VERSION_MAJOR` / `_MINOR` / `_BUILD` | Product version number |
| `QUILLA_PROJECT_VERSION_STRING` | Product version string `"major.minor.build"` |

> The macros above are **display/diagnostic only** and are never used for boot-time anti-rollback. The boot-time anti-rollback version is **baked into the signed APP image** at the fixed address `QUILLA_QBOOT_APP_START + 0x400` (`0x08007400`) as `QUILLA_APP_SECURITY_VERSION` (see `doc/secure-boot.md` §1.1); QBOOT reads it and compares it against the META floor. The APP also performs a non-cryptographic self-check of that region (`QUILLA_APP_SECVER_SELFCHECK`, default on), which is **obfuscation, not a security control** — it only raises the bar for an authorized-key end user doing a trivial edit, and is not a substitute for signature + floor. No other version macro set by a QAPP build can influence the anti-rollback gate.

- Typical usage:
```c
shell_output("%s V%s (Quilla V%s)\r\n",
             QUILLA_PROJECT_NAME,
             QUILLA_PROJECT_VERSION_STRING,
             QUILLA_VERSION_STRING);
```
- Note: `QUILLA_VERSION_BUILD` and similar macros carry leading zeros (e.g., `0001`) and are **display strings**. Do not evaluate them as numbers in C: leading zeros are parsed as octal. To rename the product, pass `-DQUILLA_PROJECT_NAME=\"MyProduct\"` on the compiler command line; no header change is needed.

---

## 11. Source File Index and Scope Notes

| Feature Group | Header File (relative to the `quilla/` root) |
|----------|--------------------------------|
| Processes/Events | `os/sys/process.h`, `os/sys/autostart.h` |
| System clock | `os/sys/clock.h` |
| Event timers | `os/sys/etimer.h` |
| Callback timers | `os/sys/ctimer.h` |
| High-resolution timing | `os/sys/rtimer.h` |
| Interactive shell | `os/services/shell/quilla-shell.h` |
| Independent watchdog | `arch/cpu/arm/<mcu>/dev/qwdt.h` |
| Clock verification | `arch/cpu/arm/<mcu>/dev/qclk.h` |
| Low power | `arch/cpu/arm/<mcu>/lpm.h` |
| Remote upgrade support | `os/services/quilla-qboot/quilla-app-qboot.h` |
| Version numbers | `os/sys/quilla-version.h` |
| Unified status codes | `os/sys/kern-status.h` (indirectly included by the above headers) |

**Scope notes**:
- The document covers only what to call, the parameters, the return values, and how to use it. How the etimer schedules, how the rtimer counts, how the lpm selects levels, and the like are **OS internal implementation details, out of scope**, and must not be relied upon by an APP.
- Bootloader (QBOOT) interfaces are excluded: `quilla-qiap`, `quilla-qboot-hal`, `quilla-qboot-flag`, etc. belong to the bootloader side and must not be called by an APP. The only APP-side upgrade support points are the `swqboot` trigger and `shell_set_partition_tag("app1")` in §9.
