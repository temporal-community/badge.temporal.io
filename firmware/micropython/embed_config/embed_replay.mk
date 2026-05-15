# Replay badge: extends MicroPython ports/embed/embed.mk so qstr/module generation
# includes extra extmod sources we ship alongside the stock embed package.

# Set the build output directory for the generated files.
BUILD = build-embed

# Include the core environment definitions; this will set $(TOP).
include $(MICROPYTHON_TOP)/py/mkenv.mk

# Include py core make definitions.
include $(TOP)/py/py.mk

# Stock embed does not include extmod.mk, so SRC_QSTR misses uctypes (and any other
# extmod .c we add). Keep paths relative to $(TOP) like extmod/extmod.mk.
SRC_QSTR += extmod/moductypes.c
# sys_stdio_mphal.c defines stdio wrapper types (MP_QSTR_TextIOWrapper, FileIO, ...).
SRC_QSTR += shared/runtime/sys_stdio_mphal.c

# Set the location of the MicroPython embed port.
MICROPYTHON_EMBED_PORT = $(MICROPYTHON_TOP)/ports/embed

# Set default makefile-level MicroPython feature configurations.
MICROPY_ROM_TEXT_COMPRESSION ?= 0

# Set CFLAGS for the MicroPython build.
CFLAGS += -I. -I$(TOP) -I$(BUILD) -I$(MICROPYTHON_EMBED_PORT)
CFLAGS += -Wall -Werror -std=c99

# Define the required generated header files.
GENHDR_OUTPUT = $(addprefix $(BUILD)/genhdr/, \
	moduledefs.h \
	mpversion.h \
	qstrdefs.generated.h \
	root_pointers.h \
	)

# Define the top-level target, the generated output files.
.PHONY: all
all: micropython-embed-package

clean: clean-micropython-embed-package

.PHONY: clean-micropython-embed-package
clean-micropython-embed-package:
	$(RM) -rf $(PACKAGE_DIR)

PACKAGE_DIR ?= micropython_embed
PACKAGE_DIR_LIST = $(addprefix $(PACKAGE_DIR)/,py extmod shared/runtime genhdr port)

.PHONY: micropython-embed-package
micropython-embed-package: $(GENHDR_OUTPUT)
	$(ECHO) "Generate micropython_embed output:"
	$(Q)$(RM) -rf $(PACKAGE_DIR_LIST)
	$(Q)$(MKDIR) -p $(PACKAGE_DIR_LIST)
	$(ECHO) "- py"
	$(Q)$(CP) $(TOP)/py/*.[ch] $(PACKAGE_DIR)/py
	$(ECHO) "- extmod"
	$(Q)$(CP) $(TOP)/extmod/modplatform.h $(PACKAGE_DIR)/extmod
	$(ECHO) "- shared"
	$(Q)$(CP) $(TOP)/shared/runtime/gchelper.h $(PACKAGE_DIR)/shared/runtime
	$(Q)$(CP) $(TOP)/shared/runtime/gchelper_generic.c $(PACKAGE_DIR)/shared/runtime
	$(ECHO) "- genhdr"
	$(Q)$(CP) $(GENHDR_OUTPUT) $(PACKAGE_DIR)/genhdr
	$(ECHO) "- port"
	$(Q)$(CP) $(MICROPYTHON_EMBED_PORT)/port/*.[ch] $(PACKAGE_DIR)/port

# Include remaining core make rules.
include $(TOP)/py/mkrules.mk
