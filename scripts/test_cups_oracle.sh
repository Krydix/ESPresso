#!/bin/sh
set -eu

root_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir="$root_dir/build/host-tests"
mkdir -p "$build_dir"

"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -pedantic \
  -I"$root_dir/main" \
  "$root_dir/main/ipp_codec.c" \
  "$root_dir/tests/ipp_oracle_server.c" \
  -o "$build_dir/ipp_oracle_server"

"$build_dir/ipp_oracle_server" &
server_pid=$!
trap 'kill "$server_pid" 2>/dev/null || true' EXIT INT TERM
sleep 1
ipptool -L -t ipp://127.0.0.1:18631/ipp/print "$root_dir/tests/cups-oracle.test"
wait "$server_pid"
trap - EXIT INT TERM
