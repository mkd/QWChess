# QwenChess build -- single Makefile, C17.
#
#   make            build the release engine      (build/release/)
#   make debug      build the debug engine        (build/debug/)
#   make sanitize   build the ASan+UBSan engine   (build/sanitize/)
#   make test       build + run the tests (release flags)
#   make test-sanitize  build + run the test with ASan/UBSan (real instrumentation)
#   make check-c17  strict -std=c17 -pedantic-errors conformance gate over all sources
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

# One shared language standard. Used by BOTH the ordinary builds (release/debug/
# sanitize) and the strict check-c17 gate, so the shipped engine is compiled as
# C17 -- not merely *checked* as C17.
CSTD := -std=c17

WARN := -Wall -Wextra -Wshadow -Wconversion -Wstrict-prototypes -Wwrite-strings

# Strict standard-compliance gate: the shared C17 standard plus -pedantic-errors,
# which turns any non-ISO-C17 construct (e.g. C23 fixed-base enums, GNU
# extensions) into a hard error.
STRICT_C17 := $(CSTD) -pedantic-errors

CFLAGS_RELEASE := $(CSTD) -O2 -g -march=native
CFLAGS_DEBUG   := $(CSTD) -O0 -g
CFLAGS_SANIT   := $(CSTD) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer

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

# Every source file (core + platform + engine + tests) for the strict-C17 check.
ALL_SRC := $(wildcard $(SRC)/*.c $(SRC)/*/*.c tests/*.c)

# The shipped engine (scaffold). Objects mirror the source tree under obj/src/.
ENGINE_SRC := $(SRC)/engine/main.c $(SRC)/platform/clock.c
ENGINE_OBJ := $(addprefix $(OBJ)/,$(ENGINE_SRC:$(SRC)/%.c=%.o))
ENGINE_BIN := $(BIN)/qwenchess

# Test binaries. Each test is a small self-contained program in tests/; the
# scaffold test also links the clock implementation. One binary per test group.
SCAFFOLD_TEST_OBJ := $(OBJ)/platform/clock.o $(OBJ)/tests/scaffold_test.o
SCAFFOLD_TEST_BIN := $(BIN)/scaffold_test
SQBB_TEST_OBJ     := $(OBJ)/tests/sq_bb_test.o
SQBB_TEST_BIN     := $(BIN)/sq_bb_test
MOVE_TEST_OBJ     := $(OBJ)/tests/move_test.o
MOVE_TEST_BIN     := $(BIN)/move_test
ATTACKS_TEST_OBJ  := $(OBJ)/core/attacks.o $(OBJ)/tests/attacks_test.o
ATTACKS_TEST_BIN  := $(BIN)/attacks_test
ZOB_TEST_OBJ      := $(OBJ)/core/zobrist.o $(OBJ)/tests/zobrist_test.o
ZOB_TEST_BIN      := $(BIN)/zobrist_test
POS_TEST_OBJ      := $(OBJ)/position/position.o $(OBJ)/tests/position_test.o $(OBJ)/core/zobrist.o $(OBJ)/core/attacks.o
POS_TEST_BIN      := $(BIN)/position_test
FEN_TEST_OBJ      := $(OBJ)/position/fen.o $(OBJ)/position/position.o $(OBJ)/tests/fen_test.o $(OBJ)/core/zobrist.o $(OBJ)/core/attacks.o
FEN_TEST_BIN      := $(BIN)/fen_test
BOUNDARY_TEST_OBJ := $(OBJ)/tests/position_boundary_test.o $(OBJ)/position/fen.o $(OBJ)/position/position.o $(OBJ)/core/zobrist.o $(OBJ)/core/attacks.o
BOUNDARY_TEST_BIN := $(BIN)/position_boundary_test
MAKEUNMAKE_TEST_OBJ := $(OBJ)/tests/makeunmake_test.o $(OBJ)/position/state.o $(OBJ)/position/fen.o $(OBJ)/position/position.o $(OBJ)/core/zobrist.o $(OBJ)/core/attacks.o
MAKEUNMAKE_TEST_BIN := $(BIN)/makeunmake_test
TEST_BINS := $(SCAFFOLD_TEST_BIN) $(SQBB_TEST_BIN) $(MOVE_TEST_BIN) $(ATTACKS_TEST_BIN) $(ZOB_TEST_BIN) $(POS_TEST_BIN) $(FEN_TEST_BIN) $(BOUNDARY_TEST_BIN) $(MAKEUNMAKE_TEST_BIN)

DEPS := $(ENGINE_OBJ:.o=.d) $(SCAFFOLD_TEST_OBJ:.o=.d) $(SQBB_TEST_OBJ:.o=.d) $(MOVE_TEST_OBJ:.o=.d) $(ATTACKS_TEST_OBJ:.o=.d) $(ZOB_TEST_OBJ:.o=.d) $(POS_TEST_OBJ:.o=.d) $(FEN_TEST_OBJ:.o=.d) $(BOUNDARY_TEST_OBJ:.o=.d) $(MAKEUNMAKE_TEST_OBJ:.o=.d)

.SUFFIXES:
.PHONY: all release debug sanitize test test-sanitize check-c17 net test-net clean help

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

# Test objects live in tests/ (not src/); one pattern rule covers all of them.
$(OBJ)/tests/%.o: tests/%.c | $(OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -MMD -MP -c $< -o $@

$(OBJ) $(BIN):
	mkdir -p $@

# Per-configuration build + run of the tests. `test` runs under release flags;
# `test-sanitize` runs the ASan/UBSan-instrumented binaries for real.
$(SCAFFOLD_TEST_BIN): $(SCAFFOLD_TEST_OBJ) | $(BIN)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SCAFFOLD_TEST_OBJ)

$(SQBB_TEST_BIN): $(SQBB_TEST_OBJ) | $(BIN)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SQBB_TEST_OBJ)

$(MOVE_TEST_BIN): $(MOVE_TEST_OBJ) | $(BIN)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(MOVE_TEST_OBJ)

$(ATTACKS_TEST_BIN): $(ATTACKS_TEST_OBJ) | $(BIN)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(ATTACKS_TEST_OBJ)

$(ZOB_TEST_BIN): $(ZOB_TEST_OBJ) | $(BIN)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(ZOB_TEST_OBJ)

$(POS_TEST_BIN): $(POS_TEST_OBJ) | $(BIN)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(POS_TEST_OBJ)

$(FEN_TEST_BIN): $(FEN_TEST_OBJ) | $(BIN)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FEN_TEST_OBJ)

$(BOUNDARY_TEST_BIN): $(BOUNDARY_TEST_OBJ) | $(BIN)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(BOUNDARY_TEST_OBJ)

$(MAKEUNMAKE_TEST_BIN): $(MAKEUNMAKE_TEST_OBJ) | $(BIN)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(MAKEUNMAKE_TEST_OBJ)

test:
	$(MAKE) CONFIG=release test-run
test-sanitize:
	$(MAKE) CONFIG=sanitize test-run
test-run: $(TEST_BINS)
	@fail=0; for b in $(TEST_BINS); do echo "running $$(basename $$b) ($(CONFIG))"; ./$$b || fail=1; done; exit $$fail

# Strict C17 conformance gate: compile every source with -std=c17 -pedantic-errors
# (syntax-only, no object output). $(CPPFLAGS) carries only include flags and no
# -std, and it precedes $(STRICT_C17), so the C17 standard is set exactly once and
# no later option can override it. Independent of the per-CONFIG build flags.
check-c17:
	@echo "strict C17 check [$(CC) -std=c17 -pedantic-errors] over $(words $(ALL_SRC)) files"
	@set -e; for f in $(ALL_SRC); do \
	  printf '  c17  -fsyntax-only  %s\n' "$$f"; \
	  $(CC) $(CPPFLAGS) $(STRICT_C17) -fsyntax-only $$f; \
	done; \
	echo "strict C17: OK ($(words $(ALL_SRC)) files, no non-C17 constructs)"

# Network fetch + verification (offline-safe build never depends on this).
net:
	$(SHELL) tools/net_fetch.sh fetch
test-net:
	$(SHELL) tools/net_test.sh

clean:
	rm -rf build

help:
	@sed -n '2,13p' $(MAKEFILE_LIST)

-include $(DEPS)
