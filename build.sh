#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DOCKER_IMAGE="${SMP_DOCKER_IMAGE:-ubuntu:24.04}"

log() {
  printf '[build.sh] %s\n' "$*"
}

usage() {
  cat <<'EOF'
Build shadowmountplus.elf locally using Docker.

Usage:
  ./build.sh

Optional env vars:
  SMP_DOCKER_IMAGE   Docker image for the Linux build environment (default: ubuntu:24.04)
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

BUILD_CMD=$(cat <<'EOF'
set -euo pipefail
cd /workspace

apt-get update
DEBIAN_FRONTEND=noninteractive apt-get install -y \
  autoconf \
  automake \
  build-essential \
  clang-18 \
  curl \
  git \
  libarchive-tools \
  libtool \
  lld-18 \
  makepkg \
  meson \
  pacman-package-manager \
  pkg-config \
  tcl \
  xxd \
  zip

HOST_UID="${HOST_UID:-1000}"
HOST_GID="${HOST_GID:-1000}"
BUILDER_USER="smpbuilder"

if ! getent group "$BUILDER_USER" >/dev/null 2>&1; then
  groupadd -g "$HOST_GID" "$BUILDER_USER" 2>/dev/null || groupadd "$BUILDER_USER"
fi
if ! id -u "$BUILDER_USER" >/dev/null 2>&1; then
  useradd -m -s /bin/bash -u "$HOST_UID" -g "$BUILDER_USER" "$BUILDER_USER" || \
    useradd -m -s /bin/bash -g "$BUILDER_USER" "$BUILDER_USER"
fi

if [ ! -d /workspace/pacbrew-repo/.git ]; then
  rm -rf /workspace/pacbrew-repo
  git clone --depth 1 https://github.com/ps5-payload-dev/pacbrew-repo /workspace/pacbrew-repo
fi
if [ ! -d /workspace/ps5-sdk/.git ]; then
  rm -rf /workspace/ps5-sdk
  git clone --depth 1 https://github.com/ps5-payload-dev/sdk /workspace/ps5-sdk
fi

su - "$BUILDER_USER" -c 'cd /workspace/pacbrew-repo/sdk && makepkg -c -f'
pacman --noconfirm -U /workspace/pacbrew-repo/sdk/ps5-payload-*.pkg.tar.gz

su - "$BUILDER_USER" -c 'cd /workspace/pacbrew-repo/sqlite && makepkg -c -f'
pacman --noconfirm -U /workspace/pacbrew-repo/sqlite/ps5-payload-*.pkg.tar.gz

cd /workspace
make clean all PS5_SCE_STUBS_DIR="/workspace/ps5-sdk/sce_stubs"
chown "$HOST_UID:$HOST_GID" /workspace/shadowmountplus.elf || true
EOF
)

log "Building in Docker..."
docker run --rm \
  -e HOST_UID="$(id -u)" \
  -e HOST_GID="$(id -g)" \
  -v "$ROOT_DIR:/workspace" \
  -w /workspace \
  "$DOCKER_IMAGE" \
  /bin/bash -lc "$BUILD_CMD"

if [[ ! -f "$ROOT_DIR/shadowmountplus.elf" ]]; then
  log "Build did not produce shadowmountplus.elf"
  exit 1
fi

log "Done."
log "Artifact: $ROOT_DIR/shadowmountplus.elf"
