# Replay badge extension to MicroPython's ports/embed/embed.mk.
# Adds VFS FAT, readline, pyexec, and timeutils sources to the QSTR scan
# so their MP_QSTR_* tokens are auto-discovered.

# Enable VFS FAT BEFORE including embed.mk (extmod.mk uses ifeq).
MICROPY_VFS_FAT = 1

# Usermod directory (passed in from setup_arduino_embed.sh).
USERMODS_DIR ?= .

# Extend SRC_QSTR *BEFORE* including embed.mk so mkrules.mk sees these dependencies when parsing the qstr.i.last rule.
SRC_QSTR += \
    shared/readline/readline.c \
    shared/runtime/pyexec.c \
    shared/runtime/sys_stdio_mphal.c \
    shared/timeutils/timeutils.c \
    extmod/modmachine.c \
    extmod/modnetwork.c \
    extmod/machine_adc.c \
    extmod/machine_adc_block.c \
    extmod/machine_pwm.c \
    extmod/machine_i2c.c \
    extmod/machine_spi.c \
	extmod/machine_bitstream.c \
	extmod/machine_pinbase.c \
    ports/embed/port/replay_machine_qstr.c \
    ports/embed/port/replay_extra_rootpointers.c \
    extmod/vfs.c \
    extmod/vfs_blockdev.c \
    extmod/vfs_fat.c \
    extmod/vfs_fat_diskio.c \
    extmod/vfs_fat_file.c \
    extmod/vfs_reader.c \
    extmod/moductypes.c \
    extmod/modvfs.c \
    extmod/modos.c \
    extmod/os_dupterm.c \
    extmod/modheapq.c \
    extmod/modjson.c \
    extmod/modbinascii.c \
    extmod/modrandom.c \
    extmod/modtime.c \
    ports/embed/port/replay_bdev.c \
    $(USERMODS_DIR)/modtemporalbadge.c

# The usermod HAL header lives next to the source.
CFLAGS += -I$(USERMODS_DIR)

# Optional extra preprocessor flags for host-side QSTR scanning.
# These are injected by setup_arduino_embed.sh to expose ESP-IDF headers.
ifneq ($(strip $(REPLAY_QSTR_CFLAGS)),)
CFLAGS += $(REPLAY_QSTR_CFLAGS)
endif

ifneq ($(strip $(REPLAY_IDF_INCLUDE_FLAGS)),)
CFLAGS += $(REPLAY_IDF_INCLUDE_FLAGS)
endif

# Include the standard embed makefile (this includes py/mkrules.mk which evaluates SRC_QSTR).
include $(MICROPYTHON_TOP)/ports/embed/embed.mk

# mkrules snapshots QSTR_GEN_CFLAGS := $(CFLAGS) when mkrules.mk is parsed. Some make
# implementations or expansion orders can omit freshly-added command-line flags from that
# snapshot. Re-append REPLAY_QSTR_CFLAGS so host `gcc -E` always sees e.g.
# `-include …/esp_idf_version.h` (needed for extmod/machine_pwm.c → ports/esp32 paths).
ifneq ($(strip $(REPLAY_QSTR_CFLAGS)),)
QSTR_GEN_CFLAGS += $(REPLAY_QSTR_CFLAGS)
endif

# Extend the packaging step to also copy extmod/VFS sources.
.PHONY: micropython-embed-package-replay
micropython-embed-package-replay: micropython-embed-package
	@echo "- extmod (VFS + moductypes)"
	$(Q)$(CP) $(TOP)/extmod/vfs.c $(TOP)/extmod/vfs.h \
	    $(TOP)/extmod/vfs_blockdev.c \
	    $(TOP)/extmod/vfs_fat.c $(TOP)/extmod/vfs_fat.h \
	    $(TOP)/extmod/vfs_fat_diskio.c $(TOP)/extmod/vfs_fat_file.c \
	    $(TOP)/extmod/vfs_reader.c \
	    $(TOP)/extmod/moductypes.c \
	    $(TOP)/extmod/vfs_lfs.h $(TOP)/extmod/vfs_posix.h \
	    $(TOP)/extmod/vfs_rom.h \
	    $(TOP)/extmod/misc.h \
	    $(TOP)/extmod/modvfs.c $(TOP)/extmod/modos.c \
	    $(TOP)/extmod/os_dupterm.c \
	    $(TOP)/extmod/modheapq.c \
	    $(TOP)/extmod/modjson.c \
	    $(TOP)/extmod/modbinascii.c \
	    $(TOP)/extmod/modrandom.c \
	    $(TOP)/extmod/modtime.c $(TOP)/extmod/modtime.h \
	    $(TOP)/extmod/modmachine.c $(TOP)/extmod/modmachine.h \
	    $(TOP)/extmod/modnetwork.c $(TOP)/extmod/modnetwork.h \
	    $(TOP)/extmod/machine_adc.c \
	    $(TOP)/extmod/machine_adc_block.c \
	    $(TOP)/extmod/machine_pwm.c \
	    $(TOP)/extmod/machine_i2c.c \
	    $(TOP)/extmod/machine_i2s.c \
	    $(TOP)/extmod/machine_spi.c \
	    $(TOP)/extmod/machine_uart.c \
		$(TOP)/extmod/machine_bitstream.c \
		$(TOP)/extmod/machine_pinbase.c \
	    $(TOP)/extmod/virtpin.h \
	    $(TOP)/extmod/virtpin.c \
	    $(PACKAGE_DIR)/extmod/
	@echo "- lib/oofatfs"
	$(Q)$(MKDIR) -p $(PACKAGE_DIR)/lib/oofatfs
	$(Q)$(CP) $(TOP)/lib/oofatfs/* $(PACKAGE_DIR)/lib/oofatfs/
	@echo "- shared/timeutils"
	$(Q)$(MKDIR) -p $(PACKAGE_DIR)/shared/timeutils
	$(Q)$(CP) $(TOP)/shared/timeutils/timeutils.h $(TOP)/shared/timeutils/timeutils.c \
	    $(PACKAGE_DIR)/shared/timeutils/
	@echo "- shared/readline"
	$(Q)$(MKDIR) -p $(PACKAGE_DIR)/shared/readline
	$(Q)$(CP) $(TOP)/shared/readline/readline.h $(TOP)/shared/readline/readline.c \
	    $(PACKAGE_DIR)/shared/readline/
	@echo "- shared/runtime (pyexec, interrupt_char, sys_stdio_mphal)"
	$(Q)$(CP) $(TOP)/shared/runtime/pyexec.c $(TOP)/shared/runtime/pyexec.h \
	    $(TOP)/shared/runtime/interrupt_char.h \
	    $(PACKAGE_DIR)/shared/runtime/
	$(Q)test -f $(TOP)/shared/runtime/sys_stdio_mphal.c && \
	    $(CP) $(TOP)/shared/runtime/sys_stdio_mphal.c $(PACKAGE_DIR)/shared/runtime/ || true
	@echo "- shared/netutils"
	$(Q)$(MKDIR) -p $(PACKAGE_DIR)/shared/netutils
	$(Q)$(CP) $(TOP)/shared/netutils/netutils.c $(TOP)/shared/netutils/netutils.h \
	    $(PACKAGE_DIR)/shared/netutils/
	@echo "- shared/runtime (mpirq)"
	$(Q)$(CP) $(TOP)/shared/runtime/mpirq.c $(TOP)/shared/runtime/mpirq.h \
	    $(PACKAGE_DIR)/shared/runtime/
	@echo "- drivers/bus"
	$(Q)$(MKDIR) -p $(PACKAGE_DIR)/drivers/bus
	$(Q)$(CP) $(TOP)/drivers/bus/* $(PACKAGE_DIR)/drivers/bus/
	@echo "- ports/esp32 (machine + wireless staged sources)"
	$(Q)$(MKDIR) -p $(PACKAGE_DIR)/ports/esp32
	$(Q)$(CP) \
	    $(TOP)/ports/esp32/modtime.c \
	    $(TOP)/ports/esp32/modos.c \
	    $(TOP)/ports/esp32/modmachine.c $(TOP)/ports/esp32/modmachine.h \
	    $(TOP)/ports/esp32/modesp32.h \
	    $(TOP)/ports/esp32/machine_pin.c $(TOP)/ports/esp32/machine_pin.h \
	    $(TOP)/ports/esp32/machine_timer.c $(TOP)/ports/esp32/machine_timer.h \
	    $(TOP)/ports/esp32/machine_rtc.c $(TOP)/ports/esp32/machine_rtc.h \
	    $(TOP)/ports/esp32/machine_touchpad.c \
	    $(TOP)/ports/esp32/machine_uart.c $(TOP)/ports/esp32/uart.h \
	    $(TOP)/ports/esp32/machine_i2c.c $(TOP)/ports/esp32/machine_i2c.h \
	    $(TOP)/ports/esp32/machine_hw_spi.c \
	    $(TOP)/ports/esp32/machine_pwm.c \
	    $(TOP)/ports/esp32/machine_wdt.c \
	    $(TOP)/ports/esp32/machine_sdcard.c \
	    $(TOP)/ports/esp32/machine_adc.c $(TOP)/ports/esp32/machine_adc_block.c $(TOP)/ports/esp32/adc.c $(TOP)/ports/esp32/adc.h \
	    $(TOP)/ports/esp32/machine_i2s.c \
	    $(TOP)/ports/esp32/machine_bitstream.c \
	    $(TOP)/ports/esp32/modnetwork.h $(TOP)/ports/esp32/modnetwork_globals.h \
	    $(TOP)/ports/esp32/network_common.c $(TOP)/ports/esp32/network_wlan.c \
	    $(TOP)/ports/esp32/modespnow.c $(TOP)/ports/esp32/modespnow.h \
	    $(PACKAGE_DIR)/ports/esp32/

# Override the default target.
all: micropython-embed-package-replay
