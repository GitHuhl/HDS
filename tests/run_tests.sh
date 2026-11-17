#!/usr/bin/env bash
# tests/run_tests.sh -- runs the HDS test programs (built by `make test`) and checks their
# fingerprints against tests/golden.txt.
#
#   tests/run_tests.sh <bindir> <hds.clap> <hds_fx.clap>            run and compare
#   tests/run_tests.sh --update <bindir> <hds.clap> <hds_fx.clap>   run and rewrite tests/golden.txt
#
# A test program prints "ok <name>", "FAIL <name>: ..." or "hash <name> <hex>" lines (see
# tests/test_common.h). This script fails if any program fails, crashes, or prints a hash that is
# missing from or different to tests/golden.txt. The GUI test needs an X display: it uses $DISPLAY
# if set, otherwise starts Xvfb (in nix-shell), and is skipped with a warning if neither exists.
set -u
update=0
if [ "${1:-}" = "--update" ]; then update=1; shift; fi
mkdir -p "$1"; bindir=$(cd "$1" && pwd); synth=$2; fx=$3 # absolute: fontconfig resolves a relative FONTCONFIG_FILE against its own config dir
here=$(cd "$(dirname "$0")" && pwd); root=$(cd "$here/.." && pwd)
golden="$here/golden.txt"
out="$bindir/out"; rm -rf "$out"; mkdir -p "$out"
results="$bindir/results.txt"; : > "$results"
status=0

# Text rendering depends on installed fonts; pin fontconfig to the vendored DejaVu Sans so GUI
# fingerprints match on every machine.
mkdir -p "$bindir/fontcache"
cat > "$bindir/fonts.conf" <<CONF
<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "fonts.dtd">
<fontconfig>
  <dir>$root/third_party/dejavu-fonts</dir>
  <cachedir>$bindir/fontcache</cachedir>
  <alias><family>sans-serif</family><prefer><family>DejaVu Sans</family></prefer></alias>
</fontconfig>
CONF
export FONTCONFIG_FILE="$bindir/fonts.conf"

run() { # name, command...
    local name=$1; shift
    echo "== $name"
    local log="$out/$name.log"
    "$@" > "$log" 2>&1; local rc=$?
    grep -E '^(ok|FAIL|hash) ' "$log" | grep -v '^hash ' | sed 's/^/   /'
    grep -E '^hash ' "$log" >> "$results"
    if [ $rc -ne 0 ]; then
        status=1
        grep -qE '^FAIL ' "$log" || { echo "   FAIL $name exited with status $rc (crash?) -- see $log"; tail -5 "$log" | sed 's/^/     /'; }
    fi
}

run backend "$bindir/test_backend" "$out"
run fx_chain "$bindir/test_fx_chain"
run recording "$bindir/test_recording" "$out"
run clap_render "$bindir/test_clap_render" "$synth" "$fx"
run clap_osc "$bindir/test_clap_osc" "$out" "$synth"
run standalone "$bindir/test_standalone" "$out" "$root/synth_engine"

xvfb_pid=""
if [ -z "${DISPLAY:-}" ] && command -v Xvfb >/dev/null; then
    for n in $(seq 90 120); do
        [ -e "/tmp/.X$n-lock" ] && continue
        Xvfb ":$n" -screen 0 1920x1080x24 -nolisten tcp >/dev/null 2>&1 & xvfb_pid=$!
        for _ in $(seq 50); do [ -e "/tmp/.X11-unix/X$n" ] && break; sleep 0.1; done
        if kill -0 "$xvfb_pid" 2>/dev/null; then export DISPLAY=":$n"; break; fi
        xvfb_pid=""
    done
fi
if [ -n "${DISPLAY:-}" ]; then
    run clap_gui "$bindir/test_clap_gui" "$out" "$synth" "$fx"
else
    echo "== clap_gui"; echo "   SKIP: no X display and no Xvfb (run inside nix-shell)"
fi
[ -n "$xvfb_pid" ] && kill "$xvfb_pid" 2>/dev/null

if [ $update -eq 1 ]; then
    { echo "# Expected fingerprints for tests/run_tests.sh. Regenerate with 'make test-golden' after an"
      echo "# intended change to the sound or the GUI, and review the diff before committing."
      sed 's/^hash //' "$results" | sort; } > "$golden"
    echo "== golden: wrote $(grep -vc '^#' "$golden") fingerprints to tests/golden.txt"
else
    echo "== golden"
    mismatches=0
    while read -r _ name value; do
        expected=$(awk -v n="$name" '$1 == n { print $2 }' "$golden" 2>/dev/null)
        if [ -z "$expected" ]; then echo "   NEW  $name (not in tests/golden.txt -- run 'make test-golden')"; mismatches=$((mismatches+1))
        elif [ "$expected" != "$value" ]; then echo "   DIFF $name: expected $expected, got $value"; mismatches=$((mismatches+1)); fi
    done < "$results"
    total=$(wc -l < "$results")
    if [ $mismatches -eq 0 ]; then echo "   ok   all $total fingerprints match"
    else echo "   $mismatches of $total fingerprints differ (GUI captures are in $out)"; status=1; fi
fi

if [ $status -eq 0 ]; then echo "PASS"; else echo "FAILED"; fi
exit $status
