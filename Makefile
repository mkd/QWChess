# QwenChess build -- single Makefile, C++20.
#
#   make            build the release engine      (build/cpp20/release/)
#   make debug      build the debug engine        (build/cpp20/debug/)
#   make sanitize   build the ASan+UBSan engine   (build/cpp20/sanitize/)
#   make test       build + run the tests (honors CONFIG; release by default)
#   make test CONFIG=debug|sanitize|release   build + run the tests in that config
#   make test-sanitize  alias for the ASan/UBSan run (real instrumentation)
#   make test-perft   full-depth perft gate (all supplied depths; builds qwcperft)
#   make test-differential  independent movegen differential (pinned python chess; PYTHON=)
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
PYTHON   ?= python3
NET_DIR  ?= networks
CPPFLAGS := -Isrc
LDFLAGS  :=
# Sanitizer-only linker flags, consumed ONLY by the sanitize configuration (see
# SANIT_LDFLAGS below). Default is STATIC ASan (-static-libasan): this environment has
# a preloaded hook (a setuid /etc/ld.so.preload entry) that breaks the DYNAMIC ASan
# loader order -- the dynamic runtime aborts at startup with "ASan runtime does not
# come first in initial library list". Statically linking the ASan runtime sidesteps
# the dynamic loader entirely and works both here and on hosts without the hook.
# Override, e.g. `make test-sanitize SANITIZER_LDFLAGS=` for ordinary dynamic
# linkage on a clean host. Only the sanitize config consumes it; release/debug link
# without it. An unrecognized value is deliberately NOT filtered -- it is handed to
# the linker and fails visibly there.
SANITIZER_LDFLAGS ?= -static-libasan

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

# The sanitizer-only linker flags, applied at link time ONLY when the sanitize config
# is selected (see SANITIZER_LDFLAGS above). release/debug link without them. By default
# empty (ordinary dynamic ASan linkage); an explicit value (e.g. -static-libasan) is
# honored only for sanitize.
ifeq ($(CONFIG),sanitize)
  SANIT_LDFLAGS := $(SANITIZER_LDFLAGS)
else
  SANIT_LDFLAGS :=
endif

SRC := src
OBJ := build/cpp20/$(CONFIG)/obj
BIN := build/cpp20/$(CONFIG)/bin

# Every source file (core + platform + engine + movegen + diagnostics + tests + the
# test-only differential probe) for the strict-C++20 check.
ALL_SRC := $(wildcard $(SRC)/*.cpp $(SRC)/*/*.cpp tests/*.cpp tools/position_probe.cpp)

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
MOVEGEN_LEGAL_TEST_OBJ := $(OBJ)/movegen/movegen.o $(OBJ)/tests/movegen_legal_test.o $(OBJ)/position/state.o $(OBJ)/position/fen.o $(OBJ)/position/position.o $(OBJ)/core/zobrist.o $(OBJ)/core/attacks.o
MOVEGEN_LEGAL_TEST_BIN := $(BIN)/movegen_legal_test
MOVEGEN_CASTLE_TEST_OBJ := $(OBJ)/movegen/movegen.o $(OBJ)/tests/movegen_castling_test.o $(OBJ)/position/state.o $(OBJ)/position/fen.o $(OBJ)/position/position.o $(OBJ)/core/zobrist.o $(OBJ)/core/attacks.o
MOVEGEN_CASTLE_TEST_BIN := $(BIN)/movegen_castling_test

# T006 search-plumbing tests (this checkpoint): the temporary material evaluation and
# the UCI move codec. The codec resolves against movegen_legal, so it links movegen +
# the full position stack (same dep set as the movegen tests).
MATERIAL_TEST_OBJ := $(OBJ)/tests/material_test.o $(OBJ)/search/material.o $(OBJ)/position/state.o $(OBJ)/position/fen.o $(OBJ)/position/position.o $(OBJ)/core/zobrist.o $(OBJ)/core/attacks.o
MATERIAL_TEST_BIN := $(BIN)/material_test
CODEC_TEST_OBJ := $(OBJ)/tests/uci_codec_test.o $(OBJ)/engine/uci_codec.o $(OBJ)/movegen/movegen.o $(OBJ)/position/state.o $(OBJ)/position/fen.o $(OBJ)/position/position.o $(OBJ)/core/zobrist.o $(OBJ)/core/attacks.o
CODEC_TEST_BIN := $(BIN)/uci_codec_test

# The T006 synchronous search (this checkpoint): iterative deepening over the legal
# generator with the temporary Material evaluation. Links the clock (deadline checks)
# in addition to the movegen + position stack.
SEARCH_TEST_OBJ := $(OBJ)/tests/search_test.o $(OBJ)/search/search.o $(OBJ)/search/material.o $(OBJ)/movegen/movegen.o $(OBJ)/position/state.o $(OBJ)/position/fen.o $(OBJ)/position/position.o $(OBJ)/engine/uci_codec.o $(OBJ)/core/zobrist.o $(OBJ)/core/attacks.o $(OBJ)/platform/clock.o
SEARCH_TEST_BIN := $(BIN)/search_test

# Perft diagnostics (T005 cp4): the shared perft module, the qwcperft developer
# tool (perft + root divide), and the perft regression test. The perft test is part
# of the ordinary suite (bounded depths); `test-perft` runs its full-depth gate.
PERFT_OBJ      := $(OBJ)/diagnostics/perft.o
QWCPERFT_OBJ   := $(PERFT_OBJ) $(OBJ)/diagnostics/perft_main.o $(OBJ)/movegen/movegen.o $(OBJ)/position/state.o $(OBJ)/position/fen.o $(OBJ)/position/position.o $(OBJ)/core/zobrist.o $(OBJ)/core/attacks.o
QWCPERFT_BIN   := $(BIN)/qwcperft
PERFT_TEST_OBJ := $(PERFT_OBJ) $(OBJ)/tests/perft_test.o $(OBJ)/movegen/movegen.o $(OBJ)/position/state.o $(OBJ)/position/fen.o $(OBJ)/position/position.o $(OBJ)/core/zobrist.o $(OBJ)/core/attacks.o
PERFT_TEST_BIN := $(BIN)/perft_test
# The independent move-generation differential oracle (test-only): links QWChess and
# is driven by tools/movegen_diff.py. A tools/ pattern rule below compiles it per config.
PROBE_OBJ      := $(OBJ)/tools/position_probe.o $(OBJ)/movegen/movegen.o $(OBJ)/position/state.o $(OBJ)/position/fen.o $(OBJ)/position/position.o $(OBJ)/core/zobrist.o $(OBJ)/core/attacks.o
PROBE_BIN      := $(BIN)/position_probe

TEST_BINS := $(SCAFFOLD_TEST_BIN) $(SQBB_TEST_BIN) $(MOVE_TEST_BIN) $(ATTACKS_TEST_BIN) $(ZOB_TEST_BIN) $(POS_TEST_BIN) $(FEN_TEST_BIN) $(BOUNDARY_TEST_BIN) $(MAKEUNMAKE_TEST_BIN) $(EP_TEST_BIN) $(CASTLING_TEST_BIN) $(PROMO_TEST_BIN) $(NULL_TEST_BIN) $(REPETITION_TEST_BIN) $(MOVEGEN_TEST_BIN) $(MOVEGEN_LEGAL_TEST_BIN) $(MOVEGEN_CASTLE_TEST_BIN) $(MATERIAL_TEST_BIN) $(CODEC_TEST_BIN) $(SEARCH_TEST_BIN) $(PERFT_TEST_BIN)

DEPS := $(ENGINE_OBJ:.o=.d) $(SCAFFOLD_TEST_OBJ:.o=.d) $(SQBB_TEST_OBJ:.o=.d) $(MOVE_TEST_OBJ:.o=.d) $(ATTACKS_TEST_OBJ:.o=.d) $(ZOB_TEST_OBJ:.o=.d) $(POS_TEST_OBJ:.o=.d) $(FEN_TEST_OBJ:.o=.d) $(BOUNDARY_TEST_OBJ:.o=.d) $(MAKEUNMAKE_TEST_OBJ:.o=.d) $(EP_TEST_OBJ:.o=.d) $(CASTLING_TEST_OBJ:.o=.d) $(PROMO_TEST_OBJ:.o=.d) $(NULL_TEST_OBJ:.o=.d) $(REPETITION_TEST_OBJ:.o=.d) $(MOVEGEN_TEST_OBJ:.o=.d) $(MOVEGEN_LEGAL_TEST_OBJ:.o=.d) $(MOVEGEN_CASTLE_TEST_OBJ:.o=.d) $(MATERIAL_TEST_OBJ:.o=.d) $(CODEC_TEST_OBJ:.o=.d) $(SEARCH_TEST_OBJ:.o=.d) $(PERFT_TEST_OBJ:.o=.d) $(QWCPERFT_OBJ:.o=.d) $(PROBE_OBJ:.o=.d)

.SUFFIXES:
.PHONY: all release debug sanitize test test-sanitize test-run test-perft test-perft-run test-differential test-differential-run test-portable-bits all-config check-c check-cpp20 net test-net clean help

all: release

release:
	$(MAKE) CONFIG=release all-config
debug:
	$(MAKE) CONFIG=debug all-config
sanitize:
	$(MAKE) CONFIG=sanitize all-config

# Per-configuration build of the engine binary.
all-config: $(ENGINE_BIN)

$(ENGINE_BIN): $(ENGINE_OBJ) Makefile | $(BIN)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(SANIT_LDFLAGS) -o $@ $(ENGINE_OBJ)
	@echo "built $@ ($(CONFIG))"

# Pattern rule for sources under src/ (maps to a parallel obj/ tree).
$(OBJ)/%.o: src/%.cpp | $(OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(WARN) -MMD -MP -c $< -o $@

# Test objects live in tests/ (not src/); one pattern rule covers all of them.
$(OBJ)/tests/%.o: tests/%.cpp | $(OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(WARN) -MMD -MP -c $< -o $@

# The test-only differential probe lives in tools/; one pattern rule compiles it per
# config (mirrors the tests/ rule). Not a shipped engine source.
$(OBJ)/tools/%.o: tools/%.cpp | $(OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(WARN) -MMD -MP -c $< -o $@

$(OBJ) $(BIN):
	mkdir -p $@

# Per-configuration test binaries: built + linked with the current CONFIG's flags
# (CXXFLAGS above) into build/cpp20/<config>/bin/. `test` / `test-sanitize` build
# and run them (see the `test` target below for how CONFIG is honored).
$(SCAFFOLD_TEST_BIN): $(SCAFFOLD_TEST_OBJ) Makefile | $(BIN)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(SANIT_LDFLAGS) -o $@ $(SCAFFOLD_TEST_OBJ)

$(SQBB_TEST_BIN): $(SQBB_TEST_OBJ) Makefile | $(BIN)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(SANIT_LDFLAGS) -o $@ $(SQBB_TEST_OBJ)

$(MOVE_TEST_BIN): $(MOVE_TEST_OBJ) Makefile | $(BIN)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(SANIT_LDFLAGS) -o $@ $(MOVE_TEST_OBJ)

$(ATTACKS_TEST_BIN): $(ATTACKS_TEST_OBJ) Makefile | $(BIN)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(SANIT_LDFLAGS) -o $@ $(ATTACKS_TEST_OBJ)

$(ZOB_TEST_BIN): $(ZOB_TEST_OBJ) Makefile | $(BIN)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(SANIT_LDFLAGS) -o $@ $(ZOB_TEST_OBJ)

$(POS_TEST_BIN): $(POS_TEST_OBJ) Makefile | $(BIN)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(SANIT_LDFLAGS) -o $@ $(POS_TEST_OBJ)

$(FEN_TEST_BIN): $(FEN_TEST_OBJ) Makefile | $(BIN)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(SANIT_LDFLAGS) -o $@ $(FEN_TEST_OBJ)

$(BOUNDARY_TEST_BIN): $(BOUNDARY_TEST_OBJ) Makefile | $(BIN)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(SANIT_LDFLAGS) -o $@ $(BOUNDARY_TEST_OBJ)

$(MAKEUNMAKE_TEST_BIN): $(MAKEUNMAKE_TEST_OBJ) Makefile | $(BIN)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(SANIT_LDFLAGS) -o $@ $(MAKEUNMAKE_TEST_OBJ)

$(EP_TEST_BIN): $(EP_TEST_OBJ) Makefile | $(BIN)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(SANIT_LDFLAGS) -o $@ $(EP_TEST_OBJ)

$(CASTLING_TEST_BIN): $(CASTLING_TEST_OBJ) Makefile | $(BIN)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(SANIT_LDFLAGS) -o $@ $(CASTLING_TEST_OBJ)

$(PROMO_TEST_BIN): $(PROMO_TEST_OBJ) Makefile | $(BIN)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(SANIT_LDFLAGS) -o $@ $(PROMO_TEST_OBJ)

$(NULL_TEST_BIN): $(NULL_TEST_OBJ) Makefile | $(BIN)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(SANIT_LDFLAGS) -o $@ $(NULL_TEST_OBJ)

$(REPETITION_TEST_BIN): $(REPETITION_TEST_OBJ) Makefile | $(BIN)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(SANIT_LDFLAGS) -o $@ $(REPETITION_TEST_OBJ)

$(MOVEGEN_TEST_BIN): $(MOVEGEN_TEST_OBJ) Makefile | $(BIN)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(SANIT_LDFLAGS) -o $@ $(MOVEGEN_TEST_OBJ)

$(MOVEGEN_LEGAL_TEST_BIN): $(MOVEGEN_LEGAL_TEST_OBJ) Makefile | $(BIN)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(SANIT_LDFLAGS) -o $@ $(MOVEGEN_LEGAL_TEST_OBJ)

$(MOVEGEN_CASTLE_TEST_BIN): $(MOVEGEN_CASTLE_TEST_OBJ) Makefile | $(BIN)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(SANIT_LDFLAGS) -o $@ $(MOVEGEN_CASTLE_TEST_OBJ)

$(MATERIAL_TEST_BIN): $(MATERIAL_TEST_OBJ) Makefile | $(BIN)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(SANIT_LDFLAGS) -o $@ $(MATERIAL_TEST_OBJ)

$(CODEC_TEST_BIN): $(CODEC_TEST_OBJ) Makefile | $(BIN)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(SANIT_LDFLAGS) -o $@ $(CODEC_TEST_OBJ)

$(SEARCH_TEST_BIN): $(SEARCH_TEST_OBJ) Makefile | $(BIN)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(SANIT_LDFLAGS) -o $@ $(SEARCH_TEST_OBJ)

$(QWCPERFT_BIN): $(QWCPERFT_OBJ) Makefile | $(BIN)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(SANIT_LDFLAGS) -o $@ $(QWCPERFT_OBJ)

$(PERFT_TEST_BIN): $(PERFT_TEST_OBJ) Makefile | $(BIN)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(SANIT_LDFLAGS) -o $@ $(PERFT_TEST_OBJ)

$(PROBE_BIN): $(PROBE_OBJ) Makefile | $(BIN)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(SANIT_LDFLAGS) -o $@ $(PROBE_OBJ)

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

# Full-depth perft gate: every supplied depth (incl. the deepest), with the
# qwcperft developer tool built alongside for manual perft/divide runs.
test-perft:
	$(MAKE) CONFIG=$(CONFIG) test-perft-run
test-perft-run: $(PERFT_TEST_BIN) $(QWCPERFT_BIN)
	@echo "perft full-depth gate [$(CONFIG)]"
	@fail=0; ./$(PERFT_TEST_BIN) --full || fail=1; exit $$fail

# Independent move-generation differential: build the QWChess-linked oracle probe,
# then drive it with tools/movegen_diff.py (pinned python `chess` 1.11.2). The Python
# interpreter is selectable: `make test-differential CONFIG=release PYTHON=/path/python`.
test-differential:
	$(MAKE) CONFIG=$(CONFIG) PYTHON=$(PYTHON) test-differential-run
test-differential-run: $(PROBE_BIN)
	@echo "move-generation differential [$(CONFIG)] via $(PYTHON)"
	@$(PYTHON) tools/movegen_diff.py --probe "$(PROBE_BIN)"

# Exercises the non-<bit> portable fallback in core/bitboard.hpp by compiling and
# running BOTH the bitboard test (sq_bb_test) and the attacks test (attacks_test,
# which links the attack-table builder) with QWC_FORCE_PORTABLE_BITS=1 forced on.
# The <bit> and portable paths must agree on every primitive and on the derived
# attack tables; this is the reproducible check that the fallback (used on hosts
# without a <bit> acceleration) is semantically identical. Honors the current
# CONFIG for the optimisation flags.
test-portable-bits: | $(BIN)
	@echo "portable fallback [$(CXX) -DQWC_FORCE_PORTABLE_BITS=1, $(CONFIG)]: sq_bb_test + attacks_test"
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(WARN) $(SANIT_LDFLAGS) -DQWC_FORCE_PORTABLE_BITS=1 -o $(BIN)/sq_bb_portable_test tests/sq_bb_test.cpp
	@$(BIN)/sq_bb_portable_test
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(WARN) $(SANIT_LDFLAGS) -DQWC_FORCE_PORTABLE_BITS=1 -o $(BIN)/attacks_portable_test src/core/attacks.cpp tests/attacks_test.cpp
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
