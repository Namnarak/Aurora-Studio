#!/usr/bin/env bash
# Copyright 2026 NamKrub
# SPDX-License-Identifier: GPL-3.0-only

set -Eeuo pipefail
readonly source_root="$(cd -- "${1:?source root required}" && pwd)"
readonly build_root="$(cd -- "${2:?build root required}" && pwd)"
fail() { printf 'Studio APK provider contract failed: %s\n' "$*" >&2; exit 1; }

[[ ! -e "${source_root}/src/update/apkcombo_provider.cc" ]] || fail 'Player APKCombo implementation is present'
[[ ! -e "${source_root}/include/update/apkcombo_provider.h" ]] || fail 'Player APKCombo header is present'
grep -Fq 'if(PROJECT_NAME STREQUAL "AuroraPlayer")' "${source_root}/CMakeLists.txt" || fail 'provider target is not product-gated'
grep -Fq 'AURORA_ENABLE_APKCOMBO_PROVIDER' "${source_root}/CMakeLists.txt" || fail 'provider compile definition is not product-gated'
if grep -Fq 'apkcombo_provider.cc' "${build_root}/build.ninja"; then
  fail 'Studio Ninja graph contains the Player APKCombo implementation'
fi
printf 'Studio APK provider contract passed\n'
