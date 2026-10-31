##########################################################################################################################
# Quilla QAPP build for CH32V305VB (RISC-V) — EIDE / GNU only.
#
# This MCU is built with the WCH "RISC-V Embedded GCC12" toolchain ONLY (no
# Keil/MDK project). The toolchain prefix is riscv-wch-elf- (xPack GNU RISC-V
# Embedded GCC 12.2.0). Point the build at it either by putting its bin/ on
# PATH or by passing GCC_PATH on the command line, e.g.
#   make GCC_PATH=/path/to/RISC-V\ Embedded\ GCC12/bin
# The absolute toolchain path is intentionally NOT recorded in this file.
#
# Device: CH32V305VB (WCH CH32V30x_D8C, RV32IMAC). 128 KiB Flash linked at the
# 0x00000000 alias, 32 KiB SRAM. Clock: HSE 8 MHz -> PLL -> 96 MHz (see
# system_ch32v30x.c). Console: USART2 on PA2(TX)/PA3(RX). QBOOT owns the first
# 24 KiB; this APP links at 0x00007000 and runs to 0x0001FFFF.
##########################################################################################################################

TARGET = QAPP

DEBUG = 1
OPT = -Og

BUILD_DIR = QAPP

######################################################################
# source
######################################################################
C_SOURCES =  \
../Src/main.c \
../../../arch/cpu/riscv/ch32v305rb/system_ch32v30x.c \
../../../arch/cpu/riscv/ch32v305rb/Core/core_riscv.c \
../../../arch/cpu/riscv/ch32v305rb/clock.c \
../../../arch/cpu/riscv/ch32v305rb/rtimer-arch.c \
../../../arch/cpu/riscv/ch32v305rb/lpm.c \
../../../arch/cpu/riscv/ch32v305rb/soc.c \
../../../arch/cpu/riscv/ch32v305rb/shellport.c \
../../../arch/cpu/riscv/ch32v305rb/quilla-qboot-hal.c \
../../../arch/cpu/riscv/ch32v305rb/dev/watchdog.c \
../../../arch/cpu/riscv/ch32v305rb/dev/qwdt.c \
../../../arch/cpu/riscv/ch32v305rb/dev/qclk.c \
../../../arch/cpu/riscv/ch32v305rb/Peripheral/src/ch32v30x_rcc.c \
../../../arch/cpu/riscv/ch32v305rb/Peripheral/src/ch32v30x_gpio.c \
../../../arch/cpu/riscv/ch32v305rb/Peripheral/src/ch32v30x_usart.c \
../../../arch/cpu/riscv/ch32v305rb/Peripheral/src/ch32v30x_dma.c \
../../../arch/cpu/riscv/ch32v305rb/Peripheral/src/ch32v30x_tim.c \
../../../arch/cpu/riscv/ch32v305rb/Peripheral/src/ch32v30x_flash.c \
../../../arch/cpu/riscv/ch32v305rb/Peripheral/src/ch32v30x_iwdg.c \
../../../arch/cpu/riscv/ch32v305rb/Peripheral/src/ch32v30x_rtc.c \
../../../arch/cpu/riscv/ch32v305rb/Peripheral/src/ch32v30x_pwr.c \
../../../arch/cpu/riscv/ch32v305rb/Peripheral/src/ch32v30x_bkp.c \
../../../os/sys/atomic.c \
../../../os/sys/autostart.c \
../../../os/sys/ctimer.c \
../../../os/sys/etimer.c \
../../../os/sys/mutex.c \
../../../os/sys/process.c \
../../../os/sys/kern-guard.c \
../../../os/sys/kern-nomalloc.c \
../../../os/sys/kern-stack.c \
../../../os/sys/kern-flow.c \
../../../os/sys/process-mutex.c \
../../../os/sys/rtimer.c \
../../../os/sys/stimer.c \
../../../os/sys/timer.c \
../../../os/lib/assert.c \
../../../os/lib/crc16.c \
../../../os/lib/list.c \
../../../os/lib/memb.c \
../../../os/lib/ringbuf.c \
../../../os/lib/ringbufindex.c \
../../../arch/cpu/riscv/int-master.c \
../../../os/services/shell/quilla-shell.c \
../../../arch/platform/riscv/ch32v305rb/platform.c \
../../../os/services/quilla-qboot/quilla-qboot-flag.c \
../../../os/services/quilla-qboot/quilla-qboot-meta.c \
../../../os/services/quilla-qboot/quilla-app-qboot.c

# ASM sources
ASM_SOURCES =  \
../../../arch/cpu/riscv/ch32v305rb/Startup/startup_ch32v30x_D8C.S

######################################################################
# binaries
######################################################################
PREFIX = riscv-wch-elf-
ifdef GCC_PATH
CC = "$(GCC_PATH)/$(PREFIX)gcc"
AS = "$(GCC_PATH)/$(PREFIX)gcc" -x assembler-with-cpp
CP = "$(GCC_PATH)/$(PREFIX)objcopy"
SZ = "$(GCC_PATH)/$(PREFIX)size"
else
CC = $(PREFIX)gcc
AS = $(PREFIX)gcc -x assembler-with-cpp
CP = $(PREFIX)objcopy
SZ = $(PREFIX)size
endif
HEX = $(CP) -O ihex
BIN = $(CP) -O binary -S

######################################################################
# CFLAGS
######################################################################
# CH32V305VB: RV32IMAC, soft-float ABI (ilp32; no FPU used, matching the GD32
# "no floating point" decision).
CPU = -march=rv32imac -mabi=ilp32
MCU = $(CPU)

C_DEFS =  \
-DCH32V30x_D8C\
-DQAPP_BUILD\
-DQUILLA_QBOOT_FLASH_BASE=0x00000000U\
-DQUILLA_QBOOT_APP_START=0x00007000U\
-DQUILLA_QBOOT_META_BASE=0x00006000U\
-DQUILLA_QBOOT_PAGE_SIZE=256U\
-DQUILLA_QBOOT_RAM_START=0x20000000U\
-DQUILLA_QBOOT_RAM_SIZE=0x00008000U\
-DQUILLA_APP_SECVER_ADDR=0x00007400\
-DQUILLA_APP_SECVER_SELFCHECK=0

AS_INCLUDES =

C_INCLUDES =  \
-I../Inc \
-I../../../os \
-I../../../os/sys \
-I../../../os/lib \
-I../../../arch/cpu/riscv \
-I../../../arch/cpu/riscv/ch32v305rb \
-I../../../arch/cpu/riscv/ch32v305rb/Peripheral/inc \
-I../../../arch/cpu/riscv/ch32v305rb/Core \
-I../../../arch/platform/riscv \
-I../../../arch/platform/riscv/ch32v305rb \
-I../../../os/services/shell \
-I../../../os/services/quilla-qboot

ASFLAGS = $(MCU) $(AS_DEFS) $(AS_INCLUDES) $(OPT) -Wall -fdata-sections -ffunction-sections

CFLAGS += $(MCU) $(C_DEFS) $(C_INCLUDES) $(OPT) -Wall -fdata-sections -ffunction-sections

ifeq ($(DEBUG), 1)
CFLAGS += -g -gdwarf-2
endif

CFLAGS += -MMD -MP -MF"$(@:%.o=%.d)"

######################################################################
# LDFLAGS
######################################################################
# -nostartfiles: the WCH startup provides _start / handle_reset; without this
# the toolchain's own crt0 _start collides. .init_array ctors are not run by
# handle_reset, which is fine for this C-only tree.
LDSCRIPT = ch32v305rb_app.ld

LIBS = -lc -lm -lnosys
LIBDIR =
LDFLAGS = $(MCU) -T$(LDSCRIPT) -nostartfiles -specs=nano.specs -specs=nosys.specs $(LIBDIR) $(LIBS) -Wl,-Map=$(BUILD_DIR)/$(TARGET).map,--cref -Wl,--gc-sections

all: $(BUILD_DIR)/$(TARGET).elf $(BUILD_DIR)/$(TARGET).hex $(BUILD_DIR)/$(TARGET).bin

OBJECTS = $(addprefix $(BUILD_DIR)/,$(notdir $(C_SOURCES:.c=.o)))
vpath %.c $(sort $(dir $(C_SOURCES)))
OBJECTS += $(addprefix $(BUILD_DIR)/,$(notdir $(ASM_SOURCES:.S=.o)))
vpath %.S $(sort $(dir $(ASM_SOURCES)))

$(BUILD_DIR)/%.o: %.c Makefile | $(BUILD_DIR)
	$(CC) -c $(CFLAGS) -Wa,-a,-ad,-alms=$(BUILD_DIR)/$(notdir $(<:.c=.lst)) $< -o $@

$(BUILD_DIR)/%.o: %.S Makefile | $(BUILD_DIR)
	$(AS) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/$(TARGET).elf: $(OBJECTS) Makefile
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@
	$(SZ) $@

$(BUILD_DIR)/%.hex: $(BUILD_DIR)/%.elf | $(BUILD_DIR)
	$(HEX) $< $@

$(BUILD_DIR)/%.bin: $(BUILD_DIR)/%.elf | $(BUILD_DIR)
	$(BIN) $< $@

$(BUILD_DIR):
	mkdir -p $@

clean:
	-rm -fR $(BUILD_DIR)

-include $(wildcard $(BUILD_DIR)/*.d)

# *** EOF ***
