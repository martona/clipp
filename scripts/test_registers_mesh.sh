#!/usr/bin/env bash
#
# Two-daemon acceptance for the mesh semantics `test_registers.sh` deliberately
# can't reach: register replication between daemons, tombstone propagation, and
# `put` making a register the live clipboard EVERYWHERE. Pin two different
# daemons with --host and assert each sees the other's writes.
#
# Usage:   ./test_registers_mesh.sh <host-A> <host-B> [path-to-clipp]
# Example: ./test_registers_mesh.sh devbox2 mac-mini
#
# Both daemons must run THIS build and be members of the same group, connected
# to each other. Settle sleeps cover broadcast latency; set SETTLE for a slow
# LAN. (On Linux there is no local daemon; both hosts must be macOS/Windows.)
#
# NOT covered here (GUI-only triggers — verify by hand on the A3+ build):
#   * MRU re-share: click a mid-list item in host-A's Clipp page -> it moves to
#     the top there AND on host-B, and host-B's OS clipboard now holds it.
#   * Mesh delete: right-click -> Delete on host-A -> the row disappears on
#     host-B too (best-effort: only currently-connected peers).
#   * Zero-anchor resync: restart host-A's daemon mid-churn -> its activity
#     list repopulates (up to clipboardSyncMaxItems) including any relocations.
#   * Binary registers: (after the popup's promote ships) promote an image ->
#     `ls -v` shows "[image/png, ...]" from both hosts; `put` of it pastes the
#     image on the other host; old CLI `paste` of it refuses a tty.

set -u

HOSTA="${1:-}"
HOSTB="${2:-}"
if [ -z "$HOSTA" ] || [ -z "$HOSTB" ]; then
    echo "usage: $0 <host-A> <host-B> [path-to-clipp]" >&2
    exit 2
fi
CLIPP="${3:-${CLIPP:-}}"
if [ -z "$CLIPP" ]; then
    if command -v clipp >/dev/null 2>&1; then
        CLIPP="clipp"
    else
        echo "clipp not found on PATH; pass its path as the 3rd argument or set \$CLIPP." >&2
        exit 2
    fi
fi
SETTLE="${SETTLE:-2}"

echo "clipp : $CLIPP"
echo "A     : $HOSTA"
echo "B     : $HOSTB"
echo

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
pass=0
fail=0
ok() { pass=$((pass + 1)); printf '  PASS  %s\n' "$1"; }
no() { fail=$((fail + 1)); printf '  FAIL  %s\n' "$1"; }

runA() { "$CLIPP" --host "$HOSTA" "$@"; }
runB() { "$CLIPP" --host "$HOSTB" "$@"; }

# --- write via A, read via B: the REGW broadcast replicated the record ---
payload="mesh-rt-$$-${RANDOM}${RANDOM}"
printf '%s' "$payload" >"$TMP/rt.in"
runA copy test.mesh.rt <"$TMP/rt.in" >/dev/null 2>&1
sleep "$SETTLE"
runB paste test.mesh.rt >"$TMP/rt.out" 2>/dev/null; rc=$?
if [ "$rc" -eq 0 ] && [ "$(cat "$TMP/rt.out")" = "$payload" ]; then ok "A-write readable via B (replicated)"
else no "A-write not readable via B (exit $rc)"; fi

# --- wide name replicates too ---
printf 'wide mesh payload' >"$TMP/wide.in"
runA copy 'test wide mesh!' <"$TMP/wide.in" >/dev/null 2>&1
sleep "$SETTLE"
runB paste 'test wide mesh!' >"$TMP/wide.out" 2>/dev/null; rc=$?
if [ "$rc" -eq 0 ] && [ "$(cat "$TMP/wide.out")" = "wide mesh payload" ]; then ok "wide-named register replicated"
else no "wide-named register not replicated (exit $rc)"; fi

# --- put via A: the register becomes the live clipboard on BOTH daemons ---
putpayload="mesh-put-$$-${RANDOM}${RANDOM}"
printf '%s' "$putpayload" >"$TMP/put.in"
runA copy test.mesh.put <"$TMP/put.in" >/dev/null 2>&1
sleep "$SETTLE"
runA put test.mesh.put >/dev/null 2>&1
sleep "$SETTLE"
onA=no; runA paste 2>/dev/null | grep -qF "$putpayload" && onA=yes
onB=no; runB paste 2>/dev/null | grep -qF "$putpayload" && onB=yes
if [ "$onA" = yes ] && [ "$onB" = yes ]; then ok "put made the register current on both daemons"
else no "put propagation (A=$onA B=$onB)"; fi

# --- rm via B: the tombstone reaches A ---
runB rm test.mesh.rt >/dev/null 2>&1; rmrc=$?
sleep "$SETTLE"
gone=yes; runA ls test.mesh.rt 2>/dev/null | grep -q 'test\.mesh\.rt' && gone=no
if [ "$rmrc" -eq 0 ] && [ "$gone" = yes ]; then ok "B-side rm tombstoned the register on A"
else no "B-side rm did not reach A (rmExit=$rmrc gone=$gone)"; fi

# --- cleanup on both sides (idempotent; tombstones replicate anyway) ---
runA rm 'test*' >/dev/null 2>&1
runB rm 'test*' >/dev/null 2>&1

echo
echo "$pass passed, $fail failed."
[ "$fail" -eq 0 ]
