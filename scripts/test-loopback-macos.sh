#!/bin/bash
# ARQ loopback test for the macOS PortAudio backend.
#
# Runs two ardopcf instances that talk to each other through the
# BlackHole 2ch virtual audio device (https://existential.audio/blackhole/,
# `brew install blackhole-2ch`) and verifies that an ARQ connection can be
# established and data flows in both directions.
#
# Because ardopcf closes its capture stream while transmitting (see
# SHARECAPTURE), both instances can share the same BlackHole device without
# decoding their own transmissions.
#
# The terminal application running this script needs microphone permission
# (macOS TCC); the first run should trigger the permission prompt.
#
# Usage: scripts/test-loopback-macos.sh [seconds]   (default timeout 60)

set -u
cd "$(dirname "$0")/.."

TIMEOUT=${1:-60}
DEV="BlackHole 2ch"
LOG_A=/tmp/ardop_loop_a.log
LOG_B=/tmp/ardop_loop_b.log
HOST_A=/tmp/ardop_loop_hosta.log
HOST_B=/tmp/ardop_loop_hostb.log

fail() { echo "FAIL: $1"; exit 1; }

[ -x ./ardopcf ] || fail "./ardopcf not built (run make first)"

system_profiler SPAudioDataType 2>/dev/null | grep -q "BlackHole 2ch" \
	|| fail "BlackHole 2ch not installed (brew install blackhole-2ch)"

pkill -9 ardopcf 2>/dev/null
sleep 1

echo "Starting instance A (port 8515) and B (port 8517) on \"$DEV\" ..."
./ardopcf --nologfile -H "CONSOLELOG 1" 8515 "$DEV" "$DEV" >"$LOG_A" 2>&1 &
PID_A=$!
./ardopcf --nologfile -H "CONSOLELOG 1" 8517 "$DEV" "$DEV" >"$LOG_B" 2>&1 &
PID_B=$!
sleep 2

kill -0 $PID_A 2>/dev/null || { cat "$LOG_A"; fail "instance A died"; }
kill -0 $PID_B 2>/dev/null || { cat "$LOG_B"; fail "instance B died"; }

echo "Configuring B (listener) ..."
python3 scripts/loopback_host.py 8517 \
	"MYCALL HB9HJI-2" "ARQBW 500MAX" "LISTEN TRUE" >"$HOST_B" 2>&1 &
HOSTB_PID=$!
sleep 2

echo "A calls B via ARQCALL ..."
python3 scripts/loopback_host.py 8515 \
	"MYCALL HB9HJI-1" "ARQBW 500MAX" "ARQCALL HB9HJI-2 5" >"$HOST_A" 2>&1 &
HOSTA_PID=$!

echo "Waiting up to $TIMEOUT s for the ARQ connection ..."
CONNECTED=""
for ((i = 0; i < TIMEOUT; i++)); do
	if grep -q "CONNECTED HB9HJI-1" "$HOST_B" 2>/dev/null \
		&& grep -q "CONNECTED HB9HJI-2" "$HOST_A" 2>/dev/null; then
		CONNECTED=yes
		break
	fi
	sleep 1
done

kill $HOSTA_PID $HOSTB_PID 2>/dev/null
pkill -INT ardopcf 2>/dev/null
sleep 1
pkill -9 ardopcf 2>/dev/null

echo
echo "=== host A (caller) ==="; tail -14 "$HOST_A"
echo "=== host B (listener) ==="; tail -14 "$HOST_B"
echo

if [ -n "$CONNECTED" ]; then
	echo "PASS: ARQ connection established in both directions."
	exit 0
fi
fail "no ARQ connection within $TIMEOUT s (see $HOST_A, $HOST_B, $LOG_A, $LOG_B)"
