#!/usr/bin/env bash
# End-to-end: ogm talks to ogmfake over real TCP sockets, once as AX.25 over
# KISS with frames being lost, once over the Telnet login.  Each side starts
# with one message for the other; both must arrive.
#
#   tests/e2e_cli.sh path/to/ogm path/to/ogmfake
set -euo pipefail
OGM=$1
FAKE=$2
work=$(mktemp -d)
cleanup() { kill $(jobs -p) 2>/dev/null || true; rm -rf "$work"; }
trap cleanup EXIT
# Never read or touch the real user's settings, keyring entry or mailbox.
export XDG_CONFIG_HOME="$work/config" XDG_DATA_HOME="$work/data"
unset OGM_CALL

count() { find "$1" -name '*.b2f' 2>/dev/null | wc -l; }
wait_port() {
    for _ in $(seq 50); do
        (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null && return 0
        sleep 0.1
    done
    echo "ogmfake did not start on port $1" >&2
    return 1
}

run_case() {  # name, fake args, connect args
    local name=$1 fakeargs=$2 connect=$3 port=$((20000 + RANDOM % 20000))
    local me="$work/$name/me" rms="$work/$name/rms"
    echo "Welcome to the bench." | "$OGM" --call W1AW --mailbox "$rms" compose --to N0CALL --subject "From the RMS" >/dev/null
    printf 'Hello from the field.\n' | "$OGM" --call N0CALL --mailbox "$me" compose --to friend@example.com --subject "$name" >/dev/null
    # shellcheck disable=SC2086
    "$FAKE" --port "$port" --password pw --mailbox "$rms" $fakeargs >"$work/$name.fake.log" 2>&1 &
    wait_port "$port"
    # shellcheck disable=SC2086
    if ! OGM_PASSWORD=pw timeout 180 "$OGM" --call N0CALL --mailbox "$me" connect $connect "127.0.0.1:$port" $([ "$name" = ax25 ] && echo W1AW-10) </dev/null; then
        echo "--- $name: ogm failed; gateway log:" >&2
        cat "$work/$name.fake.log" >&2
        return 1
    fi
    kill %% 2>/dev/null || true
    local sent got delivered left
    sent=$(count "$me/sent"); got=$(count "$me/in"); delivered=$(count "$rms/in"); left=$(count "$me/out")
    echo "$name: sent=$sent received=$got gateway_received=$delivered outbox_left=$left"
    [ "$sent" -eq 1 ] && [ "$got" -eq 1 ] && [ "$delivered" -eq 1 ] && [ "$left" -eq 0 ]
}

run_case ax25 "--drop 8" kiss-tcp
run_case telnet "--telnet" telnet
echo "e2e: ok"
