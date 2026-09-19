# QwenChess build -- single Makefile, C17.
#
#   make            build the release engine      (build/release/)
#   make debug      build the debug engine        (build/debug/)
#   make sanitize   build the ASan+UBSan engine   (build/sanitize/)
#   make test       build + run the scaffold test (release flags)
#   make test-sanitize  build + run the test with ASan/UBSan (real instrumentation)
#   make net        fetch + verify the NNUE network into networks/
#   make test-net   run the net-checker self-test (negative + positive control)
#   make clean      remove all build output
#
# Clang is optional: `make CC=clang`. Each CONFIG uses its own object dir so
# configurations can never accidentally reuse incompatible objects.

CC       ?= gcc
CONFIG   ?= release
NET_DIR  ?= networks
CPPFLAGS := -Isrc
LDFLAGS  :=

WARN := -Wall -Wextra -Wshadow -Wconversion -Wstrict-prototypes -Wwrite-strings

CFLAGS_RELEASE := -O2 -g -march=native
CFLAGS_DEBUG   := -O0 -g
CFLAGS_SANIT   := -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer

ifneq ($(CONFIG),release)
  ifneq ($(CONFIG),debug)
    CFLAGS := $(CFLAGS_SANIT)
  else
    CFLAGS := $(CFLAGS_DEBUG)
  endif
else
  CFLAGS := $(CFLAGS_RELEASE)
endif

SRC := src
OBJ := build/$(CONFIG)/obj
BIN := build/$(CONFIG)/bin

# The shipped engine (scaffold). Objects mirror the source tree under obj/src/.
ENGINE_SRC := $(SRC)/engine/main.c $(SRC)/platform/clock.c
ENGINE_OBJ := $(addprefix $(OBJ)/,$(ENGINE_SRC:$(SRC)/%.c=%.o))
ENGINE_BIN := $(BIN)/qwenchess

# The scaffold test reuses the clock implementation; its own object is built
# from tests/. (Add engine objects here in later tasks as the test grows.)
TEST_SRC   := $(SRC)/platform/clock.c
TEST_OBJ   := $(addprefix $(OBJ)/,$(TEST_SRC:$(SRC)/%.c=%.o)) $(OBJ)/tests/scaffold_test.o
TEST_BIN   := $(BIN)/scaffold_test

DEPS := $(ENGINE_OBJ:.o=.d) $(TEST_OBJ:.o=.d)

.SUFFIXES:
.PHONY: all release debug sanitize test test-sanitize net test-net clean help

all: release

release:
	$(MAKE) CONFIG=release all-config
debug:
	$(MAKE) CONFIG=debug all-config
sanitize:
	$(MAKE) CONFIG=sanitize all-config

# Per-configuration build of the engine binary.
all-config: $(ENGINE_BIN)

$(ENGINE_BIN): $(ENGINE_OBJ) | $(BIN)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(ENGINE_OBJ)
	@echo "built $@ ($(CONFIG))"

# Pattern rule for sources under src/ (maps to a parallel obj/ tree).
$(OBJ)/%.o: src/%.c | $(OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -MMD -MP -c $< -o $@

# The scaffold test lives in tests/, not src/; explicit rule.
$(OBJ)/tests/scaffold_test.o: tests/scaffold_test.c | $(OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -MMD -MP -c $< -o $@

$(OBJ) $(BIN):
	mkdir -p $@

# Per-configuration build + run of the test. `test` runs under release flags;
# `test-sanitize` runs the ASan/UBSan-instrumented binary for real.
$(TEST_BIN): $(TEST_OBJ) | $(BIN)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(TEST_OBJ)

test:
	$(MAKE) CONFIG=release test-run
test-sanitize:
	$(MAKE) CONFIG=sanitize test-run
test-run: $(TEST_BIN)
	@echo "running $@ ($(CONFIG))"; $(TEST_BIN)

# Network fetch + verification (offline-safe build never depends on this).
net:
	$(SHELL) tools/net_fetch.sh fetch
test-net:
	$(SHELL) tools/net_test.sh

clean:
	rm -rf build

help:
	@sed -n '2,12p' $(MAKEFILE_LIST)

-include $(DEPS)
