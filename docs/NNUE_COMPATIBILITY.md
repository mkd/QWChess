# NNUE Compatibility (Stockfish 19 / SFNNv16)

This is the compatibility specification QwenChess's C NNUE must match. It
separates **facts verified against the pinned source/artifact** from **details
still requiring source inspection**. A file that merely *loads* is **not**
proof of a compatible evaluation — parity is proven only by the differential
oracle (below) on the exact integer pipeline.

Verification legend:
- **✓ 2026-09-18** = re-fetched from the pinned source this session.
- **△ plan** = inspected during planning; re-verify before implementing.
- **○ pending** = requires source inspection before any code is written.

## 1. Pinned reference

| Field | Value | Status |
|-------|-------|--------|
| Repo | `official-stockfish/Stockfish` | ✓ |
| Tag | `sf_19` → commit `edb0d9db6731067ec50ce619ff372b463bc4dd5d` | ✓ (GitHub API, 2026-09-18) |
| Release notes | SFNNv16; revised threat features; new pawn-pair features; secondary small net retired; QAT; RVV/LoongArch; universal binaries; strict position validation | ✓ |
| Upstream license | GPL-3.0-or-later (`src/Copying.txt`) | △ plan |

Re-verify the pin: `git -C reference/stockfish-sf19 rev-parse HEAD` must equal the
commit. See `PROVENANCE.md` for the isolated-checkout procedure.

## 2. Network artifact

| Field | Value | Status |
|-------|-------|--------|
| Filename | `nn-1a298aa575a0.nnue` (`nn-`+SHA256[0:12]+`.nnue`) | ✓ |
| **Full SHA-256** | `1a298aa575a085434d29027978dc36867fe9c5bcea9376654b7a8eba1e52dfc2` | ✓ (downloaded + hashed) |
| **Size** | `98511183` bytes | ✓ |
| Primary origin | `https://tests.stockfishchess.org/api/nn/…` | ✓ (`scripts/net.sh:52`) |
| Mirror origin | `https://github.com/official-stockfish/networks/raw/master/…` | ✓ |
| Default name | `src/evaluate.h:36` `EvalFileDefaultName` | ✓ |

## 3. File format

- Header (little-endian): `[u32 Version][u32 hash][u32 descLen][desc bytes]`
  then the parameters, then EOF. **△ plan** (`src/nnue/network.cpp:300-360`)
- `Version = 0x6A448AFA` — **✓ 2026-09-18** (`nnue_common.h:65`).
- `hash` = `FeatureTransformer::get_hash_value() ^ NetworkArchitecture::get_hash_value()`. △ plan
- Weights may be **LEB128-compressed**: marker `Leb128MagicString = "COMPRESSED_LEB128"`. ✓ (constant) — **○ exact decode** pending (`nnue_misc`).

## 4. Types, scales, constants — `src/nnue/nnue_common.h` (✓ 2026-09-18)

```
BiasType=i16  ThreatWeightType=i8  WeightType=i16  PSQTWeightType=i32  IndexType=u32
OutputScale=16  WeightScaleBits=6  FtMaxVal=255  HiddenOneVal=128  CacheLineSize=64
```

## 5. Network architecture — `src/nnue/nnue_architecture.h` (✓ 2026-09-18)

Feature sets: `ThreatFeatureSet=FullThreats`, `PairFeatureSet=PP_3Wide`,
`PSQFeatureSet=HalfKAv2_hm`.
`L1=1024  L2=32  L3=32  PSQTBuckets=8  LayerStacks=8`.

```
fc_0 : AffineTransformSparseInput<L1=1024, L2=32>   ac_sqr_0 : SqrClippedReLU<32, WS+1>
ac_0 : ClippedReLU<32, WS+1>                        fc_1 : AffineTransform<64, L3=32>
ac_sqr_1 : SqrClippedReLU<32, WS>                   ac_1 : ClippedReLU<32, WS>
fc_2 : AffineTransform<128, 1>
```
(`WS = WeightScaleBits = 6`. `read_parameters` reads fc_0,ac_0,fc_1,ac_1,fc_2 — the
`ac_sqr_*` are parameterless. Forward: fc_0 →(sqr+relu concat)→ fc_1 →(sqr+relu
concat)→ fc_2 → scalar.) **△ plan** for exact read order / ac_sqr placement.

## 6. Feature sets (✓ 2026-09-18 for dims/hashes; **○ for index bit-math**)

| Feature | `Dimensions` | Hash | Notes |
|---------|-------------|------|-------|
| `HalfKAv2_hm` (PSQ) | `SQUARE_NB·PS_NB/2 = 64·704/2 = 22528` | `0x7f234cb8` | `KingBuckets[64]` (8 buckets), `OrientTBL[64]` (180° for black), `MaxActive=32`, `IndexList=ValueList<u16,32>` |
| `FullThreats` | `59808` | `0x2e6b9d04` | `OrientTBL[64]` (i8); ray/target encoding **○ pending** |
| `PP_3Wide` (pawn pairs) | `PawnIds·(PawnIds-1)/2 = 96·95/2 = 4560` | `0x86f2b1dd` | `PawnIds=COLOR_NB·48=96`; `IndexBase=59808` (concatenated after threats) |

The feature transformer maps the combined input (`22528 + 59808 + 4560 = 86896`)
to `L1=1024`; PSQ weights are `i16`, threat+pair weights are `i8`. PSQT outputs
are `i32` per `PSQTBuckets`. **○ pending**: exact weight read order, the
`PackusEpi16Order` permutation, and the per-set index arithmetic.

## 7. Evaluation wrapper (search-dependent) — `src/evaluate.cpp` (✓ 2026-09-18)

```c
[psqt, positional] = network.evaluate(pos, accumulators, caches);   // raw NNUE
nnue = psqt + positional;
nnueComplexity = abs(psqt - positional);
optimism += optimism * i64(nnueComplexity) / 476;                  // search-fed optimism
nnue  -= nnue * i64(nnueComplexity) / 18236;
material = 534 * pos.count<PAWN>() + pos.non_pawn_material();
v = nnue + (nnue * i64(material) + optimism * i64(7675)) / 91000;
v -= v * pos.rule50_count() / 199;                                  // shuffle damping
v = clamp(v, VALUE_TB_LOSS_IN_MAX_PLY+1, VALUE_TB_WIN_IN_MAX_PLY-1);// avoid TB range
```
UCI cp = `round(100·v / a)` where `a` is from a win-rate model (`uci.cpp to_cp`).
The eval is side-to-move; negate for White. This wrapper (optimism/material/rule50
blend + clamp) is **search-dependent** and distinct from the raw NNUE output.

## 8. Score / depth (upstream reference) — `src/types.h` (✓ 2026-09-18)

`MAX_PLY=246` (SF's; **QwenChess uses its own 256**), `Value=int`,
`VALUE_MATE=32000`, `MATE_IN_MAX_PLY=31754`, `TB=31753`, `INFINITE=32001`,
`NONE=32002`; material P208 N781 B825 R1276 Q2538. QwenChess derives its own
ranges from 256 (see `docs/ARCHITECTURE.md`) — the *search-depth* limit is not an
NNUE-compatibility requirement.

## 9. Accumulator / Finny — `src/nnue/nnue_accumulator.h` (△ plan)

`Accumulator` = `i16[2][L1]` + `i32[2][PSQTBuckets]` + `computed[2]`.
`AccumulatorCaches` (Finny) = `entries[64][2]` of `{i16[1024], i32[8], piece
snapshot, bitboard}` — a per-king-square, per-perspective refresh cache.
`AccumulatorStack` holds per-ply `AccumulatorState {Accumulator, Dirties}` up to
`MAX_PLY+1`. **○ pending**: the exact forward/backward dirty-update rules and
`find_last_usable_accumulator`. **Per the corrected architecture, this cache is
mutable and per-worker** (rebuild on network change).

## 10. Still unresolved (○) — must be resolved in the NNUE tasks, with source citations

1. Exact **LEB128** weight decode (`nnue_misc`).
2. Feature-transformer **weight read order** (PSQ i16 vs threat/pair i8 interleaving) + `PackusEpi16Order`.
3. **FullThreats** ray/target index bit-math (`map[]`, `OrientTBL`, `numValidTargets`).
4. **PP_3Wide** pair ordering within the 96 pawn IDs.
5. **HalfKAv2_hm** index application (king-bucket + orientation) for incremental diffs.
6. **Accumulator** dirty forward/backward update + last-usable search.
7. Network **artifact license/redistribution** terms (see `PROVENANCE.md` pending item).

## 11. Oracle strategy (exact differential)

A separately **instrumented** upstream SF19 C++ build (patched at the pinned
commit, kept reproducible, **never** a runtime backend) exports, for a FEN + move
sequence: per-side feature index lists, `i16[1024]` accumulators, `i32[8]` PSQT,
`fc0/fc1/fc2` raw outputs, `{psqt, positional}`, wrapped `Value`. QwenChess's
scalar path dumps the same; we **byte-compare integers** (identical arithmetic ⇒
identical outputs). Then verify incremental == full-refresh inside QwenChess over
seeded make/unmake sequences. Rounded UCI cp is insufficient for this check.
