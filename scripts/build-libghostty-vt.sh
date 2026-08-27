#!/usr/bin/env bash
# Cross-compile libghostty-vt static libraries for Android.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck disable=SC1091
source "${ROOT}/native/pin.env"

NATIVE_DIR="${ROOT}/native"
TOOLS_DIR="${NATIVE_DIR}/tools"
VENDOR_DIR="${NATIVE_DIR}/vendor"
GHOSTTY_DIR="${NATIVE_DIR}/ghostty"
ZIG_DIR="${TOOLS_DIR}/zig-${ZIG_VERSION}"

ABIS=(
  "arm64-v8a:aarch64-linux-android"
  "x86_64:x86_64-linux-android"
)

need_build=0
for spec in "${ABIS[@]}"; do
  abi="${spec%%:*}"
  if [[ ! -f "${VENDOR_DIR}/${abi}/libghostty-vt.a" ]]; then
    need_build=1
  fi
done
if [[ ! -d "${VENDOR_DIR}/include/ghostty" ]]; then
  need_build=1
fi
if [[ "${FORCE:-0}" != "1" && "${need_build}" -eq 0 ]]; then
  echo "libghostty-vt vendor libraries already exist."
  exit 0
fi

if [[ -z "${ANDROID_NDK_HOME:-}" ]]; then
  if [[ -n "${ANDROID_HOME:-}" && -d "${ANDROID_HOME}/ndk" ]]; then
    ANDROID_NDK_HOME="$(find "${ANDROID_HOME}/ndk" -mindepth 1 -maxdepth 1 -type d | sort -V | tail -n 1)"
  elif [[ -d "${HOME}/Android/Sdk/ndk" ]]; then
    ANDROID_NDK_HOME="$(find "${HOME}/Android/Sdk/ndk" -mindepth 1 -maxdepth 1 -type d | sort -V | tail -n 1)"
  fi
fi
if [[ -z "${ANDROID_NDK_HOME:-}" || ! -d "${ANDROID_NDK_HOME}" ]]; then
  echo "Set ANDROID_NDK_HOME to an installed Android NDK." >&2
  exit 1
fi
export ANDROID_NDK_HOME
echo "Using NDK: ${ANDROID_NDK_HOME}"

mkdir -p "${TOOLS_DIR}" "${VENDOR_DIR}"

if [[ ! -x "${ZIG_DIR}/zig" ]]; then
  host_arch="$(uname -m)"
  case "${host_arch}" in
    x86_64) zig_arch="x86_64" ;;
    aarch64|arm64) zig_arch="aarch64" ;;
    *)
      echo "Unsupported host arch: ${host_arch}" >&2
      exit 1
      ;;
  esac
  tarball="zig-${zig_arch}-linux-${ZIG_VERSION}.tar.xz"
  url="https://ziglang.org/download/${ZIG_VERSION}/${tarball}"
  echo "Downloading Zig ${ZIG_VERSION} from ${url}"
  curl -fsSL -o "${TOOLS_DIR}/${tarball}" "${url}"
  tar -C "${TOOLS_DIR}" -xJf "${TOOLS_DIR}/${tarball}"
  extracted="$(find "${TOOLS_DIR}" -maxdepth 1 -type d -name "zig-${zig_arch}-linux-${ZIG_VERSION}" | head -n 1)"
  rm -rf "${ZIG_DIR}"
  mv "${extracted}" "${ZIG_DIR}"
  rm -f "${TOOLS_DIR}/${tarball}"
fi
ZIG="${ZIG_DIR}/zig"
echo "Using Zig: $("${ZIG}" version)"

if [[ ! -d "${GHOSTTY_DIR}/.git" ]]; then
  echo "Fetching ghostty ${GHOSTTY_COMMIT}"
  rm -rf "${GHOSTTY_DIR}"
  git clone --filter=blob:none --depth 1 \
    https://github.com/ghostty-org/ghostty.git "${GHOSTTY_DIR}"
fi
git -C "${GHOSTTY_DIR}" fetch --depth 1 origin "${GHOSTTY_COMMIT}"
git -C "${GHOSTTY_DIR}" checkout --detach "${GHOSTTY_COMMIT}"

# -Dsimd=false keeps lib-vt free of simdutf/highway C++ deps.
for spec in "${ABIS[@]}"; do
  abi="${spec%%:*}"
  zig_target="${spec##*:}"
  prefix="${NATIVE_DIR}/out-${abi}"
  echo "Building libghostty-vt for ${abi} (${zig_target})"
  rm -rf "${prefix}"
  (
    cd "${GHOSTTY_DIR}"
    "${ZIG}" build -Demit-lib-vt \
      "-Dtarget=${zig_target}" \
      -Dsimd=false \
      -Doptimize=ReleaseFast \
      --prefix "${prefix}"
  )
  mkdir -p "${VENDOR_DIR}/${abi}"
  lib_src="$(find "${prefix}" -name 'libghostty-vt*.a' | head -n 1)"
  if [[ -z "${lib_src}" ]]; then
    echo "No libghostty-vt static library produced for ${abi}." >&2
    exit 1
  fi
  cp "${lib_src}" "${VENDOR_DIR}/${abi}/libghostty-vt.a"
  echo "${lib_src} -> ${VENDOR_DIR}/${abi}/libghostty-vt.a"
done

include_src="$(find "${NATIVE_DIR}/out-arm64-v8a" -type d -path '*/include/ghostty' | head -n 1)"
if [[ -z "${include_src}" ]]; then
  include_src="${GHOSTTY_DIR}/include/ghostty"
fi
rm -rf "${VENDOR_DIR}/include"
mkdir -p "${VENDOR_DIR}/include"
cp -a "$(dirname "${include_src}")/ghostty" "${VENDOR_DIR}/include/ghostty"
echo "${GHOSTTY_COMMIT}" > "${VENDOR_DIR}/COMMIT"
echo "Vendor libraries ready in ${VENDOR_DIR}"
