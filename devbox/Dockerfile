# The build and test toolchain for every plugin in this repository, in one
# image, so a machine needs nothing but Docker to build, test and release.
#
# Built and entered through ../devbox.sh, which mounts the repository at its
# own absolute path (so build trees and CMake caches stay valid on both sides)
# and runs as the calling user (so nothing it writes is owned by root).
#
# What is in it, and what it is for:
#   build-essential cmake ninja       Linux builds of every plugin and tool
#   mingw-w64                         Windows cross-builds
#   meson python3-venv curl           setup-winbuild.sh: the Windows Cairo
#   wine64                            running the Windows self-tests and GUI hosts
#   libcairo2-dev libx11-dev ...      the X11 plugin windows
#   xvfb xdotool imagemagick          GUI tests and screenshots, on a private display
#   python3-markdown wkhtmltopdf      the PDF manuals (poppler-utils: pdfinfo,
#   poppler-utils                     which make-manual.sh counts pages with)
#   ffmpeg lame sox                   demo tracks
#   python3-numpy python3-scipy       the analysis helpers
#   zip git                           release.sh
#   fonts-inter fonts-noto-core ...   the fonts the windows and manuals are drawn in
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive \
    LANG=C.UTF-8 \
    LC_ALL=C.UTF-8

RUN dpkg --add-architecture i386 \
 && apt-get update \
 && apt-get install -y --no-install-recommends \
      build-essential cmake ninja-build pkg-config git ca-certificates curl \
      zip unzip xz-utils file binutils \
      mingw-w64 \
      meson python3 python3-venv python3-pip \
      python3-markdown python3-numpy python3-scipy \
      wine64 wine \
      libcairo2-dev libx11-dev libxext-dev libxrandr-dev libxcursor-dev \
      xvfb xauth xdotool x11-utils imagemagick \
      wkhtmltopdf poppler-utils \
      ffmpeg lame sox libsox-fmt-all \
      fonts-inter fonts-noto-core fonts-noto-mono fonts-dejavu-core fontconfig \
 && rm -rf /var/lib/apt/lists/*

#   mingw-w64 stays on Ubuntu's default (win32) alternative: it is the compiler
#   every Windows release so far was built with ("GCC: (GNU) 13-win32").

# clap-validator is a Rust program; it is built from the CLAP/ checkout on the
# host when needed (see devbox.sh), so no toolchain for it lives in the image.

# A home that belongs to whoever runs the container. Everything a plugin writes
# into ~/.config, ~/.local/share or a wine prefix lands here and dies with the
# container -- never in the user's real home.
RUN mkdir -m 1777 /home/dev
ENV HOME=/home/dev \
    WINEDEBUG=-all \
    WINEPREFIX=/home/dev/.wine

WORKDIR /work
CMD ["bash"]
