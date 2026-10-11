#!/bin/sh
# Downloads Mark Moxon's flicker-free build of Acornsoft Elite (the disc version, "Stairway to Hell"
# variant) into discs/, where the Presto build embeds it from. Elite is (c) Ian Bell and David
# Braben, so the image isn't part of this repository; see
# https://github.com/markmoxon/elite-source-code-bbc-micro-disc
set -e
mkdir -p "$(dirname "$0")/../discs"
cd "$(dirname "$0")/../discs"
curl -fsSLO https://raw.githubusercontent.com/markmoxon/elite-source-code-bbc-micro-disc/flicker-free/5-compiled-game-discs/elite-disc-flicker-free-sth.ssd
echo "fetched discs/elite-disc-flicker-free-sth.ssd"
