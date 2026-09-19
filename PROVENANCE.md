# Provenance & Licensing

QwenChess is licensed **GPL-3.0-or-later** (see `COPYING`). This file records
where code and the evaluation network come from and what must travel with them
on redistribution.

## Upstream reference: Stockfish 19

QwenChess reuses proven algorithms from Stockfish and ports parts of its NNUE
inference to C. All such reuse is attributed to the pinned upstream revision.

| Field | Value |
|-------|-------|
| Upstream project | Stockfish (official-stockfish/Stockfish) |
| Reference release | `sf_19` (Stockfish 19) |
| **Pinned commit** | `edb0d9db6731067ec50ce619ff372b463bc4dd5d` |
| Verified | 2026-09-18 via GitHub API: tag `sf_19` -> commit `edb0d9db...` |
| Upstream license | GNU GPL v3 (or later) — upstream `Copying.txt` |
| Upstream language | C++ (QwenChess ports to C17; no C++ runtime at play time) |

The pinned commit is the source of truth for every "upstream reference fact" in
`docs/NNUE_COMPATIBILITY.md` (architecture dimensions, types, file version,
score ranges, eval-wrapper formula). **Do not** track a moving `master` branch.

### How to obtain the reference (isolated; not part of QwenChess sources)

The upstream C++ tree is only needed to build the *test oracle* (a later task)
and to consult source. It is **never** a runtime dependency and is **never**
committed into QwenChess. Keep it out of the normal source tree:

```sh
# From the QwenChess directory (creates a gitignored, isolated checkout):
git clone --filter=blob:none https://github.com/official-stockfish/Stockfish reference/stockfish-sf19
git -C reference/stockfish-sf19 checkout edb0d9db6731067ec50ce619ff372b463bc4dd5d
```

`reference/` is listed in `.gitignore`. Re-verify the checkout matches the pin:
`git -C reference/stockfish-sf19 rev-parse HEAD` must equal the pinned commit.

## Evaluation network

| Field | Value |
|-------|-------|
| Filename | `nn-1a298aa575a0.nnue` (= `nn-` + SHA256[0:12] + `.nnue`) |
| **Full SHA-256** | `1a298aa575a085434d29027978dc36867fe9c5bcea9376654b7a8eba1e52dfc2` |
| **Size (bytes)** | `98511183` (~94 MiB) |
| Primary origin | `https://tests.stockfishchess.org/api/nn/nn-1a298aa575a0.nnue` |
| Mirror origin | `https://github.com/official-stockfish/networks/raw/master/nn-1a298aa575a0.nnue` |
| Default name source | upstream `src/evaluate.h:36` (`EvalFileDefaultName`) |
| Trained by | The Stockfish team (official network) |

The filename encodes the first 12 hex digits of the file's SHA-256. `make net`
(`tools/net_fetch.sh`) downloads to a temporary file, verifies the full digest
**and** the filename prefix, and only then installs to `networks/` (gitignored).
A computed checksum alone does not prove provenance; the two independent
origins and the pinned-source filename are the provenance chain.

> **Pending provenance check:** confirm the license/redistribution terms that
> apply to the official Stockfish *network* artifact (consult the
> `official-stockfish/networks` repository) before redistributing the `.nnue`
> file outside this project. Code provenance is GPL-3.0-or-later; the network's
> own terms must be verified separately.

## Redistribution requirements

- Keep `COPYING` (GPL-3.0-or-later) and this `PROVENANCE.md`.
- Preserve upstream Stockfish copyright/notices on any derived or translated code.
- If distributing a binary that embeds the network, also convey the network's
  provenance and its (to-be-confirmed) license terms.
