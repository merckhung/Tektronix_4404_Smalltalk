#!/bin/sh
# Regenerates docs/screenshots: one headless frame per stage at its showcase
# moment (Mesa's lavapipe software Vulkan driver is enough). Extra
# arguments are passed to Bazel.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
cd "$here"
bazel build "$@" //:bootviz
mkdir -p docs/screenshots
./bazel-bin/src/app/bootviz --screenshots="$here/docs/screenshots"
