# SPDX-License-Identifier: GPL-3.0-only
#
# Minimal devkitPPC Makefile for the stub. Build with:  make
#
# Prerequisites (see README): devkitPro installed, and in your shell
#   export DEVKITPRO=/opt/devkitpro
#   export DEVKITPPC=${DEVKITPRO}/devkitPPC
#
# `wii_rules` (shipped with devkitPPC) provides the cross-compiler prefix,
# the Wii-specific flags in $(MACHDEP), the libogc paths, and the rule that
# turns an .elf into a .dol using the `elf2dol` tool.
#
# NOTE: this Makefile was written without access to devkitPPC. If it
# misbehaves, diff it against devkitPro's own wii "template" Makefile; the
# variable names are deliberately the same.

ifeq ($(strip $(DEVKITPPC)),)
$(error "DEVKITPPC is not set. Install devkitPro and export DEVKITPRO/DEVKITPPC (see README)")
endif
include $(DEVKITPPC)/wii_rules

TARGET   := wiidbg-stub
SOURCES  := source ../common
INCLUDES := include ../common
BUILD    := build

CFILES := $(foreach d,$(SOURCES),$(notdir $(wildcard $(d)/*.c)))
OFILES := $(addprefix $(BUILD)/,$(CFILES:.c=.o))
vpath %.c $(SOURCES)

# MACHDEP = -DGEKKO -mrvl -mcpu=750 -meabi -mhard-float
#   GEKKO  : macro libogc headers check for
#   -mrvl  : target the Wii ("Revolution") CPU
#   -mcpu=750 : PowerPC 750 core family (Broadway)
CFLAGS  := -g -O2 -Wall -Wextra -std=gnu11 $(MACHDEP) $(addprefix -I,$(INCLUDES)) -I$(LIBOGC_INC)
LDFLAGS := -g $(MACHDEP) -Wl,-Map,$(BUILD)/$(TARGET).map
LIBS    := -lwiiuse -lbte -logc -lm
LIBDIRS := -L$(LIBOGC_LIB)

.PHONY: all clean run deploy
all: $(TARGET).dol

$(BUILD):
	@mkdir -p $@

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET).elf: $(OFILES)
	$(CC) $(LDFLAGS) $^ $(LIBDIRS) $(LIBS) -o $@

# Send to a Wii that is sitting in the Homebrew Channel (needs WIILOAD set,
# e.g. export WIILOAD=tcp:192.168.1.50).
deploy: $(TARGET).dol
	wiiload $(TARGET).dol

# Run in the Dolphin emulator.
run: $(TARGET).dol
	dolphin-emu -e $(TARGET).dol

clean:
	rm -rf $(BUILD) $(TARGET).elf $(TARGET).dol
