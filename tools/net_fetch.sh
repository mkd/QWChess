#!/bin/sh
# QwenChess NNUE network fetcher + verifier.
#
# Safe by design: downloads to a temporary file in the target directory and
# only moves it into place after the checksum AND the filename hash-prefix
# both verify. Any failure (network, checksum, prefix) exits non-zero and the
# expected digest is NEVER auto-corrected to force a pass.
#
#   net_fetch.sh fetch            download + verify + install
#   net_fetch.sh check <FILE>     verify an existing file in place (no download)
#
# Override any of NET_NAME / EXPECTED_SHA / NET_DIR via environment.
set -eu

NET_NAME="${NET_NAME:-nn-1a298aa575a0.nnue}"
# Full expected SHA-256 of the pinned Stockfish 19 default net.
EXPECTED_SHA="${EXPECTED_SHA:-1a298aa575a085434d29027978dc36867fe9c5bcea9376654b7a8eba1e52dfc2}"
NET_DIR="${NET_DIR:-networks}"

URLS="https://tests.stockfishchess.org/api/nn/${NET_NAME}
https://github.com/official-stockfish/networks/raw/master/${NET_NAME}"

sha() { sha256sum "$1" | awk '{print $1}'; }
short() { printf '%.12s' "$1"; }

# filename must be nn-<sha256 first 12 hex>.nnue
name_prefix() { n=$(basename "$1"); p="${n#nn-}"; printf '%s' "${p%%.nnue}"; }

verify_file() {
  file=$1
  [ -f "$file" ] || { echo "missing: $file" >&2; return 6; }
  got=$(sha "$file")
  if [ "$got" != "$EXPECTED_SHA" ]; then
    echo "checksum MISMATCH for $file" >&2
    echo "  expected: $EXPECTED_SHA" >&2
    echo "  got:      $got" >&2
    return 4
  fi
  np=$(name_prefix "$file"); gp=$(short "$got")
  if [ "$np" != "$gp" ]; then
    echo "filename hash-prefix mismatch: name=$np sha256=$gp" >&2
    return 5
  fi
  echo "verified: $file  sha256=$got" >&2
  return 0
}

cmd=${1:-fetch}
if [ "$cmd" = "check" ]; then
  verify_file "${2:?usage: net_fetch.sh check FILE}"
  exit $?
fi
if [ "$cmd" != "fetch" ]; then
  echo "usage: net_fetch.sh [fetch|check FILE]" >&2
  exit 2
fi

OUT="${NET_DIR}/${NET_NAME}"
mkdir -p "$NET_DIR"

# Already present and good? Skip the download.
if [ -f "$OUT" ]; then
  got=$(sha "$OUT")
  if [ "$got" = "$EXPECTED_SHA" ]; then
    echo "network already present and verified: $OUT" >&2
    exit 0
  fi
  echo "existing $OUT has wrong checksum; re-downloading" >&2
  rm -f "$OUT"
fi

# Download into a temp directory using the EXACT final filename, so the
# checksum + filename hash-prefix checks both apply before promotion.
tmpdir=$(mktemp -d "${NET_DIR}/.dl.XXXXXX")
tmp="${tmpdir}/${NET_NAME}"
trap 'rm -rf "$tmpdir"' EXIT

dl=1
for u in $URLS; do
  echo "  downloading $u" >&2
  if curl -fsSL --retry 1 --max-time 300 -o "$tmp" "$u" 2>/dev/null; then
    dl=0
    break
  fi
  if command -v wget >/dev/null 2>&1 && wget -q --tries=1 --timeout=300 -O "$tmp" "$u"; then
    dl=0
    break
  fi
  rm -f "$tmp"
done
[ "$dl" -eq 0 ] || { echo "ERROR: download failed from all origins" >&2; exit 3; }

# Verify the downloaded bytes before promoting them into place.
if ! verify_file "$tmp"; then
  echo "ERROR: downloaded file failed verification; not installed" >&2
  exit 4
fi
mv "$tmp" "$OUT"
rm -rf "$tmpdir"
trap - EXIT
echo "network installed: $OUT" >&2
exit 0
