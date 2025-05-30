#!/bin/bash

# This script will build libgav1 for the default ABI targets supported by
# android. You must pass the path to the android NDK as a parameter to this
# script.
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
# When updating the libgav1 version, make the same change to libgav1.cmd.
git clone -b v0.19.0 --depth 1 https://chromium.googlesource.com/codecs/libgav1

mkdir libgav1/build

ABI_LIST="armeabi-v7a arm64-v8a x86 x86_64"
for abi in ${ABI_LIST}; do
  cmake -G Ninja -S libgav1 -B "libgav1/build/${abi}" \
    -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/android.cmake \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DCMAKE_BUILD_TYPE=Release \
    -DLIBGAV1_ANDROID_NDK_PATH=${1} \
    -DLIBGAV1_THREADPOOL_USE_STD_MUTEX=1 \
    -DLIBGAV1_ENABLE_EXAMPLES=0 \
    -DLIBGAV1_ENABLE_TESTS=0 \
    -DLIBGAV1_MAX_BITDEPTH=12 \
    -DANDROID_ABI=${abi}
  cmake --build "libgav1/build/${abi}" --config Release --parallel
done
