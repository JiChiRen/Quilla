**[中文](app-api-reference.zh-CN.md) · [English](app-api-reference.md)**

# Quilla OS 层公开 API 参考（面向应用开发者）

> 本文档是应用开发者在 Quilla 系统上实现产品功能时唯一需要查阅的 OS 层 API 手册。
> 配套阅读：`doc/qboot-app-architecture.md`（双分区架构）、`doc/upstream/programming/Processes-and-events.md` 与 `doc/upstream/programming/Timers.md`（Contiki 进程与定时器编程指南）。

## 1. 读者对象与设计原则

**读者对象**：在 `user/<mcu>/Src/main.c` 及其它 APP 源码中调用系统 API 实现产品功能的应用开发者。本文只回答"调哪个函数、传什么参数、怎么用"；OS 内部的调度与计数机制不在范围内。

**设计原则**：

1. **只调用公开 API**。本文列出的函数、宏、结构体均为 `os/` 与 `arch/` 公开头文件导出的稳定接口。APP 不得直接访问内核内部数据结构，也不得依赖未列出的符号。
2. **APP 与 OS 内部解耦**。定时器、进程、shell、看门狗、低功耗均通过统一接口暴露。更换 MCU（如 PY32F040xB ↔ GD32F502VE）时，只要使用同名公开 API，APP 代码无需改动。
3. **QBOOT 专属内容不在范围内**。`quilla-qiap`、`quilla-qboot-hal`、`quilla-qboot-flag` 等属于 bootloader 侧。本文仅覆盖 APP 侧对远程升级的支持：`quilla_app_qboot_init`、`swqboot` 命令、`shell_set_partition_tag("app1")`。

所有返回 `kern_status_t` 的接口使用统一状态码（`os/sys/kern-status.h`）：`KERN_OK`（0，成功），以及 `KERN_ERR_NULL / KERN_ERR_ALREADY / KERN_ERR_NOT_READY / KERN_ERR_FULL / KERN_ERR_RANGE / KERN_ERR_CONTEXT / KERN_ERR_INTERNAL / KERN_ERR_STACK / KERN_ERR_CLOCK` 等。返回值忽略不影响编译；生产代码仍应检查 `qwdt_*`、`qclk_*` 等关键路径。

---

## 2. 进程模型与事件（Contiki process）

APP 的每一个后台任务都是一个 **process**（协程/protothread）：不是线程，而是由内核事件驱动的轻量协程。遇到 `PROCESS_WAIT_*` / `PROCESS_YIELD` 时让出 CPU，事件到达时从让出处恢复。

> 源文件：`os/sys/process.h`、`os/sys/autostart.h`

### 速查表

| API | 作用 |
|-----|------|
| `PROCESS_THREAD(name, ev, data)` | 定义一个 process 的函数体 |
| `PROCESS(name, strname)` | 声明并初始化一个 process 变量（在 C 文件中） |
| `PROCESS_NAME(name)` | 在头文件中前向声明一个 process 名字 |
| `PROCESS_BEGIN()` / `PROCESS_END()` | process 体首/尾固定宏 |
| `PROCESS_YIELD()` | 让出 CPU，下次事件到达时恢复 |
| `PROCESS_WAIT_EVENT()` | 等待任意事件（等价于 `PROCESS_YIELD`） |
| `PROCESS_WAIT_EVENT_UNTIL(c)` | 等待事件且条件 `c` 成立才继续 |
| `PROCESS_EXIT()` | 退出当前 process |
| `PROCESS_PAUSE()` | 让出片刻，让其它 process 先跑 |
| `PROCESS_CURRENT()` | 取当前正在运行的 process 指针 |
| `PROCESS_BROADCAST` | 广播事件目标（向所有 process 投递） |
| `process_start(p, data)` | 启动一个 process |
| `process_post(p, ev, data)` | 异步向某 process（或广播）投递事件 |
| `process_poll(p)` | 请求立即轮询某 process（可在 ISR 中调用） |
| `process_alloc_event()` | 分配一个全局事件号（>127） |
| `AUTOSTART_PROCESSES(...)` | 声明上电自动启动的 process 列表 |
| `autostart_start(list)` | 启动给定的 process 列表 |

### 关键条目

**`PROCESS_THREAD(name, ev, data)`** — 定义 process 入口。
- 作用：声明一个静态 process 线程函数，必须与 `PROCESS_BEGIN()` / `PROCESS_END()` 配对。
- 典型用法：
```c
PROCESS_THREAD(led_blink_process, ev, data)
{
  static struct etimer timer;
  PROCESS_BEGIN();
  etimer_set(&timer, CLOCK_SECOND);          /* 见 §3 定时器 */
  while(1) {
    PROCESS_WAIT_EVENT_UNTIL(ev == PROCESS_EVENT_TIMER);
    /* 翻转 LED ... */
    etimer_reset(&timer);                     /* 周期触发 */
  }
  PROCESS_END();
}
```

**`PROCESS(name, strname)`** — 在 `.c` 文件中创建 process 实例。
```c
PROCESS(led_blink_process, "LED blink");
```

**`process_start(struct process *p, process_data_t data)`**
- 作用：启动一个 process。成功返回 `KERN_OK`。
- 典型用法：在 `main.c` 启动阶段调用，或在另一个 process 里动态拉起。`data` 会作为 `PROCESS_BEGIN()` 之后的 `data` 参数传入。

**`process_post(p, ev, data)`**
- 作用：异步投递事件，目标可以是某个 process 或 `PROCESS_BROADCAST`。返回 `PROCESS_ERR_OK` / `PROCESS_ERR_FULL`（队列满）。
- 典型用法：ISR 或定时器回调不能直接做复杂工作，可 `process_post` 一个自定义事件给某 process 处理。

**`process_poll(p)`**
- 作用：让某 process 在下一个调度点被"轮询"（`PROCESS_EVENT_POLL` 事件）。可在中断上下文调用，用于把 ISR 的工作转交给 process。

**`AUTOSTART_PROCESSES(...)` + `autostart_start()`**
- 作用：列出上电自动启动的 process，由系统启动代码统一拉起。
- 典型用法：
```c
AUTOSTART_PROCESSES(&led_blink_process, &shell_process);
/* 系统启动时会调用 autostart_start(autostart_processes); */
```
- 注意：`shell_process` 等系统 process 可能已由平台侧的 `autostart` 列表包含，APP 只需追加自己的。

---

## 3. 定时器与系统时钟

### 3a. 系统时钟 clock

> 源文件：`os/sys/clock.h`

| API | 作用 |
|-----|------|
| `clock_time_t clock_time(void)` | 当前系统节拍计数（tick） |
| `unsigned long clock_seconds(void)` | 启动以来的秒数 |
| `void clock_set_seconds(unsigned long sec)` | 设置秒计数 |
| `clock_time_t clock_wait(clock_time_t t)` | 忙等若干 tick |
| `void clock_delay_usec(uint16_t dt)` | 忙等若干微秒（受中断影响） |
| `CLOCK_SECOND` | 一秒对应的 tick 数（宏，`clock_time_t` 类型） |
| `clock_time_t` | 节拍计数类型（4 或 8 字节，由 `CLOCK_CONF_SIZE`） |

- `clock_init()` / `rtimer_init()` / `process_init()` 等初始化函数在 boot 流程中已由平台调用，**应用开发者一般无需调用**。
- `CLOCK_SECOND` 是 etimer/ctimer 时间参数的换算基准，例如 `etimer_set(&t, 2 * CLOCK_SECOND)` 表示 2 秒。

### 3b. 事件定时器 etimer

> 源文件：`os/sys/etimer.h`

etimer 到期时向设置它的 process 投递 `PROCESS_EVENT_TIMER` 事件，因此必须在 process 内部使用。

| API | 作用 |
|-----|------|
| `struct etimer` | 事件定时器对象（声明后需 `etimer_set`） |
| `kern_status_t etimer_set(struct etimer *et, clock_time_t interval)` | 启动定时器，`interval` 个 tick 后到期 |
| `kern_status_t etimer_reset(struct etimer *et)` | 以原间隔重置（从到期时刻算，无漂移，适合周期） |
| `kern_status_t etimer_restart(struct etimer *et)` | 从当前时刻重启动（会漂移） |
| `kern_status_t etimer_adjust(struct etimer *et, int td)` | 微调到期时间 |
| `bool etimer_expired(struct etimer *et)` | 是否已到期（返回 true 表示已触发） |
| `kern_status_t etimer_stop(struct etimer *et)` | 取消未到期定时器 |
| `clock_time_t etimer_expiration_time(struct etimer *et)` | 到期时刻（tick） |
| `int etimer_pending(void)` | 是否存在未到期定时器 |
| `clock_time_t etimer_next_expiration_time(void)` | 下一个到期的绝对时刻（idle 休眠用） |

- **注意**：`etimer_set` 必须在 process 上下文调用；返回值 `KERN_ERR_CONTEXT` 表示在 ISR 中调用，不允许。
- 典型用法（周期闪烁，见 §2 示例）。一次性延时：
```c
static struct etimer t;
etimer_set(&t, CLOCK_SECOND / 10);
PROCESS_WAIT_EVENT_UNTIL(ev == PROCESS_EVENT_TIMER);
/* 100ms 到，做一件事 */
```

### 3c. 回调定时器 ctimer

> 源文件：`os/sys/ctimer.h`

ctimer 到期时**直接调用注册的 C 函数**，无需 process 等待事件，用于到点执行一段代码。

| API | 作用 |
|-----|------|
| `struct ctimer` | 回调定时器对象 |
| `kern_status_t ctimer_set(struct ctimer *c, clock_time_t t, void (*f)(void *), void *ptr)` | 设置在 `t` 个 tick 后调用 `f(ptr)` |
| `kern_status_t ctimer_reset(struct ctimer *c)` | 以原间隔重置（无漂移） |
| `kern_status_t ctimer_restart(struct ctimer *c)` | 从当前时刻重启（会漂移） |
| `kern_status_t ctimer_stop(struct ctimer *c)` | 取消定时器 |
| `bool ctimer_expired(struct ctimer *c)` | 是否已到期 |

- 典型用法：
```c
static struct ctimer sampling_timer;
static void sample_now(void *ptr) { /* 采集一次 ... */ }
ctimer_set(&sampling_timer, 5 * CLOCK_SECOND, sample_now, NULL);
```
- 注意：ctimer 仍依赖 etimer 底层，不能在 ISR 中 `ctimer_set`。

---

## 4. 高精度定时 rtimer

> 源文件：`os/sys/rtimer.h`

rtimer 用于微秒级/超低抖动的任务调度，独立于系统节拍时钟，常用于精确时序（如 bit-bang、采样触发）。

| API | 作用 |
|-----|------|
| `struct rtimer` | rtimer 任务对象（含 `time`、`func`、`ptr`） |
| `int rtimer_set(struct rtimer *task, rtimer_clock_t time, rtimer_clock_t duration, rtimer_callback_t func, void *ptr)` | 在绝对时刻 `time` 执行 `func(ptr)`；返回 `RTIMER_OK` / `RTIMER_ERR_FULL` / `RTIMER_ERR_TIME` / `RTIMER_ERR_ALREADY_SCHEDULED` |
| `RTIMER_NOW()` | 取当前 rtimer 计数值（宏） |
| `RTIMER_SECOND` | 每秒对应的 rtimer tick 数（= `RTIMER_ARCH_SECOND`） |
| `RTIMER_TIME(task)` | 取某任务上次执行的时刻 |
| `RTIMER_BUSYWAIT_UNTIL(cond, max_ticks)` | 忙等条件 `cond` 成立，最多 `max_ticks`（宏） |
| `RTIMER_BUSYWAIT(duration)` | 忙等固定 `duration` 个 rtimer tick |
| `rtimer_clock_t` | rtimer 计数类型（2/4/8 字节，由 `RTIMER_CONF_CLOCK_SIZE`） |
| `rtimer_callback_t` | 回调类型：`void (*)(struct rtimer *t, void *ptr)` |

- 典型用法（精确延时 100µs）：
```c
/* 假设某 MCU：US_TO_RTIMERTICKS 可用，见 rtimer-arch.h */
RTIMER_BUSYWAIT(US_TO_RTIMERTICKS(100));
```
- 典型用法（绝对时刻调度）：
```c
static struct rtimer my_rt;
static void tick(void *ptr) { /* 精确动作 */ }
rtimer_set(&my_rt, RTIMER_NOW() + RTIMER_SECOND / 100, 0, tick, NULL);
```
- 注意：`time` 是**绝对时刻**而非相对延迟；需留出 `RTIMER_GUARD_TIME` 余量以避免负延迟。`RTIMER_BUSYWAIT_*` 为忙等，会占住 CPU，仅适用于短延时。

---

## 5. 交互式 shell

> 源文件：`os/services/shell/quilla-shell.h`

shell 是硬件无关的命令解释器。APP 通过注册命令把产品功能暴露在串口/控制台上；输入由平台 HAL 通过 `shell_input()` 推入，输出通过 `shell_set_writer()` 注册的回调送出。

### 速查表

| API | 作用 |
|-----|------|
| `int shell_init(void)` | 初始化 shell 核心（环形缓冲、命令表、内置 `help`/`part`/`kernel`） |
| `void shell_set_writer(shell_write_t writer)` | 注册输出回调（平台 HAL 调用） |
| `void shell_input(const uint8_t *data, uint16_t len)` | 把收到的字节推入 shell（HAL 在中断上下文调用） |
| `int shell_register_cmd(const shell_cmd_t *cmd)` | 注册一条自定义命令 |
| `void shell_output(const char *fmt, ...)` | printf 风格格式化输出 |
| `void shell_puts(const char *s)` | 输出一行原始字符串 |
| `void shell_set_partition_tag(const char *tag)` | 设置 `part` 命令显示的当前分区标识 |

### 关键条目

**`shell_cmd_t` 结构 + handler 原型**
```c
typedef void (*shell_cmd_handler_t)(const char *args);

typedef struct shell_cmd {
  const char *name;          /* 命令名，无空格 */
  shell_cmd_handler_t handler;/* 处理函数，必须非 NULL */
  const char *help;          /* 一行帮助文本 */
} shell_cmd_t;
```

**`shell_register_cmd(const shell_cmd_t *cmd)`** — 注册命令。
- 返回 `0` 成功，`-1` 表示命令表已满或描述符非法。
- 典型用法：
```c
static void cmd_status(const char *args)
{
  shell_output("uptime=%lu s\n", clock_seconds());
}
static const shell_cmd_t cmds[] = {
  { "status", cmd_status, "print uptime" },
};
/* 在 APP 启动阶段： */
shell_register_cmd(&cmds[0]);
```
- 注意：命令名/help 字符串必须是常驻字符串（建议用字符串字面量）。

**`shell_output` / `shell_puts`**
```c
shell_output("temp=%d.%02d C\n", t/100, t%100);
shell_puts("OK\r\n");
```

**`shell_set_partition_tag("app1")`**
- 作用：设置内置 `part` 命令显示的分区标识。APP 启动时调用 `shell_set_partition_tag("app1")`，QBOOT 则标记 `"qboot"`。
- 须在 shell 初始化之后调用；默认即为 `"app1"`。

> `shell_init()`、`shell_set_writer()`、`shell_input()` 由平台启动代码调用；APP 只需 `shell_register_cmd`、`shell_output`/`shell_puts`、`shell_set_partition_tag`。

---

## 6. 独立看门狗 qwdt

> 源文件：`arch/cpu/arm/<mcu>/dev/qwdt.h`（如 `arch/cpu/arm/py32f040xb/dev/qwdt.h`；GD32F502VE 有同名 `dev/qwdt.h`）

看门狗由 LSI（RC 振荡器）驱动。超时范围存在离散度，模块给出最短/最长边界供安全分析使用。`QWDT_CONF_*` 系列宏在编译期配置目标超时、LSI 速率假设与过程安全时间。

### 速查表

| API | 作用 |
|-----|------|
| `kern_status_t qwdt_init(void)` | 按配置计算分频/重载（不碰硬件） |
| `kern_status_t qwdt_start(void)` | 启动看门狗（幂等） |
| `kern_status_t qwdt_refresh(void)` | 喂狗（未启动时返回 `KERN_ERR_ALREADY`） |
| `void qwdt_reboot(void)` | 立即触发看门狗复位，永不返回 |
| `unsigned int qwdt_is_started(void)` | 是否已启动（1/0） |
| `unsigned int qwdt_timeout_nominal_ms(void)` | 标称超时（ms） |
| `unsigned int qwdt_timeout_min_ms(void)` | 最短超时（喂狗频率上限依据） |
| `unsigned int qwdt_timeout_max_ms(void)` | 最长超时（与安全时间比对） |
| `unsigned long qwdt_prescaler(void)` | 实际选中的分频值 |
| `unsigned int qwdt_reload(void)` | 实际选中的重载值（0x000–0xFFF） |
| `kern_status_t qwdt_calibrate(unsigned long measured_hz)` | 喂入实测 LSI 频率（见 §7） |
| `unsigned long qwdt_measured_lsi_hz(void)` | 当前生效的实测频率（未校准为 0） |
| `unsigned int qwdt_describe(char *dst, unsigned int len)` | 把配置描述写入调用方缓冲区 |

- 典型启动流程：
```c
qwdt_init();                 /* 在 kern_stack_init 之后、看门狗启动前 */
qwdt_start();
```
- 在主循环或低优先级 process 中喂狗，频率快于 `qwdt_timeout_min_ms()`：
```c
qwdt_refresh();
```
- 注意：
  - `qwdt_init()` 必须在**栈监控 `kern_stack_init()` 之后、看门狗启动前**完成；填充栈需要时间。
  - `qwdt_refresh()` 的最迟喂狗时刻由 `qwdt_timeout_min_ms()` 决定，而非标称值。
  - `qwdt_describe()` 不依赖 printf，可安全用于资源受限镜像；返回写入字节数（不含 `\0`）。
  - 实测校准见 §7：`qclk_calibrate()` → `qwdt_calibrate()`。

---

## 7. 时钟交叉校验 qclk

> 源文件：`arch/cpu/arm/<mcu>/dev/qclk.h`（如 `arch/cpu/arm/py32f040xb/dev/qclk.h`）

qclk 以系统时钟为参考测量 LSI 实际频率，结果喂给 qwdt 以缩小超时边界；同时可在运行期持续检查 LSI 是否失锁。仅 APP 镜像包含，因其依赖 rtimer 已启动的 LPTIM1。

### 速查表

| API | 作用 |
|-----|------|
| `kern_status_t qclk_init(void)` | 启动参考定时器并校验可用速率 |
| `kern_status_t qclk_calibrate(void)` | 阻塞测量一次 LSI 频率（约一窗口，默认 50ms） |
| `kern_status_t qclk_poll(void)` | 在 idle 循环中持续比对（窗口内无操作） |
| `unsigned long qclk_measured_lsi_hz(void)` | 最近一次完成的窗口测得的 LSI 频率 |
| `unsigned long qclk_reference_hz(void)` | 参考定时器实际运行频率 |
| `unsigned int qclk_window_count(void)` | 启动以来完成的窗口数 |
| `unsigned int qclk_violation_count(void)` | 落在允许带外的窗口数 |
| `unsigned long qclk_last_window_hz(void)` | 最近窗口频率（无论是否超带） |
| `unsigned int qclk_describe(char *dst, unsigned int len)` | 把状态描述写入调用方缓冲区 |

- 典型流程：
```c
qclk_init();
qclk_calibrate();                       /* 阻塞约 50ms */
qwdt_calibrate(qclk_measured_lsi_hz()); /* 用实测值校准看门狗 */
```
- 运行期监控（idle 循环）：
```c
if (qclk_poll() == KERN_ERR_CLOCK) {
  /* 时钟越界，已通过 kern_raise(KERN_MOD_CLOCK) 进入安全态 */
}
```
- 调用顺序：`qclk_calibrate()` 必须在 `qwdt_init()` **之前**执行，看门狗才能按实测值选择分频/重载；`qclk_init()` 之前需确保 rtimer（LPTIM1）已运行。越界经 `kern_raise()` 上报为 `KERN_MOD_CLOCK`。

---

## 8. 低功耗 lpm

> 源文件：`arch/cpu/arm/<mcu>/lpm.h`（注意：在 MCU 目录下，**不是** `dev/`，如 `arch/cpu/arm/py32f040xb/lpm.h`；GD32F502VE 为 `arch/cpu/arm/gd32f502ve/lpm.h`）

lpm 提供 Sleep（PM0）与 Stop 0（PM1，仅 PY32F040xB）两级省电模式。进入决策由模块自动完成，APP 只需启动它、必要时限制最高档位、并注册"禁止进入 PM1 的外设"。

| API | 作用 |
|-----|------|
| `void lpm_init(void)` | 初始化 LPM，默认从 PM0 开始 |
| `void lpm_enter(void)` | 在 idle 循环（所有事件处理完）调用，进入当前允许的最低功耗 |
| `void lpm_exit(void)` | 从 Stop 唤醒的 ISR 开头调用，恢复时钟/软件时钟 |
| `void lpm_set_max_pm(uint8_t pm)` | 限制最高可进入档位（`LPM_PM0`/`LPM_PM1`） |
| `void lpm_register_peripheral(lpm_periph_permit_pm1_func_t f)` | 注册"是否允许 PM1"的外设回调 |
| `unsigned int lpm_max_pm(void)` | 当前允许的最高档位 |
| `unsigned long lpm_stop_refusals(void)` | PM1 被拒绝的次数 |
| `const char *lpm_stop_refusal_reason(void)` | 上一次拒绝原因（静态字符串） |
| `LPM_PM0` / `LPM_PM1` | 档位常量（0=Sleep，1=Stop0） |

- 典型用法（idle 循环）：
```c
lpm_init();
while(1) {
  process_run();          /* 处理完所有事件 */
  lpm_enter();            /* 自动决定 PM0 还是 PM1 */
}
```
- 注意（**重要**）：
  - **`lpm_exit()` 必须在每一个能把内核从 Stop 唤醒的 ISR 开头调用**（PY32F040xB PM1），负责恢复系统时钟并重新同步软件时钟（SysTick 在 Stop 期间停止）。PM0 下为空操作，调用无害。
  - PM1 下 IWDG 仍由 LSI 驱动持续计数；`lpm_enter()` 用 `qwdt_timeout_min_ms()` 约束最长可睡时间，APP 无需自行计算。
  - 依赖 Stop 期间停掉的时钟的外设（如 USART + DMA），须通过 `lpm_register_peripheral()` 注册返回 `false` 的回调，将系统强制留在 PM0，避免丢数据。
  - GD32F502VE **只实现 PM0**（Sleep），任何 `LPM_CONF_MAX_PM > 0` 在编译期报错，因此该平台无需关心 `lpm_exit()` 的 Stop 恢复逻辑。
  - 整个模块可被 `LPM_CONF_ENABLE=0` 整体关闭，届时 `lpm_*` 退化为空操作/内联。

---

## 9. 支持远程升级（QIAP / swqboot）

> 源文件：`os/services/quilla-qboot/quilla-app-qboot.h`

APP 无需理解升级协议，只需做两件事：(1) 让 `swqboot` 命令可用；(2) 启动阶段把分区标识设为 `"app1"`，使 `part` 命令和升级流程正确识别当前镜像。

| API | 作用 |
|-----|------|
| `void quilla_app_qboot_init(void)` | 注册 shell 命令 `swqboot`（须在 shell 启动之后调用一次） |
| `shell_set_partition_tag("app1")` | 设置当前分区标识（见 §5） |

- 典型用法（在 APP 启动阶段）：
```c
shell_init();                  /* 通常由平台完成 */
shell_set_partition_tag("app1");
quilla_app_qboot_init();      /* 之后用户可在控制台输入 swqboot 触发升级 */
```
- `swqboot` 行为：将 QBOOT/APP 交互标志置为 QIAP 并复位进入 QBOOT，由 bootloader 完成串口升级。APP 侧**不实现**该流程，也不得触碰 `quilla-qiap` 等 bootloader 模块。
- `quilla_app_qboot_init()` 仅编译进 APP 镜像，且必须在 shell 已启动后调用。

---

## 10. 版本号

> 源文件：`os/sys/quilla-version.h`

所有版本字符串都从这一处派生，杜绝多处拷贝导致不同步。

| 宏 | 含义 |
|----|------|
| `QUILLA_VERSION` | 打包的 OS 版本 `(major<<8)|minor`（16 位，仅展示/诊断用途） |
| `QUILLA_VERSION_MAJOR` / `QUILLA_VERSION_MINOR` / `QUILLA_VERSION_BUILD` | OS 版本号（major.minor.build，build 仅开发期递增） |
| `QUILLA_VERSION_STRING` | OS 版本字符串 `"major.minor.build"` |
| `QUILLA_PROJECT_NAME` | 产品名（字符串，**可用 `-D` 覆盖**，默认 `"Project"`） |
| `QUILLA_PROJECT_VERSION_MAJOR` / `_MINOR` / `_BUILD` | 产品版本号 |
| `QUILLA_PROJECT_VERSION_STRING` | 产品版本字符串 `"major.minor.build"` |

> 上述宏**仅用于展示/诊断**，绝不参与启动期防回滚。启动期防回滚版本**烧录在已签名的 APP 镜像**内、固定地址 `QUILLA_QBOOT_APP_START + 0x400`（`0x08007400`），即 `QUILLA_APP_SECURITY_VERSION`（见 `doc/secure-boot.md` §1.1）；QBOOT 读取它与 META 下限比较。APP 另对该区域做非密码学的自检（`QUILLA_APP_SECVER_SELFCHECK`，默认开），它**是混淆、不是安全控制**——只抬高持授权私钥终端用户的门槛，不构成签名+下限的替代。QAPP 构建设置任何其它版本宏都无法影响防回滚闸门。

- 典型用法：
```c
shell_output("%s V%s (Quilla V%s)\r\n",
             QUILLA_PROJECT_NAME,
             QUILLA_PROJECT_VERSION_STRING,
             QUILLA_VERSION_STRING);
```
- 注意：`QUILLA_VERSION_BUILD` 等带前导零（如 `0001`），是**展示用字符串**，不得在 C 中当作数值求值（前导零按八进制解析）。重命名产品时，在编译命令行传 `-DQUILLA_PROJECT_NAME=\"MyProduct\"` 即可，无需改头文件。

---

## 11. 源文件索引与范围说明

| 功能分组 | 头文件（相对 `quilla/` 根目录） |
|----------|--------------------------------|
| 进程/事件 | `os/sys/process.h`、`os/sys/autostart.h` |
| 系统时钟 | `os/sys/clock.h` |
| 事件定时器 | `os/sys/etimer.h` |
| 回调定时器 | `os/sys/ctimer.h` |
| 高精度定时 | `os/sys/rtimer.h` |
| 交互 shell | `os/services/shell/quilla-shell.h` |
| 独立看门狗 | `arch/cpu/arm/<mcu>/dev/qwdt.h` |
| 时钟校验 | `arch/cpu/arm/<mcu>/dev/qclk.h` |
| 低功耗 | `arch/cpu/arm/<mcu>/lpm.h` |
| 远程升级支持 | `os/services/quilla-qboot/quilla-app-qboot.h` |
| 版本号 | `os/sys/quilla-version.h` |
| 统一状态码 | `os/sys/kern-status.h`（被上述头文件间接包含） |

**范围说明**：
- 本文只描述"调什么、参数、返回值、怎么用"。etimer 如何调度、rtimer 如何计数、lpm 如何选择档位等 **OS 内部实现细节不在范围内**，APP 不得依赖。
- 本文不含 bootloader（QBOOT）专属接口：`quilla-qiap`、`quilla-qboot-hal`、`quilla-qboot-flag` 等属于 bootloader 侧，APP 不应调用。APP 侧对升级的支持点仅限 §9 的 `swqboot` 触发与 `shell_set_partition_tag("app1")`。
