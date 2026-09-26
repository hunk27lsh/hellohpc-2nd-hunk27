#!/usr/bin/env bash

set -u

if [ "$#" -ne 1 ]; then
	echo "usage: $0 <tasks.txt>" >&2
	exit 2
fi

TASKS=$1
BIN=$(dirname "$0")/md5fastcoll

if [ ! -r "$TASKS" ]; then
	echo "$0: cannot read task file: $TASKS" >&2
	exit 1
fi

if [ ! -x "$BIN" ]; then
	echo "$0: $BIN not found or not executable; run 'make all' first" >&2
	exit 1
fi

# md5fastcoll reads the task file itself and uses every CPU in the current
# affinity mask: per input file it races independent collision searches, one
# per worker thread, and keeps the first result.
exec "$BIN" "$TASKS"
