#!/bin/bash
# Copyright 2022 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# ------------------------------------------------------------------------------
#
# tests for command lines (lossless)

source $(dirname "$0")/cmd_test_common.sh

# Input file paths.
INPUT_PNG="${TESTDATA_DIR}/paris_icc_exif_xmp.png"
INPUT_GRAY_PNG="${TESTDATA_DIR}/kodim03_grayscale_gamma1.6.png"
# Output file names.
ENCODED_FILE="avif_test_cmd_lossless_encoded.avif"
DECODED_FILE="avif_test_cmd_lossless_decoded.png"
DECODED_FILE_LOSSLESS="avif_test_cmd_lossless_decoded_lossless.png"

# Cleanup
cleanup() {
  pushd ${TMP_DIR}
    rm -f -- "${ENCODED_FILE}" "${DECODED_FILE}" "${DECODED_FILE_LOSSLESS}"
  popd
}
trap cleanup EXIT

pushd ${TMP_DIR}
  # Generate test data.
  "${AVIFENC}" -s 8 "${INPUT_PNG}" -o "${ENCODED_FILE}"
  "${AVIFDEC}" "${ENCODED_FILE}" "${DECODED_FILE}"

  # Combining some arguments with lossless should fail.
  for option in "-y 420" "--min 0 --max 1" "-r limited" "--cicp 2/2/8"; do
    "${AVIFENC}" $option -s 10 -l "${DECODED_FILE}" -o "${ENCODED_FILE}" && exit 1
  done

  # Combining some arguments with lossless should work.
  for option in "-y 444" "--min 0 --max 0" "-r full"; do
    "${AVIFENC}" $option -s 10 -l "${DECODED_FILE}" -o "${ENCODED_FILE}"
  done

  # Lossless test. The decoded pixels should be the same as the original image.
  echo "Testing basic lossless"
  "${AVIFENC}" -s 10 -l "${INPUT_PNG}" -o "${ENCODED_FILE}"
  "${AVIFDEC}" "${ENCODED_FILE}" "${DECODED_FILE_LOSSLESS}"
  "${ARE_IMAGES_EQUAL}" "${INPUT_PNG}" "${DECODED_FILE_LOSSLESS}" 0

  echo "Testing YCgCo-Re lossless"
  "${AVIFENC}" -s 10 -l --cicp 2/2/16 "${INPUT_PNG}" -o "${ENCODED_FILE}"
  "${AVIFDEC}" "${ENCODED_FILE}" "${DECODED_FILE_LOSSLESS}"
  "${ARE_IMAGES_EQUAL}" "${INPUT_PNG}" "${DECODED_FILE_LOSSLESS}" 0

  echo "Testing YCgCo-Ro lossless fails because the input bit depth is even"
  "${AVIFENC}" -s 10 -l --cicp 2/2/17 "${INPUT_PNG}" -o "${ENCODED_FILE}" && exit 1

  # 400 test.
  echo "Testing 400 lossless"
  "${AVIFENC}" -y 400 -s 10 -l "${INPUT_GRAY_PNG}" -o "${ENCODED_FILE}"
  "${AVIFDEC}" "${ENCODED_FILE}" "${DECODED_FILE_LOSSLESS}"
  "${ARE_IMAGES_EQUAL}" "${INPUT_GRAY_PNG}" "${DECODED_FILE_LOSSLESS}" 0
popd

exit 0
