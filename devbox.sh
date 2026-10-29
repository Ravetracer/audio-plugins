#!/usr/bin/env bash
#
# Runs a command inside the repository's build container, or opens a shell in
# it. The container has every tool the plugins, their tests and the release
# need (see devbox/Dockerfile); the host needs nothing but Docker.
#
#   ./devbox.sh                                  an interactive shell
#   ./devbox.sh ./release.sh 0.19.0              any command, from the current folder
#   ./devbox.sh --rebuild                        rebuild the image, then a shell
#
# The repository is mounted at its own absolute path and the command runs in
# the folder it was started from, as the calling user. Build trees and their
# CMake caches are therefore valid on both sides, and nothing the container
# writes into the repository belongs to root.
#
# The container's home is its own and is thrown away on exit: a plugin that
# writes ~/.config, ~/.local/share or %APPDATA% under wine writes there, never
# into the user's real home. GUI tests start their own Xvfb inside it; no
# window ever reaches the host's desktop.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
dockerfile="${here}/devbox/Dockerfile"

# The tag carries a hash of the Dockerfile, so an edited Dockerfile builds a
# new image by itself instead of silently running the old one.
tag="audio-plugins-devbox:$(sha256sum "$dockerfile" | cut -c1-12)"

rebuild=0
if [ "${1:-}" = "--rebuild" ]; then
   rebuild=1
   shift
fi

if [ "$rebuild" = 1 ] || ! docker image inspect "$tag" >/dev/null 2>&1; then
   echo "==> building ${tag}" >&2
   docker build -t "$tag" "${here}/devbox" >&2
fi

case "$PWD" in
   "$here"|"$here"/*) workdir="$PWD" ;;
   *) workdir="$here" ;;
esac

tty=()
[ -t 0 ] && [ -t 1 ] && tty=(-it)

[ $# -eq 0 ] && set -- bash

# The home is a tmpfs owned by the calling user: wine refuses a home it does not
# own, and needs exec on it for its prefix.
exec docker run --rm "${tty[@]}" --init \
   --user "$(id -u):$(id -g)" \
   --tmpfs "/home/dev:uid=$(id -u),gid=$(id -g),mode=0755,exec,size=4g" \
   -v "${here}:${here}" \
   -w "$workdir" \
   "$tag" "$@"
