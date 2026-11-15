# Vectrex-PiTrex: um2run (support/pitrex/um2run, a copy of Vectrex_MiSTer's pitrex/um2run) built into
# this Main as its worker mode (main-pitrex --um2run ..., support/pitrex/pitrex_worker.cpp).
# It is compiled with its own flags (as its build.sh does) and linked into one object in which only
# um2run_main is global, so none of its names can meet Main's.

# included before Main's own link rule: keep that the default goal
.DEFAULT_GOAL := $(BUILDDIR)/$(PRJ)

OBJCOPY  ?= $(BASE)-objcopy

UM2_DIR   = support/pitrex/um2run
UM2_SRC   = $(addprefix $(UM2_DIR)/, um2run.c um2bus.c um2sd.c um2aud.c um2clock.c ptx_core.c ptx_backend_file.c ptx_backend_mister.c)
UM2_OBJ   = $(UM2_SRC:%.c=$(BUILDDIR)/%.um2.o)
UM2_ALL   = $(BUILDDIR)/$(UM2_DIR)/um2run_all.o
UM2_CFLAGS = -O2 -Wall -Wextra -Wno-unused-parameter -pthread -I$(UM2_DIR) -Dmain=um2run_main

# um2run's speed depends on the compiler: on the MiSTer (15 Nov 2026) the same source built with gcc 10.2 (this
# toolchain) ran games 5-9 % slower than with gcc 14. UM2_CC names another C compiler for um2run alone; UM2_INC then
# points it at this toolchain's C library headers, so it links against the MiSTer's glibc, e.g.
#   make UM2_CC=arm-linux-gnueabihf-gcc UM2_INC="-nostdinc -isystem <gcc 14 include> -isystem <this toolchain>/arm-none-linux-gnueabihf/libc/usr/include"
UM2_CC   ?= $(CC)
UM2_INC  ?=

$(BUILDDIR)/$(UM2_DIR)/%.um2.o: $(UM2_DIR)/%.c $(wildcard $(UM2_DIR)/*.h)
	@mkdir -p $(dir $@)
	$(Q)$(info $<)
	$(Q)$(UM2_CC) $(UM2_INC) $(UM2_CFLAGS) -c -o $@ $< 2>&1 | $(OUTPUT_FILTER)

$(UM2_ALL): $(UM2_OBJ)
	$(Q)$(info $@)
	$(Q)$(LD) -r -o $@.tmp $^
	$(Q)$(OBJCOPY) --keep-global-symbol=um2run_main $@.tmp $@
	$(Q)rm -f $@.tmp

OBJ += $(UM2_ALL)
