#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
#
# SPDX-License-Identifier: BSD-2-Clause
#
# Builds tools/gdb/testbed/testbed.cpp under GDB in all three supported
# printer-delivery modes and checks that every `// GDB_CHECK:` comment's
# expected substring appears in the corresponding `print` output. See
# README.md in this directory for how to add a case for a new printer.
#
# Requires `reloco`'s and `microfmt`'s `include/` directories on the
# compiler's search path -- set RELOCO_INCLUDE_DIR / MICROFMT_INCLUDE_DIR
# if they aren't checked out as siblings of this repository.
#
# Usage: tools/gdb/testbed/run.sh [--keep-build]
#
#   --keep-build   Don't delete the build/ scratch directory on exit
#                   (useful for inspecting binaries/logs after a failure).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
CXX="${CXX:-g++}"
GDB="${GDB:-gdb}"
RELOCO_INCLUDE_DIR="${RELOCO_INCLUDE_DIR:-${REPO_ROOT}/../reloco/include}"
MICROFMT_INCLUDE_DIR="${MICROFMT_INCLUDE_DIR:-${REPO_ROOT}/../microfmt/include}"

KEEP_BUILD=0
if [[ "${1:-}" == "--keep-build" ]]; then
  KEEP_BUILD=1
fi

cleanup() {
  if [[ "${KEEP_BUILD}" -eq 0 ]]; then
    rm -rf "${BUILD_DIR}"
  fi
}
trap cleanup EXIT

rm -rf "${BUILD_DIR}"
mkdir -p "${BUILD_DIR}"

CXX_INCLUDES=(-I "${REPO_ROOT}/include" -I "${RELOCO_INCLUDE_DIR}" -I "${MICROFMT_INCLUDE_DIR}")

echo "==> Compiling testbed (plain, no printer header)"
"${CXX}" -std=c++17 -g -O0 "${CXX_INCLUDES[@]}" \
  "${SCRIPT_DIR}/testbed.cpp" -o "${BUILD_DIR}/testbed_plain"

echo "==> Compiling testbed (with include/structo/sync/gdb_printers.hpp embedded)"
"${CXX}" -std=c++17 -g -O0 "${CXX_INCLUDES[@]}" \
  "${SCRIPT_DIR}/testbed_embedded.cpp" -o "${BUILD_DIR}/testbed_embedded"

# A `<binary>-gdb.py` next to `testbed_plain` that just imports the standalone
# module, to exercise GDB's "auto-load by file name" mechanism as well as
# the embedded-section mechanism.
cat > "${BUILD_DIR}/testbed_plain-gdb.py" <<PYEOF
import sys
sys.path.insert(0, "${SCRIPT_DIR}/..")
import structo_printers  # noqa: F401
PYEOF

BREAK_LINE="$(grep -n '// GDB_BREAK$' "${SCRIPT_DIR}/testbed.cpp" | head -1 | cut -d: -f1)"
if [[ -z "${BREAK_LINE}" ]]; then
  echo "error: could not find '// GDB_BREAK' marker in testbed.cpp" >&2
  exit 1
fi

mapfile -t CHECKS < <(grep -oP '// GDB_CHECK:\s*\K.*=>.*' "${SCRIPT_DIR}/testbed.cpp")
if [[ "${#CHECKS[@]}" -eq 0 ]]; then
  echo "error: found no '// GDB_CHECK:' comments in testbed.cpp" >&2
  exit 1
fi
echo "==> Found ${#CHECKS[@]} GDB_CHECK case(s), breakpoint at testbed.cpp:${BREAK_LINE}"

# Builds a GDB batch command file at $1 for the print commands shared by
# every mode (breakpoint + run + one `print` per GDB_CHECK case).
write_common_commands() {
  local out="$1"
  echo "break testbed.cpp:${BREAK_LINE}" >> "${out}"
  echo "run" >> "${out}"
  local check name
  for check in "${CHECKS[@]}"; do
    name="${check%%=>*}"
    name="${name#"${name%%[![:space:]]*}"}" # trim leading space
    name="${name%"${name##*[![:space:]]}"}" # trim trailing space
    echo "print ${name}" >> "${out}"
  done
  echo "quit" >> "${out}"
}

# Runs `$1` (a binary) through GDB with the pre-existing $2..$n leading
# commands/options, then verifies every GDB_CHECK substring appears
# somewhere in the transcript. Prints a PASS/FAIL line per case.
run_mode() {
  local mode_name="$1"
  local binary="$2"
  shift 2
  local cmdfile="${BUILD_DIR}/cmds_${mode_name}.gdb"
  : > "${cmdfile}"
  write_common_commands "${cmdfile}"

  local transcript
  transcript="$("${GDB}" -q -batch -nx "$@" -x "${cmdfile}" "${binary}" 2>&1 || true)"
  echo "${transcript}" > "${BUILD_DIR}/transcript_${mode_name}.txt"

  echo "--- mode: ${mode_name} ---"
  local check name expected ok=1
  for check in "${CHECKS[@]}"; do
    name="${check%%=>*}"
    expected="${check#*=>}"
    name="${name#"${name%%[![:space:]]*}"}"
    name="${name%"${name##*[![:space:]]}"}"
    expected="${expected#"${expected%%[![:space:]]*}"}"
    expected="${expected%"${expected##*[![:space:]]}"}"
    if grep -qF -- "${expected}" <<< "${transcript}"; then
      echo "  PASS  ${name}: found \"${expected}\""
    else
      echo "  FAIL  ${name}: expected substring \"${expected}\" not found"
      ok=0
    fi
  done
  return $((1 - ok))
}

OVERALL=0

run_mode "source-script" "${BUILD_DIR}/testbed_plain" \
  -iex "source ${REPO_ROOT}/tools/gdb/structo_printers.py" || OVERALL=1

run_mode "autoload-gdb-py" "${BUILD_DIR}/testbed_plain" \
  -iex "set auto-load safe-path ${BUILD_DIR}" || OVERALL=1

run_mode "embedded-header" "${BUILD_DIR}/testbed_embedded" \
  -iex "set auto-load safe-path ${BUILD_DIR}" || OVERALL=1

if [[ "${OVERALL}" -eq 0 ]]; then
  echo "==> All GDB printer checks passed in all three modes."
else
  echo "==> Some GDB printer checks FAILED; see transcripts in ${BUILD_DIR} (use --keep-build to inspect)." >&2
fi
exit "${OVERALL}"
