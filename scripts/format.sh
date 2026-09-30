#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
#
# SPDX-License-Identifier: BSD-2-Clause

# Applies clang-format (using the repository's .clang-format style) to every
# header and source file under include/, tests/, and examples/ (the latter is
# skipped if it does not exist).
#
# Usage:
#   ./scripts/format.sh            # reformat files in place
#   ./scripts/format.sh --check    # exit non-zero if any file is not formatted
#
# The clang-format binary can be overridden via STRUCTO_CLANG_FORMAT
# (defaults to "clang-format").

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"

clang_format="${STRUCTO_CLANG_FORMAT:-clang-format}"

if ! command -v "${clang_format}" >/dev/null 2>&1; then
  echo "error: '${clang_format}' not found on PATH (set STRUCTO_CLANG_FORMAT to override)" >&2
  exit 1
fi

check_only=0
case "${1:-}" in
  --check)
    check_only=1
    ;;
  "")
    ;;
  *)
    echo "usage: $(basename "$0") [--check]" >&2
    exit 1
    ;;
esac

dirs=()
for d in include tests examples; do
  [[ -d "${d}" ]] && dirs+=("${d}")
done

if [[ ${#dirs[@]} -eq 0 ]]; then
  echo "error: none of include/, tests/, examples/ exist" >&2
  exit 1
fi

mapfile -d '' files < <(find "${dirs[@]}" -type f \( \
  -name '*.hpp' -o -name '*.h' -o -name '*.hh' -o -name '*.hxx' \
  -o -name '*.cpp' -o -name '*.cc' -o -name '*.cxx' \) -print0)

if [[ ${#files[@]} -eq 0 ]]; then
  echo "no files found to format under: ${dirs[*]}" >&2
  exit 0
fi

echo "using $(${clang_format} --version)"
echo "formatting ${#files[@]} file(s) under: ${dirs[*]}"

if [[ ${check_only} -eq 1 ]]; then
  failed=0
  for f in "${files[@]}"; do
    if ! diff -q <("${clang_format}" --style=file "${f}") "${f}" >/dev/null; then
      echo "not formatted: ${f}"
      failed=1
    fi
  done
  if [[ ${failed} -ne 0 ]]; then
    echo "clang-format check failed; run ./scripts/format.sh to fix" >&2
    exit 1
  fi
  echo "all files are correctly formatted"
else
  "${clang_format}" -i --style=file "${files[@]}"
  echo "done"
fi
