#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
#
# SPDX-License-Identifier: BSD-2-Clause

set -euo pipefail

readonly script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly source_dir="$(cd -- "${script_dir}/.." && pwd)"
readonly build_dir="${1:-${source_dir}/build-ci}"
readonly build_type="${2:-Release}"
readonly parallel="${JPLCZ_STRUCTO_BUILD_PARALLEL:-2}"

if (($# >= 1)); then
  shift
fi
if (($# >= 1)); then
  shift
fi

cmake \
  -S "${source_dir}" \
  -B "${build_dir}" \
  -G Ninja \
  "-DCMAKE_BUILD_TYPE=${build_type}" \
  -DJPLCZ_STRUCTO_ENABLE_WERROR=ON \
  "$@"

cmake --build "${build_dir}" --parallel "${parallel}"
ctest \
  --test-dir "${build_dir}" \
  --build-config "${build_type}" \
  --output-on-failure \
  --parallel "${parallel}"
cmake \
  --build "${build_dir}" \
  --target jplcz_structo_check_public_headers \
  --parallel "${parallel}"
