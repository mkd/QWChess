#!/bin/sh
# Self-test for the NNUE network checker. Proves the verifier actually
# rejects bad input and accepts good input, without touching the real network.
# Exits non-zero if either control fails.
set -u

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
NET_NAME=nn-1a298aa575a0.nnue
NET_DIR=${NET_DIR:-networks}
rc=0

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# Negative control: correct filename, wrong content -> must be REJECTED.
printf 'deliberately-invalid-bytes-for-net-check' > "$tmp/$NET_NAME"
if "$SCRIPT_DIR/net_fetch.sh" check "$tmp/$NET_NAME" 2>/dev/null; then
  echo "FAIL: bogus net unexpectedly PASSED verification" >&2
  rc=1
else
  echo "PASS: bogus net correctly rejected (non-zero exit)" >&2
fi

# Negative control 2: wrong filename prefix -> must be REJECTED.
printf 'x' > "$tmp/nn-000000000000.nnue"
if "$SCRIPT_DIR/net_fetch.sh" check "$tmp/nn-000000000000.nnue" 2>/dev/null; then
  echo "FAIL: wrong-prefix net unexpectedly PASSED" >&2
  rc=1
else
  echo "PASS: wrong-prefix net correctly rejected" >&2
fi

# Positive control: the real net (if downloaded) must PASS; absent -> skip.
real="$NET_DIR/$NET_NAME"
if [ -f "$real" ]; then
  if "$SCRIPT_DIR/net_fetch.sh" check "$real" 2>/dev/null; then
    echo "PASS: real net verified" >&2
  else
    echo "FAIL: real net failed verification" >&2
    rc=1
  fi
else
  echo "SKIP: real net not downloaded (run: make net)" >&2
fi

echo "net_test: $([ "$rc" -eq 0 ] && echo PASS || echo FAIL)" >&2
exit "$rc"
