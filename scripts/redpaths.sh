#!/bin/sh
# Proves the gate turns red for each rule an agent might break.
# Each case copies the posture example into a fresh git repo, starts a
# ticket, breaks one rule, and expects the gate to fail at a named check.
#
# Approving a ticket needs a terminal, so the script plays the architect
# through `script(1)`, which gives the command a pseudo-terminal.
#
# Usage: scripts/redpaths.sh [path/to/stategate]
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
SG=$(cd "$(dirname "${1:-$ROOT/build/stategate}")" && pwd)/$(basename "${1:-$ROOT/build/stategate}")
WORK=$(mktemp -d)
export CARGO_TARGET_DIR="$WORK/target"
trap 'rm -rf "$WORK"' EXIT
PASSED=0
FAILED=0
command -v script > /dev/null || { echo "redpaths.sh needs script(1) from util-linux"; exit 2; }

# architect <phrase> <stategate args...>: types the phrase at a terminal.
architect() {
    phrase=$1
    shift
    printf '%s\n' "$phrase" | script -qec "$SG $*" /dev/null
}

setup() {
    dir="$WORK/case"
    rm -rf "$dir"
    mkdir -p "$dir"
    (cd "$ROOT/examples/posture" && tar cf - --exclude=./target --exclude=./Cargo.lock .) | (cd "$dir" && tar xmf -)   # -m: fresh mtimes, so cargo rebuilds each case
    cd "$dir"
    rm -f .fsm/tickets/*.yaml .fsm/gate.json
    cat > .fsm/tickets/T900.yaml <<'EOF'
id: T900
title: "red path case"
step: step0
status: open
base: ""
failures: 0
nodes: ["posture.Nudging"]
files: ["src/actions/start_nudge.rs", "src/actions/stim_off.rs", "tests/logic_*.rs"]
EOF
    printf '\n## T900\nTouches posture.Nudging.\n' >> CHANGELOG.md
    git init -q . && git add -A && git -c user.name=t -c user.email=t@t commit -qm base
    [ "${1:-}" = "--no-start" ] || architect approve ticket start T900 > /dev/null
}

# expect <description> <check name>   (empty name = expect green)
expect() {
    out=$("$SG" gate 2>&1 || true)
    if [ -z "$2" ]; then
        if echo "$out" | grep -q "GATE GREEN"; then ok "$1"; else bad "$1" "expected green"; fi
    elif echo "$out" | grep -q "GATE RED  at check [0-9]* ($2)"; then
        ok "$1"
    else
        bad "$1" "expected red at $2"
    fi
}

ok() { PASSED=$((PASSED + 1)); printf '  ok    %s\n' "$1"; }
bad() {
    FAILED=$((FAILED + 1))
    printf '  FAIL  %s (%s)\n' "$1" "$2"
    echo "$out" | sed 's/^/        /'
}

echo "stategate red paths"

setup
expect "untouched example is green" ""

setup
echo "// tweak" >> src/gen/machine.rs
expect "hand-edited generated file" drift

setup
sed -i '/@node posture.Nudging/d' src/actions/start_nudge.rs
expect "trace tag removed" trace_tags

setup
echo "pub fn sneaky() {}" > src/sneaky.rs
expect "file outside the ticket" ticket_scope

setup
sed -i 's/max: 3 }/max: 8 }/' posture.yaml
expect "locked intensity limit raised without sign-off" signoff

setup
sed -i 's/max: 3 }/max: 8 }/' posture.yaml
"$SG" gen > /dev/null
expect "locked limit raised and regenerated" signoff

setup
sed -i 's/ignore: \[POWER_ON, CALIBRATED, UPRIGHT, GRACE_ELAPSED, NUDGE_DONE\]/ignore: [POWER_ON, CALIBRATED, GRACE_ELAPSED, NUDGE_DONE]/' posture.yaml
expect "event neither handled nor ignored" statechart

setup
awk '/id: posture.Calibrating/{c=1} /id: posture.Monitoring/{c=0} c && /FAULT: posture.Off/{sub(/posture.Off/, "posture.Monitoring")} {print}' posture.yaml > p.tmp && mv p.tmp posture.yaml
expect "a state no longer sends FAULT to Off" invariants

setup
sed -i '1a // @stub' src/actions/stim_off.rs
expect "locked action marked as a stub" stubs

setup
sed -i 's/stim_set_intensity(0)/stim_set_intensity(1)/' src/actions/stim_off.rs
expect "logic test fails" tests

setup
sed -i 's/hal.stim_set_intensity(5);/hal.stim_set_intensity(5);\n    panic!("broken");/' src/actions/start_nudge.rs
expect "action panics in a generated test" tests

setup
out=$("$SG" gate --release 2>&1 || true)
if echo "$out" | grep -q "GATE RED  at check 8 (stubs)"; then ok "release blocked while stubs remain"; else bad "release blocked while stubs remain" "expected red at stubs"; fi

setup
echo "x" > src/sneaky.rs
"$SG" gate > /dev/null 2>&1 || true
"$SG" gate > /dev/null 2>&1 || true
"$SG" gate > /dev/null 2>&1 || true
rm src/sneaky.rs
out=$("$SG" gate 2>&1 || true)
if echo "$out" | grep -q "ticket T900 is locked after 3 failed gate runs"; then ok "ticket locks after 3 failed runs"; else bad "ticket locks after 3 failed runs" "expected locked ticket"; fi
out=$("$SG" ticket split T900 "a" "b" < /dev/null 2>&1 || true)
if echo "$out" | grep -q "needs the architect"; then ok "split refused without the architect"; else bad "split refused without the architect" "expected refusal"; fi

# Tickets: an agent may write them; only the architect starts them.

setup --no-start
out=$("$SG" ticket start T900 < /dev/null 2>&1 || true)
if echo "$out" | grep -q "needs the architect" && grep -q "status: open" .fsm/tickets/T900.yaml; then
    ok "ticket start refused without the architect"
else
    bad "ticket start refused without the architect" "expected refusal"
fi

setup --no-start
sed -i "s/status: open/status: active/; s/base: \"\"/base: \"$(git rev-parse HEAD)\"/" .fsm/tickets/T900.yaml
expect "ticket set active by hand, never approved" ticket_scope

setup
sed -i 's|^files:|files:\n  - "src/**"|' .fsm/tickets/T900.yaml
echo "pub fn sneaky() {}" > src/sneaky.rs
expect "ticket widened after approval" ticket_scope

setup --no-start
out=$("$SG" ticket new T901 --title "x" --step step0 --node posture.Nudging --file 'src/gen/*.rs' 2>&1 || true)
if echo "$out" | grep -q "covers protected file" && [ ! -e .fsm/tickets/T901.yaml ]; then
    ok "ticket new rejects a ticket that grants generated files"
else
    bad "ticket new rejects a ticket that grants generated files" "expected rejection"
fi

setup --no-start
rm .fsm/tickets/T900.yaml
"$SG" ticket new T900 --title "red path case" --step step0 --node posture.Nudging \
    --file src/actions/start_nudge.rs --file src/actions/stim_off.rs --file 'tests/logic_*.rs' > /dev/null
git add -A && git -c user.name=t -c user.email=t@t commit -qm "ticket from the writer"
architect approve ticket start T900 > /dev/null
expect "ticket from ticket new, approved by the architect, is green" ""

echo "$PASSED passed, $FAILED failed"
[ "$FAILED" -eq 0 ]
