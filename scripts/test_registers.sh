#!/usr/bin/env bash
#
# Acceptance test for clipp named registers. Exercises the CLI <-> daemon round
# trip that the C++ unit tests can't reach: argument parsing, the OneShot socket,
# stdio/pipe handling, exit codes, and the rich RLST list path.
#
# This is NOT a CRDT test -- it pins ONE daemon with --host, so copy and the
# following paste/ls hit the same store deterministically. Convergence across two
# daemons needs a different harness.
#
# Usage:   ./test_registers.sh <device-name-or-ip> [path-to-clipp]
# Example: ./test_registers.sh mac-mini
#
# Notes:
#   * The target daemon must run THIS build (the RLST list frame gained a field;
#     ls / ls -v / rm-glob won't decode against an older daemon). copy/paste of a
#     single name work cross-version.
#   * On Linux there is no local register daemon, so --host must name a macOS or
#     Windows daemon on the LAN.
#   * Register names are all "test.*"; everything is rm'd at the end, leaving only
#     tombstones (they persist ~90 days). Point this at a throwaway group if you
#     don't want the traffic on your real mesh.

set -u

HOST="${1:-}"
if [ -z "$HOST" ]; then
    echo "usage: $0 <device-name-or-ip> [path-to-clipp]" >&2
    exit 2
fi
CLIPP="${2:-${CLIPP:-}}"
if [ -z "$CLIPP" ]; then
    if command -v clipp >/dev/null 2>&1; then
        CLIPP="clipp"
    else
        echo "clipp not found on PATH; pass its path as the 2nd argument or set \$CLIPP." >&2
        exit 2
    fi
fi

echo "clipp : $CLIPP"
echo "host  : $HOST"
echo

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
pass=0
fail=0
ok() { pass=$((pass + 1)); printf '  PASS  %s\n' "$1"; }
no() { fail=$((fail + 1)); printf '  FAIL  %s\n' "$1"; }

# Every call pins the target daemon. Native redirection (< >) is byte-exact.
run() { "$CLIPP" --host "$HOST" "$@"; }

# copy <payload-file> into <name>, paste it back, assert byte-exact.
roundtrip() { # name infile
    local name="$1" infile="$2" out="$TMP/$1.out" rc
    run copy "$name" <"$infile" >/dev/null 2>&1; rc=$?
    if [ "$rc" -ne 0 ]; then no "$name copy (exit $rc)"; return; fi
    run paste "$name" >"$out" 2>/dev/null; rc=$?
    if [ "$rc" -ne 0 ]; then no "$name paste (exit $rc)"; return; fi
    if cmp -s "$infile" "$out"; then ok "$name round-trips ($(wc -c <"$infile" | tr -d ' ') bytes)"
    else no "$name round-trip mismatch"; fi
}

# --- byte-exact round trips through the daemon ---
printf 'hello world'           >"$TMP/basic.in"; roundtrip test.basic     "$TMP/basic.in"
printf 'line1\nline2\nline3\n' >"$TMP/multi.in"; roundtrip test.multiline "$TMP/multi.in"
# 'cafe' + COMBINING ACUTE (U+0301) + ' ' + party-popper. Decomposed on purpose:
# proves registers are byte-exact and do NOT NFC-normalise.
printf 'cafe\xcc\x81 \xf0\x9f\x8e\x89' >"$TMP/uni.in"; roundtrip test.unicode "$TMP/uni.in"

# --- overwrite is last-writer-wins ---
printf 'first'  >"$TMP/ow1"
printf 'second' >"$TMP/ow2"
run copy test.overwrite <"$TMP/ow1" >/dev/null 2>&1
run copy test.overwrite <"$TMP/ow2" >/dev/null 2>&1
run paste test.overwrite >"$TMP/ow.out" 2>/dev/null
if [ "$(cat "$TMP/ow.out")" = "second" ]; then ok "test.overwrite keeps the newer value"
else no "test.overwrite did not keep newer value"; fi

# --- private: masked in ls -v, still readable to a non-tty (a redirected pipe) ---
secret="sup3rsecret-$$-${RANDOM}${RANDOM}"
printf '%s' "$secret" >"$TMP/priv.in"
run copy test.priv --private <"$TMP/priv.in" >/dev/null 2>&1
lsv="$(run ls -v test.priv 2>/dev/null)"
if printf '%s' "$lsv" | grep -q '\[private\]' && ! printf '%s' "$lsv" | grep -qF "$secret"; then
    ok "test.priv masked in ls -v"
else
    no "test.priv NOT masked in ls -v"
fi
run paste test.priv >"$TMP/priv.out" 2>/dev/null; prc=$?
if [ "$prc" -eq 0 ] && [ "$(cat "$TMP/priv.out")" = "$secret" ]; then ok "test.priv readable to a non-tty"
else no "test.priv not readable to a non-tty (exit $prc)"; fi

# --- rm lifecycle: created -> listed -> removed -> paste fails ---
printf 'removable' >"$TMP/rmc.in"
run copy test.rmcheck <"$TMP/rmc.in" >/dev/null 2>&1
present=no; run ls test.rmcheck 2>/dev/null | grep -q 'test\.rmcheck' && present=yes
run rm test.rmcheck >/dev/null 2>&1; rmrc=$?
gone=yes;    run ls test.rmcheck 2>/dev/null | grep -q 'test\.rmcheck' && gone=no
run paste test.rmcheck >/dev/null 2>&1; prc=$?
if [ "$present" = yes ] && [ "$rmrc" -eq 0 ] && [ "$gone" = yes ] && [ "$prc" -ne 0 ]; then
    ok "test.rmcheck created/listed/removed, paste then fails"
else
    no "test.rmcheck lifecycle (present=$present rmExit=$rmrc gone=$gone pasteExit=$prc)"
fi

# --- rm of an absent register errors ---
run rm test.never-existed-zzz >/dev/null 2>&1
if [ $? -ne 0 ]; then ok "rm of absent register errors"; else no "rm of absent register did NOT error"; fi

# --- wide (GUI-era) names: spaces, caps, punctuation all valid now ---
printf 'wide-name payload' >"$TMP/wide.in"; roundtrip 'test wide (Name)!' "$TMP/wide.in"

# --- NFC at name ingress: a decomposed name arg addresses the same register as
# --- its precomposed spelling (register VALUES stay byte-exact; only NAMES fold)
printf 'nfc payload' >"$TMP/nfc.in"
run copy "$(printf 'test.cafe\xcc\x81')" <"$TMP/nfc.in" >/dev/null 2>&1   # 'e' + COMBINING ACUTE
run paste "$(printf 'test.caf\xc3\xa9')" >"$TMP/nfc.out" 2>/dev/null      # precomposed 'é'
if cmp -s "$TMP/nfc.in" "$TMP/nfc.out"; then ok "NFC name equivalence (decomposed == precomposed)"
else no "NFC name equivalence failed"; fi

# --- reserved printables rejected (globs stay CLI metacharacters; / is namespaces) ---
printf 'x' >"$TMP/bad.in"
run copy 'test/bad' <"$TMP/bad.in" >/dev/null 2>&1
if [ $? -ne 0 ]; then ok "reserved '/' in name rejected"; else no "reserved '/' in name accepted"; fi
run copy 'test.bad*' <"$TMP/bad.in" >/dev/null 2>&1
if [ $? -ne 0 ]; then ok "reserved '*' in name rejected"; else no "reserved '*' in name accepted"; fi

# --- cleanup: tombstone everything we made ('test*' now also covers wide names;
# --- quote the glob so the shell can't expand it) ---
run rm 'test*' >/dev/null 2>&1

echo
echo "$pass passed, $fail failed."
[ "$fail" -eq 0 ]
