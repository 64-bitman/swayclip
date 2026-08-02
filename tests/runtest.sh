#!/bin/sh

set -e

builddir="$(realpath $1)"

export TEST_DAEMON="$builddir/daemon/swayclip"
export TEST_SERVER="$builddir/tests/testserver"

exec pytest -v ${@:2} tests
