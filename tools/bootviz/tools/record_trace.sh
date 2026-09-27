#!/bin/sh
# Re-records data/: boots the Tektronix image headlessly with //:hal_trace
# (in the repository root), logs every primitive, HAL and file-system call,
# clicks the yellow button on the desktop, and saves the display at the
# moments the visualisation shows. Extra arguments are passed to Bazel.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
root=$(cd "$here/../.." && pwd)
cd "$root"
bazel build "$@" //:hal_trace
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cycles="832 8064 8818 9667 10700 13000 17000 21400 31000 35000 41000 45000 90000 1900000"
args=""
for c in $cycles; do args="$args -display-at $c $tmp/$c.pbm"; done
# The yellow (menu) button goes down 300 virtual ms after resume, over the
# desktop background, and is still held when the last screen is saved.
# shellcheck disable=SC2086
./bazel-bin/hal_trace -cycles 1900001 -click 300 800 850 $args \
    >"$here/data/tektronix_trace.txt" 2>/dev/null
for c in $cycles; do
  python3 "$here/tools/pbm2png.py" "$tmp/$c.pbm" \
      "$here/data/screens/$(printf 'screen_%07d.png' "$c")"
done
echo "wrote $here/data/tektronix_trace.txt and $(echo $cycles | wc -w) screens"
