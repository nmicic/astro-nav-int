# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Nenad Micic <nenad@micic.be>
#
# Embedded portability and measurement harness. Everything here is
# opt-in tooling: the library itself needs only the top-level Makefile.
#
#   make all       cross-build every profile for every target, plus host
#   make check     host-vs-target hash equality: the cross-ISA
#                  bit-exactness gate (runs every image under QEMU)
#   make plugin    build the QEMU instruction-count plugin (needs glib
#                  and qemu-plugin.h; see QEMU_PLUGIN_INC)
#   make measure   sizes, stack, libgcc imports, FP-opcode audit,
#                  dynamic instruction counts (runs ./measure.sh)
#   run-<t>-<p>    run one image, e.g. make run-rv32i-moon
#   insns-<t>-<p>  same with the instruction-count plugin loaded
#
# Targets: m0 (Armv6-M; built M0+, run on QEMU microbit Cortex-M0),
#          m3 (Cortex-M3, mps2-an385),
# m4 (Cortex-M4 soft-float, mps2-an386), rv32i and rv32im (QEMU virt,
# -bios none). Profiles: baseline core fix sun moon all.
#
# Requires arm-none-eabi-gcc, riscv64-elf-gcc (with rv32 multilibs),
# qemu-system-arm, qemu-system-riscv32. Override tool names below, or
# narrow the matrix: make TARGETS=rv32im check.

ARM_CC   ?= arm-none-eabi-gcc
RV_CC    ?= riscv64-elf-gcc
HOST_CC  ?= cc
QEMU_ARM ?= qemu-system-arm
QEMU_RV  ?= qemu-system-riscv32

# Guard against a wedged emulator hanging the matrix.
TIMEOUT ?= perl -e 'alarm 120; exec @ARGV'

PROFILES ?= baseline core fix sun moon all
TARGETS  ?= m0 m3 m4 rv32i rv32im

OPT ?= -O2
CFLAGS = -std=c99 $(OPT) -g -Wall -Wextra -ffreestanding \
         -ffunction-sections -fdata-sections -fstack-usage -I.. -Iharness

CC_m0        := $(ARM_CC)
ARCH_m0      := -mcpu=cortex-m0plus -mthumb
LDS_m0       := microbit.ld
START_m0     := start_arm.c
QEMU_RUN_m0  := $(QEMU_ARM) -M microbit

CC_m3        := $(ARM_CC)
ARCH_m3      := -mcpu=cortex-m3 -mthumb
LDS_m3       := mps2.ld
START_m3     := start_arm.c
QEMU_RUN_m3  := $(QEMU_ARM) -M mps2-an385

CC_m4        := $(ARM_CC)
ARCH_m4      := -mcpu=cortex-m4 -mthumb -mfloat-abi=soft
LDS_m4       := mps2.ld
START_m4     := start_arm.c
QEMU_RUN_m4  := $(QEMU_ARM) -M mps2-an386

CC_rv32i       := $(RV_CC)
ARCH_rv32i     := -march=rv32i -mabi=ilp32
LDS_rv32i      := rv32_virt.ld
START_rv32i    := start_rv32.c
QEMU_RUN_rv32i := $(QEMU_RV) -M virt -bios none
# Everything lives in the virt machine's one RAM region, so the single
# RWX LOAD segment the linker warns about is the layout, not a bug.
LDEXTRA_rv32i  := -Wl,--no-warn-rwx-segments

CC_rv32im       := $(RV_CC)
ARCH_rv32im     := -march=rv32im -mabi=ilp32
LDS_rv32im      := rv32_virt.ld
START_rv32im    := start_rv32.c
QEMU_RUN_rv32im := $(QEMU_RV) -M virt -bios none
LDEXTRA_rv32im  := -Wl,--no-warn-rwx-segments

QEMU_FLAGS = -semihosting -nographic -monitor none -serial none

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
PLUGIN_EXT       := dylib
PLUGIN_LDFLAGS   := -dynamiclib -undefined dynamic_lookup
QEMU_PLUGIN_INC  ?= /opt/homebrew/include
else
PLUGIN_EXT       := so
PLUGIN_LDFLAGS   := -shared -fPIC
QEMU_PLUGIN_INC  ?= /usr/include
endif
GLIB_CFLAGS := $(shell pkg-config --cflags glib-2.0 2>/dev/null)

ALL_ELFS  := $(foreach t,$(TARGETS),\
               $(foreach p,$(PROFILES),build/$(t)/profile_$(p).elf))
HOST_BINS := $(foreach p,$(PROFILES),build/host/profile_$(p))

all: $(ALL_ELFS) $(HOST_BINS)

# Keep pattern-rule intermediates: measure.sh runs nm over the .o files.
.SECONDARY:

# One rule set per target. $$ survives the eval; $(1) is the target.
define TGT_RULES
build/$(1):
	mkdir -p $$@

build/$(1)/astro_nav.o: ../astro_nav.c ../astro_nav.h ../fp_math.h | build/$(1)
	$$(CC_$(1)) $$(CFLAGS) $$(ARCH_$(1)) -c ../astro_nav.c -o $$@

build/$(1)/start.o: harness/$$(START_$(1)) harness/semihost.h | build/$(1)
	$$(CC_$(1)) $$(CFLAGS) $$(ARCH_$(1)) -c harness/$$(START_$(1)) -o $$@

build/$(1)/harness_main.o: harness/harness_main.c harness/semihost.h \
                           harness/harness.h | build/$(1)
	$$(CC_$(1)) $$(CFLAGS) $$(ARCH_$(1)) -c harness/harness_main.c -o $$@

build/$(1)/libc_min.o: harness/libc_min.c | build/$(1)
	$$(CC_$(1)) $$(CFLAGS) $$(ARCH_$(1)) -fno-builtin \
	    -fno-tree-loop-distribute-patterns -c harness/libc_min.c -o $$@

build/$(1)/profile_%.o: profiles/profile_%.c harness/harness.h \
                        ../astro_nav.h | build/$(1)
	$$(CC_$(1)) $$(CFLAGS) $$(ARCH_$(1)) -c $$< -o $$@

build/$(1)/profile_%.elf: build/$(1)/profile_%.o build/$(1)/astro_nav.o \
                          build/$(1)/harness_main.o build/$(1)/start.o \
                          build/$(1)/libc_min.o harness/$$(LDS_$(1))
	$$(CC_$(1)) $$(ARCH_$(1)) -nostartfiles -nostdlib -Wl,--gc-sections \
	    -Wl,-Map=$$@.map $$(LDEXTRA_$(1)) -T harness/$$(LDS_$(1)) \
	    $$(filter %.o,$$^) -lgcc -o $$@

run-$(1)-%: build/$(1)/profile_%.elf
	$$(TIMEOUT) $$(QEMU_RUN_$(1)) $$(QEMU_FLAGS) -kernel $$<

insns-$(1)-%: build/$(1)/profile_%.elf build/insncount.$$(PLUGIN_EXT)
	$$(TIMEOUT) $$(QEMU_RUN_$(1)) $$(QEMU_FLAGS) -kernel $$< \
	    -plugin ./build/insncount.$$(PLUGIN_EXT) -d plugin

check-$(1)-%: build/$(1)/profile_%.elf build/host/profile_%
	@host="$$$$(./build/host/profile_$$* | grep '^PROFILE ')"; \
	tgt="$$$$($$(TIMEOUT) $$(QEMU_RUN_$(1)) $$(QEMU_FLAGS) \
	          -kernel build/$(1)/profile_$$*.elf 2>&1 \
	          | grep '^PROFILE ' | tr -d '\r')"; \
	if [ "$$$$host" = "$$$$tgt" ] && [ -n "$$$$host" ]; then \
	    printf 'OK   %-7s %s\n' '$(1)' "$$$$tgt"; \
	else \
	    printf 'FAIL %-7s host="%s" target="%s"\n' \
	        '$(1)' "$$$$host" "$$$$tgt"; exit 1; \
	fi
endef

$(foreach t,$(TARGETS),$(eval $(call TGT_RULES,$(t))))

build/host:
	mkdir -p $@

build/host/profile_%: profiles/profile_%.c harness/host_main.c \
                      ../astro_nav.c ../astro_nav.h ../fp_math.h \
                      harness/harness.h | build/host
	$(HOST_CC) -std=c99 $(OPT) -Wall -Wextra -I.. -Iharness \
	    profiles/profile_$*.c harness/host_main.c ../astro_nav.c -o $@

run-host-%: build/host/profile_%
	./$<

# The cross-ISA bit-exactness gate: every target image must print the
# exact PROFILE line the host build prints.
CHECKS := $(foreach t,$(TARGETS),$(foreach p,$(PROFILES),check-$(t)-$(p)))
check: $(CHECKS)
	@echo "embedded check: all target hashes match the host"

plugin: build/insncount.$(PLUGIN_EXT)

build/insncount.$(PLUGIN_EXT): tools/insncount.c | build/host
	$(HOST_CC) -O2 $(PLUGIN_LDFLAGS) -I$(QEMU_PLUGIN_INC) $(GLIB_CFLAGS) \
	    tools/insncount.c -o $@

measure:
	./measure.sh

clean:
	rm -rf build

# The check-<t>-<p> names stay OUT of .PHONY: phony targets are skipped
# by pattern-rule search, which would silently skip the whole matrix.
# They create no files, so they re-run every time regardless.
.PHONY: all check plugin measure clean
