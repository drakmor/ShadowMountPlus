#!/bin/bash
# Build shadowmountplus.elf locally using Docker.
# Usage:
#  ./build.sh
# Usage with cache disabled:
#  SMP_FORCE_REBUILD_IMAGE=1 ./build.sh

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DOCKER_IMAGE="${SMP_DOCKER_IMAGE:-ubuntu:24.04}"
APT_PACKAGES=(
  autoconf
  automake
  build-essential
  clang-18
  curl
  git
  libarchive-tools
  libtool
  lld-18
  makepkg
  meson
  pacman-package-manager
  pkg-config
  tcl
  xxd
  zip
)

# log prints a consistently prefixed status/error message.
log() {
  printf '[BUILD] %s\n' "$*"
}

# usage prints command-line help for this build entrypoint.
usage() {
  cat <<'EOF'
Build shadowmountplus.elf locally using Docker.

Usage:
  ./build.sh

Optional env vars:
  SMP_FORCE_REBUILD_IMAGE=1   Rebuild the cached Docker image before build
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

if ! command -v docker >/dev/null 2>&1; then
  log "Docker is not installed or not available in PATH."
  log "Please install Docker Desktop/Engine and try again."
  exit 1
fi

if ! docker info >/dev/null 2>&1; then
  log "Docker is installed, but the daemon is not reachable."
  log "Please start Docker and try again."
  exit 1
fi

if ! docker image inspect "$DOCKER_IMAGE" >/dev/null 2>&1; then
  log "Pulling image: $DOCKER_IMAGE"
  docker pull "$DOCKER_IMAGE" >/dev/null
fi

sanitize_image_key() {
  printf '%s' "$1" |
    tr '[:upper:]' '[:lower:]' |
    tr '/:@' '---' |
    tr -cd '[:alnum:]._-'
}

DEFAULT_LOCAL_BUILD_IMAGE="smp-build-cache:$(sanitize_image_key "$DOCKER_IMAGE")"
LOCAL_BUILD_IMAGE="$DEFAULT_LOCAL_BUILD_IMAGE"
FORCE_REBUILD_IMAGE="${SMP_FORCE_REBUILD_IMAGE:-0}"
APT_PACKAGES_INLINE="$(printf '%s ' "${APT_PACKAGES[@]}")"

if [[ "$FORCE_REBUILD_IMAGE" == "1" ]] || ! docker image inspect "$LOCAL_BUILD_IMAGE" >/dev/null 2>&1; then
  log "Preparing cached build image: $LOCAL_BUILD_IMAGE"
  docker build -t "$LOCAL_BUILD_IMAGE" -f - "$ROOT_DIR" <<EOF
FROM $DOCKER_IMAGE
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y $APT_PACKAGES_INLINE && rm -rf /var/lib/apt/lists/*
RUN useradd -m -s /bin/bash smpbuilder
RUN git clone --depth 1 https://github.com/ps5-payload-dev/pacbrew-repo /opt/pacbrew-repo
RUN git clone --depth 1 https://github.com/ps5-payload-dev/sdk /opt/ps5-sdk
RUN chown -R smpbuilder:smpbuilder /opt/pacbrew-repo /opt/ps5-sdk
RUN ln -sf /proc/self/mounts /etc/mtab
RUN su - smpbuilder -c 'cd /opt/pacbrew-repo/sdk && makepkg -c -f'
RUN pacman --noconfirm -U /opt/pacbrew-repo/sdk/ps5-payload-*.pkg.tar.gz
RUN su - smpbuilder -c 'cd /opt/pacbrew-repo/sqlite && makepkg -c -f'
RUN pacman --noconfirm -U /opt/pacbrew-repo/sqlite/ps5-payload-*.pkg.tar.gz
EOF
else
  log "Using cached build image: $LOCAL_BUILD_IMAGE"
fi

RUNTIME_IMAGE="$LOCAL_BUILD_IMAGE"

# BUILD_CMD runs inside the container to install dependencies, prepare the
# cached SDK components, and build the ELF.
BUILD_CMD=$(cat <<'EOF'
  set -euo pipefail
  cd /workspace
  test -f /opt/ps5-sdk/sce_stubs/libkernel_sys.c || {
    printf '[BUILD] Missing required stub: %s\n' /opt/ps5-sdk/sce_stubs/libkernel_sys.c
    exit 1
  }
  make clean all PS5_SCE_STUBS_DIR="/opt/ps5-sdk/sce_stubs"
EOF
)

log "Building in Docker..."
docker run --rm \
  --user "$(id -u):$(id -g)" \
  -v "$ROOT_DIR:/workspace" \
  -w /workspace \
  "$RUNTIME_IMAGE" \
  /bin/bash -lc "$BUILD_CMD"

if [[ ! -f "$ROOT_DIR/shadowmountplus.elf" ]]; then
  log "Build did not produce shadowmountplus.elf"
  exit 1
fi

log "Done."
log "Artifact: $ROOT_DIR/shadowmountplus.elf"
