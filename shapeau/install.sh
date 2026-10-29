#!/usr/bin/env bash
#
# Builds Shapeau and installs it where CLAP hosts look, so Bitwig Studio (and
# any other CLAP host) picks it up.
#
#   ./install.sh [--vst3]
#
# A thin wrapper over ../shared/tools/install-plugin.sh: "shapeau" is a usable
# shell identifier, so the shared script can derive everything it needs from
# the folder and the project name. The wrapper exists so the plugin builds the
# same way as the rest of the collection.
#
# Override the destination with SHAPEAU_PREFIX=/some/where ./install.sh
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

exec "${here}/../shared/tools/install-plugin.sh" "${here}" "$@"
