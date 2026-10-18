# QwenChess build -- single Makefile, C++20.
#
#   make            build the release engine      (build/cpp20/release/)
#   make debug      build the debug engine        (build/cpp20/debug/)
#   make sanitize   build the ASan+UBSan engine   (build/cpp20/sanitize/)
#   make test       build + run the tests (honors CONFIG; release by default)
#   make test CONFIG=debug|sanitize|release   build + run the tests in that config
#   make test-sanitize  alias for the ASan/UBSan run (real instrumentation)
#   make test-portable-bits  force the non-<bit> fallback in the bitboard + attacks tests
#   make check-cpp20  strict -std=c++20 -pedantic-errors conformance gate over all sources
#   make check-c      legacy alias for check-cpp20 (the strict C++20 conformance gate)
#   make net        fetch + verify the NNUE network into networks/
#   make test-net   run the net-checker self-test (negative + positive control)
#   make clean      remove all build output
#
# Clang is optional: `make CXX=clang++`. Each CONFIG uses its own object dir so
# configurations can never accidentally reuse incompatible objects. Migrated
# artifacts live under build/cpp20/<config>/, separate from any older C build.

CXX      ?= g++
CONFIG   ?= release
NET_DIR  ?= networks
CPPFLAGS := -Isrc
LDFLAGS  :=

# One shared language standard. Used by BOTH the ordinary builds (release/debug/
# sanitize) and the strict check-cpp20 gate, so the shipped engine is compiled as
# C++20 -- not merely *checked* as C++20.
CPPSTD := -std=c++20

WARN := -Wall -Wextra -Wshadow -Wconversion -Wwrite-strings

# Strict standard-compliance gate: the shared C++20 standard plus -pedantic-errors,
# which turns any non-ISO-C++20 construct (GNU extensions, C-only syntax, etc.)
# into a hard error.
STRICT_CPP20 := $(CPPSTD) -pedantic-errors

CXXFLAGS_RELEASE := $(CPPSTD) -O2 -g -march=native
CXXFLAGS_DEBUG   := $(CPPSTD) -O0 -g
CXXFLAGS_SANIT   := $(CPPSTD) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer

# CONFIG is exactly one of release/debug/sanitize. Any other value is rejected
# with a clear error instead of silently falling into the sanitizer flags (the
# old `else` branch treated every unknown CONFIG as a sanitizer build).
VALID_CONFIGS := release debug sanitize
ifneq ($(filter $(CONFIG),$(VALID_CONFIGS)),)
  ifneq ($(CONFIG),release)
    ifneq ($(CONFIG),debug)
      CXXFLAGS := $(CXXFLAGS_SANIT)   # the only valid value left (validated above)
    else
      CXXFLAGS := $(CXXFLAGS_DEBUG)
    endif
  else
    CXXFLAGS := $(CXXFLAGS_RELEASE)
  endif
else
  $(error Unknown CONFIG '$(CONFIG)'; expected one of: $(VALID_CONFIGS))
endif

SRC := src
OBJ := build/cpp20/$(CONFIG)/obj
BIN := build/cpp20/$(CONFIG)/bin

# Every source file (core + platform + engine + movegen + tests) for the
# strict-C++20 check.
ALL_SRC := $(wildcard $(SRC)/*.cpp $(SRC)/*/*.cpp tests/*.cpp)

# The shipped engine (scaffold). Objects mirror the source tree under obj/src/.
ENGINE_SRC := $(SRC)/engine/main.cpp $(SRC)/platform/clock.cpp
ENGINE_OBJ := $(addprefix $(OBJ)/,$(ENGINE_SRC:$(SRC)/%.cpp=%.o))
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
EP_TEST_OBJ         := $(OBJ)/tests/ep_makeunmake_test.o $(OBJ)/position/state.o $(OBJ)/position/fen.o $(OBJ)/position/position.o $(OBJ)/core/zobrist.o $(OBJ)/core/attacks.o
EP_TEST_BIN         := $(BIN)/ep_makeunmake_test
CASTLING_TEST_OBJ   := $(OBJ)/tests/castling_makeunmake_test.o $(OBJ)/position/state.o $(OBJ)/position/fen.o $(OBJ)/position/position.o $(OBJ)/core/zobrist.o $(OBJ)/core/attacks.o
CASTLING_TEST_BIN   := $(BIN)/castling_makeunmake_test
PROMO_TEST_OBJ      := $(OBJ)/tests/promotion_makeunmake_test.o $(OBJ)/position/state.o $(OBJ)/position/fen.o $(OBJ)/position/position.o $(OBJ)/core/zobrist.o $(OBJ)/core/attacks.o
PROMO_TEST_BIN      := $(BIN)/promotion_makeunmake_test
NULL_TEST_OBJ       := $(OBJ)/tests/null_makeunmake_test.o $(OBJ)/position/state.o $(OBJ)/position/fen.o $(OBJ)/position/position.o $(OBJ)/core/zobrist.o $(OBJ)/core/attacks.o
NULL_TEST_BIN       := $(BIN)/null_makeunmake_test
REPETITION_TEST_OBJ := $(OBJ)/tests/repetition_test.o $(OBJ)/position/state.o $(OBJ)/position/fen.o $(OBJ)/position/position.o $(OBJ)/core/zobrist.o $(OBJ)/core/attacks.o
REPETITION_TEST_BIN := $(BIN)/repetition_test
MOVEGEN_TEST_OBJ    := $(OBJ)/movegen/movegen.o $(OBJ)/tests/movegen_pseudo_test.o $(OBJ)/position/state.o $(OBJ)/position/fen.o $(OBJ)/position/position.o $(OBJ)/core/zobrist.o $(OBJ)/core/attacks.o
MOVEGEN_TEST_BIN    := $(BIN)/movegen_pseudo_test
TEST_BINS := $(SCAFFOLD_TEST_BIN) $(SQBB_TEST_BIN) $(MOVE_TEST_BIN) $(ATTACKS_TEST_BIN) $(ZOB_TEST_BIN) $(POS_TEST_BIN) $(FEN_TEST_BIN) $(BOUNDARY_TEST_BIN) $(MAKEUNMAKE_TEST_BIN) $(EP_TEST_BIN) $(CASTLING_TEST_BIN) $(PROMO_TEST_BIN) $(NULL_TEST_BIN) $(REPETITION_TEST_BIN) $(MOVEGEN_TEST_BIN)

DEPS := $(ENGINE_OBJ:.o=.d) $(SCAFFOLD_TEST_OBJ:.o=.d) $(SQBB_TEST_OBJ:.o=.d) $(MOVE_TEST_OBJ:.o=.d) $(ATTACKS_TEST_OBJ:.o=.d) $(ZOB_TEST_OBJ:.o=.d) $(POS_TEST_OBJ:.o=.d) $(FEN_TEST_OBJ:.o=.d) $(BOUNDARY_TEST_OBJ:.o=.d) $(MAKEUNMAKE_TEST_OBJ:.o=.d) $(EP_TEST_OBJ:.o=.d) $(CASTLING_TEST_OBJ:.o=.d) $(PROMO_TEST_OBJ:.o=.d) $(NULL_TEST_OBJ:.o=.d) $(REPETITION_TEST_OBJ:.o=.d) $(MOVEGEN_TEST_OBJ:.o=.d)

.SUFFIXES:
.PHONY: all release debug sanitize test test-sanitize test-run test-portable-bits all-config check-c check-cpp20 net test-net clean help

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
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $(ENGINE_OBJ)
	@echo "built $@ ($(CONFIG))"

# Pattern rule for sources under src/ (maps to a parallel obj/ tree).
$(OBJ)/%.o: src/%.cpp | $(OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(WARN) -MMD -MP -c $< -o $@

# Test objects live in tests/ (not src/); one pattern rule covers all of them.
$(OBJ)/tests/%.o: tests/%.cpp | $(OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(WARN) -MMD -MP -c $< -o $@

$(OBJ) $(BIN):
	mkdir -p $@

# Per-configuration test binaries: built + linked with the current CONFIG's flags
# (CXXFLAGS above) into build/cpp20/<config>/bin/. `test` / `test-sanitize` build
# and run them (see the `test` target below for how CONFIG is honored).
$(SCAFFOLD_TEST_BIN): $(SCAFFOLD_TEST_OBJ) | $(BIN)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $(SCAFFOLD_TEST_OBJ)

$(SQBB_TEST_BIN): $(SQBB_TEST_OBJ) | $(BIN)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $(SQBB_TEST_OBJ)

$(MOVE_TEST_BIN): $(MOVE_TEST_OBJ) | $(BIN)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $(MOVE_TEST_OBJ)

$(ATTACKS_TEST_BIN): $(ATTACKS_TEST_OBJ) | $(BIN)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $(ATTACKS_TEST_OBJ)

$(ZOB_TEST_BIN): $(ZOB_TEST_OBJ) | $(BIN)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $(ZOB_TEST_OBJ)

$(POS_TEST_BIN): $(POS_TEST_OBJ) | $(BIN)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $(POS_TEST_OBJ)

$(FEN_TEST_BIN): $(FEN_TEST_OBJ) | $(BIN)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $(FEN_TEST_OBJ)

$(BOUNDARY_TEST_BIN): $(BOUNDARY_TEST_OBJ) | $(BIN)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $(BOUNDARY_TEST_OBJ)

$(MAKEUNMAKE_TEST_BIN): $(MAKEUNMAKE_TEST_OBJ) | $(BIN)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $(MAKEUNMAKE_TEST_OBJ)

$(EP_TEST_BIN): $(EP_TEST_OBJ) | $(BIN)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $(EP_TEST_OBJ)

$(CASTLING_TEST_BIN): $(CASTLING_TEST_OBJ) | $(BIN)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $(CASTLING_TEST_OBJ)

$(PROMO_TEST_BIN): $(PROMO_TEST_OBJ) | $(BIN)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $(PROMO_TEST_OBJ)

$(NULL_TEST_BIN): $(NULL_TEST_OBJ) | $(BIN)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $(NULL_TEST_OBJ)

$(REPETITION_TEST_BIN): $(REPETITION_TEST_OBJ) | $(BIN)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $(REPETITION_TEST_OBJ)

$(MOVEGEN_TEST_BIN): $(MOVEGEN_TEST_OBJ) | $(BIN)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $(MOVEGEN_TEST_OBJ)

# `test` honors CONFIG (release by default): `make test CONFIG=debug` runs the debug
# binaries, `make test CONFIG=sanitize` the ASan/UBSan-instrumented ones.
# `test-sanitize` is the explicit alias for the instrumented run. CONFIG is validated
# up front (see VALID_CONFIGS), so an unknown CONFIG is a hard error, not a silent
# sanitizer build. The recursive make passes CONFIG explicitly so the child selects
# the matching build/cpp20/<config>/ objects and flags.
test:
	$(MAKE) CONFIG=$(CONFIG) test-run
test-sanitize:
	$(MAKE) CONFIG=sanitize test-run
test-run: $(TEST_BINS)
	@fail=0; for b in $(TEST_BINS); do echo "running $$(basename $$b) ($(CONFIG))"; ./$$b || fail=1; done; exit $$fail

# Exercises the non-<bit> portable fallback in core/bitboard.hpp by compiling and
# running BOTH the bitboard test (sq_bb_test) and the attacks test (attacks_test,
# which links the attack-table builder) with QWC_FORCE_PORTABLE_BITS=1 forced on.
# The <bit> and portable paths must agree on every primitive and on the derived
# attack tables; this is the reproducible check that the fallback (used on hosts
# without a <bit> acceleration) is semantically identical. Honors the current
# CONFIG for the optimisation flags.
test-portable-bits: | $(BIN)
	@echo "portable fallback [$(CXX) -DQWC_FORCE_PORTABLE_BITS=1, $(CONFIG)]: sq_bb_test + attacks_test"
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(WARN) -DQWC_FORCE_PORTABLE_BITS=1 -o $(BIN)/sq_bb_portable_test tests/sq_bb_test.cpp
	@$(BIN)/sq_bb_portable_test
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(WARN) -DQWC_FORCE_PORTABLE_BITS=1 -o $(BIN)/attacks_portable_test src/core/attacks.cpp tests/attacks_test.cpp
	@$(BIN)/attacks_portable_test
	@echo "portable-bits: OK ($(CONFIG)) (sq_bb + attacks agree with the <bit> path)"

# Legacy alias for check-cpp20. `make check-c` is kept only for the names in older
# docs/commands; it runs the strict C++20 conformance gate and nothing more. The
# CPP20-2B token blacklist (grepping for __builtin_/__GNUC__/asm) was removed: such
# scanning does not establish C++ correctness or portability and conflicts with the
# policy that permits justified, *guarded* platform code (e.g. measured intrinsics
# with a portable fallback and independent validation). check-cpp20 is authoritative.
check-c: check-cpp20

# Strict C++20 conformance gate: compile every source with
# -std=c++20 -pedantic-errors (syntax-only, no object output). $(CPPFLAGS)
# carries only include flags and no -std, and it precedes $(STRICT_CPP20), so the
# C++20 standard is set exactly once and no later option can override it.
# Independent of the per-CONFIG build flags.
check-cpp20:
	@echo "strict C++20 check [$(CXX) -std=c++20 -pedantic-errors] over $(words $(ALL_SRC)) files"
	@set -e; for f in $(ALL_SRC); do \
	  printf '  cpp20  -fsyntax-only  %s\n' "$$f"; \
	  $(CXX) $(CPPFLAGS) $(STRICT_CPP20) -fsyntax-only $$f; \
	done; \
	echo "strict C++20: OK ($(words $(ALL_SRC)) files, no non-C++20 constructs)"

# Network fetch + verification (offline-safe build never depends on this).
net:
	$(SHELL) tools/net_fetch.sh fetch
test-net:
	$(SHELL) tools/net_test.sh

clean:
	rm -rf build

help:
	@sed -n '2,18p' $(MAKEFILE_LIST)

-include $(DEPS)
