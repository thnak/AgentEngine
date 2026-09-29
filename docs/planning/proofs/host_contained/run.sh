#!/usr/bin/env bash
# ADR-211 prove phase: build contained.cpp and run the correct configuration plus every control, each in
# its own resource-capped container (CLAUDE.md "Machine safety": the fork-bomb claim C4 must not be able
# to take the machine with it, so --pids-limit, --memory and --cpus are always set).
#
# Needs: Linux, g++-14 (static), Docker. Exit status 0 only if the correct configuration passes every
# check AND every control fails exactly the check it exists to break.
set -u
here="$(cd "$(dirname "$0")" && pwd)"
bin="$(mktemp -d)/contained"
g++-14 -std=c++23 -O2 -Wall -Wno-unused-result -static -pthread -o "$bin" "$here/contained.cpp" || exit 2
printf 'FROM alpine:latest\nRUN apk add --no-cache iptables ip6tables\n' | docker build -q -t ae-adr211-n2 - >/dev/null || exit 2

caps=(--pids-limit 256 --memory 512m --cpus 1)
run() {  # run <network args...> -- <contained args...>
    local net=()
    while [ "$1" != "--" ]; do net+=("$1"); shift; done
    shift
    timeout 180 docker run --rm "${caps[@]}" "${net[@]}" -v "$bin:/contained:ro" ae-adr211-n2 /contained "$@" 2>&1
}

status=0
expect() {  # expect <name> <regex that must appear> <output>
    if grep -qE "$2" <<<"$3"; then echo "PASS  $1"; else echo "FAIL  $1  (wanted /$2/)"; status=1; fi
}

n2=(--network bridge --cap-add NET_ADMIN)
out=$(run "${n2[@]}" -- launch --n2);             expect "correct configuration, N2"        '^[0-9]+ checks, 0 failed' "$out"
out=$(run --network none --cap-add NET_ADMIN -- launch --n2); expect "correct configuration, N1 + rules" '^[0-9]+ checks, 0 failed' "$out"
# Finding, kept as a check: `--network none` still has loopback, so without the owner rules the executor
# reaches every listener the engine has. N1 alone is NOT executor isolation; the rules are mandatory in both.
out=$(run --network none -- launch);              expect "finding: N1 without rules leaks loopback" '\[FAIL\] C5: the executor cannot connect' "$out"
out=$(run --network none -- launch --same-uid);   expect "control: X = E refused before any command" 'PREFLIGHT REFUSED \(P0' "$out"
out=$(run --network none -- launch --leak-canary);    expect "control: readable canary refused" 'preflight FAIL P2' "$out"
out=$(run --network none -- launch --broker-dumpable); expect "control: dumpable broker refused" 'preflight FAIL P8' "$out"
out=$(run --network none -- launch --no-reap);    expect "control: no reap breaks C3"       '\[FAIL\] C3' "$out"
out=$(run --network none -- launch --no-nproc);   expect "control: no nproc breaks C4"      '\[FAIL\] C4: the engine can fork' "$out"
out=$(run --network none -- launch --no-fixup);   expect "control: no fixup breaks P5"      '\[FAIL\] P5-attack' "$out"
out=$(run "${n2[@]}" -- launch);                  expect "control: no N2 rules breaks C5"   '\[FAIL\] C5: the executor cannot connect' "$out"
out=$(run --network none -- engine);              expect "control: no broker fd refused"    'C6: no broker fd' "$out"
out=$(run "${n2[@]}" -- launch --n2 --same-uid --force); expect "control: X = E, forced past preflight: the reap kills the engine" 'ENGINE KILLED by signal 9' "$out"
out=$(run "${n2[@]}" -- launch --n2 --no-normalize);   expect "control: no reset() normalize breaks C11" '\[FAIL\] C11' "$out"
out=$(run "${n2[@]}" -- launch --n2 --no-tmp-sweep);   expect "control: no /tmp sweep breaks C10"     '\[FAIL\] C10' "$out"
exit $status
