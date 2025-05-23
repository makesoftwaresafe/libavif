#!/bin/bash

# This script will build dav1d for the default ABI targets supported by android.
# This script only works on linux. You must pass the path to the android NDK as
# a parameter to this script.
#
# Android NDK: https://developer.android.com/ndk/downloads
#
# The git tag below is known to work, and will occasionally be updated. Feel
# free to use a more recent commit.

set -e

if [ $# -ne 1 ]; then
  echo "Usage: ${0} <path_to_android_ndk>"
  exit 1
fi
git clone -b 1.5.1 --depth 1 https://code.videolan.org/videolan/dav1d.git
mkdir dav1d/build

# This only works on linux and mac.
if [ "$(uname)" == "Darwin" ]; then
  HOST_TAG="darwin"
else
  HOST_TAG="linux"
fi
android_bin="${1}/toolchains/llvm/prebuilt/${HOST_TAG}-x86_64/bin"

ABI_LIST=("armeabi-v7a" "arm64-v8a" "x86" "x86_64")
ARCH_LIST=("arm" "aarch64" "x86" "x86_64")
for i in "${!ABI_LIST[@]}"; do
  abi="${ABI_LIST[i]}"
  PATH=$PATH:${android_bin} meson setup --default-library=static --buildtype release \
    --cross-file="../../package/crossfiles/${ARCH_LIST[i]}-android.meson" \
    -Denable_tools=false -Denable_tests=false "dav1d/build/${abi}" dav1d
  PATH=$PATH:${android_bin} meson compile -C "dav1d/build/${abi}"
done
