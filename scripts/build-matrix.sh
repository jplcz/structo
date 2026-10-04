#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
#
# SPDX-License-Identifier: BSD-2-Clause

set -uo pipefail

readonly script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly source_dir="$(cd -- "${script_dir}/.." && pwd)"
readonly build_root="${JPLCZ_STRUCTO_MATRIX_BUILD_ROOT:-${source_dir}/build-matrix}"

read -r -a build_types <<<"${JPLCZ_STRUCTO_BUILD_TYPES:-Debug Release RelWithDebInfo MinSizeRel}"
read -r -a selected_targets <<<"${JPLCZ_STRUCTO_BUILD_TARGETS:-}"

# name|architecture|compiler|clang-target|run-tests
readonly matrix=(
  "x86_64-gcc|x86_64|g++||ON"
  "x86_64-clang|x86_64|clang++||ON"
  "arm32-gcc|arm|arm-linux-gnueabihf-g++||OFF"
  "arm32-clang|arm|clang++|arm-linux-gnueabihf|OFF"
  "aarch64-gcc|aarch64|aarch64-linux-gnu-g++||OFF"
  "aarch64-clang|aarch64|clang++|aarch64-linux-gnu|OFF"
  "riscv64-gcc|riscv64|riscv64-linux-gnu-g++||OFF"
  "riscv64-clang|riscv64|clang++|riscv64-linux-gnu|OFF"
)

contains_target() {
  local candidate="$1"
  local selected

  if ((${#selected_targets[@]} == 0)); then
    return 0
  fi

  for selected in "${selected_targets[@]}"; do
    if [[ "${selected}" == "${candidate}" ]]; then
      return 0
    fi
  done
  return 1
}

require_command() {
  if ! command -v "$1" >/dev/null 2>&1; then
    printf 'error: required command not found: %s\n' "$1" >&2
    return 1
  fi
}

require_command cmake || exit 1
require_command ninja || exit 1
require_command ccache || exit 1

failures=()
build_count=0

for entry in "${matrix[@]}"; do
  IFS='|' read -r name architecture compiler clang_target run_tests <<<"${entry}"

  if ! contains_target "${name}"; then
    continue
  fi

  if ! command -v "${compiler}" >/dev/null 2>&1; then
    failures+=("${name}: compiler '${compiler}' not found")
    continue
  fi

  if [[ -n "${clang_target}" ]]; then
    gcc_compiler="${clang_target}-g++"
    if ! command -v "${gcc_compiler}" >/dev/null 2>&1; then
      failures+=("${name}: GCC sysroot provider '${gcc_compiler}' not found")
      continue
    fi
  fi

  for build_type in "${build_types[@]}"; do
    build_dir="${build_root}/${name}/${build_type}"
    ((build_count += 1))

    if [[ "${JPLCZ_STRUCTO_MATRIX_CLEAN:-0}" == "1" ]]; then
      cmake -E remove_directory "${build_dir}"
    fi

    printf '\n==> Configuring %s / %s\n' "${name}" "${build_type}"

    configure_args=(
      -S "${source_dir}"
      -B "${build_dir}"
      -G Ninja
      "-DCMAKE_BUILD_TYPE=${build_type}"
      "-DCMAKE_CXX_COMPILER=${compiler}"
      -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
      -DJPLCZ_STRUCTO_ENABLE_STRICT_WARNINGS=ON
      -DJPLCZ_STRUCTO_ENABLE_WERROR=ON
      "-DJPLCZ_STRUCTO_BUILD_TESTS=${run_tests}"
      -DJPLCZ_STRUCTO_BUILD_HEADER_CHECKS=ON
    )

    if [[ "${architecture}" != "x86_64" ]]; then
      configure_args+=(
        -DCMAKE_SYSTEM_NAME=Linux
        "-DCMAKE_SYSTEM_PROCESSOR=${architecture}"
        -DCMAKE_DISABLE_FIND_PACKAGE_Boost=ON
        -DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=ON
      )
    fi

    if [[ -n "${clang_target}" ]]; then
      configure_args+=(
        "-DCMAKE_CXX_COMPILER_TARGET=${clang_target}"
        "-DCMAKE_CXX_FLAGS_INIT=--gcc-toolchain=/usr"
      )
    fi

    if ! cmake "${configure_args[@]}"; then
      failures+=("${name}/${build_type}: configure")
      continue
    fi

    printf '\n==> Building %s / %s\n' "${name}" "${build_type}"
    if ! cmake --build "${build_dir}" -j"$(nproc)"; then
      failures+=("${name}/${build_type}: build")
      continue
    fi

    if [[ "${run_tests}" == "ON" &&
          "${JPLCZ_STRUCTO_MATRIX_SKIP_TESTS:-0}" != "1" ]]; then
      printf '\n==> Testing %s / %s\n' "${name}" "${build_type}"
      if ! ctest --test-dir "${build_dir}" --output-on-failure -j"$(nproc)"; then
        failures+=("${name}/${build_type}: tests")
      fi
    fi
  done
done

if ((build_count == 0)); then
  printf 'error: no matrix targets selected\n' >&2
  exit 1
fi

if ((${#failures[@]} != 0)); then
  printf '\nBuild matrix completed with %d failure(s):\n' "${#failures[@]}" >&2
  printf '  - %s\n' "${failures[@]}" >&2
  exit 1
fi

printf '\nBuild matrix completed successfully (%d configurations).\n' \
  "${build_count}"
