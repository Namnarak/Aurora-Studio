#!/usr/bin/env bash
# Copyright 2026 NamKrub
# SPDX-License-Identifier: GPL-3.0-only

set -Eeuo pipefail
readonly ROOT="$(cd -- "${1:?source root is required}" && pwd)"
fail() { printf 'Studio product contract failed: %s\n' "$*" >&2; exit 1; }

grep -Fq 'project(AuroraStudio' "${ROOT}/CMakeLists.txt" || fail 'CMake project identity is wrong'
grep -Fq 'set(AURORA_PACKAGE_NAME "aurora-studio"' "${ROOT}/CMakeLists.txt" || fail 'stable package name is wrong'
grep -Fq 'set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/Namnarak/Aurora-Studio")' "${ROOT}/CMakeLists.txt" || fail 'CPack homepage is wrong'
grep -Fq 'set(CPACK_RPM_PACKAGE_LICENSE "GPL-3.0-only AND Apache-2.0 AND MIT AND BSD-2-Clause AND BSD-2-Clause-FreeBSD AND BSD-3-Clause AND BSD-4-Clause AND GPL-2.0-only WITH Classpath-exception-2.0")' "${ROOT}/CMakeLists.txt" || fail 'aggregate RPM license metadata is incomplete'
grep -Fq 'License: GPL-3.0-only AND Apache-2.0.' "${ROOT}/CMakeLists.txt" || fail 'package description omits one of the licenses'
grep -Fq '<project_license>GPL-3.0-only AND Apache-2.0</project_license>' "${ROOT}/packaging/space.bigrat.aurora-studio.metainfo.xml" || fail 'desktop metadata license is wrong'
grep -Fq 'https://github.com/Namnarak/Aurora-Studio' "${ROOT}/packaging/space.bigrat.aurora-studio.metainfo.xml" || fail 'desktop metadata homepage is wrong'
grep -Fq 'LICENSE-APACHE' "${ROOT}/CMakeLists.txt" || fail 'Apache shared-code license is not installed'
test -f "${ROOT}/LICENSE" || fail 'GPL license is missing'
test -f "${ROOT}/LICENSE-APACHE" || fail 'Apache license is missing'
test -f "${ROOT}/LICENSES/PROVENANCE.md" || fail 'license provenance document is missing'
test -f "${ROOT}/LICENSES/APACHE-FILES.txt" || fail 'Apache file inventory is missing'
test -f "${ROOT}/LICENSES/DISTRIBUTION-LICENSES.md" || fail 'distribution license summary is missing'
test -s "${ROOT}/third_party/jni/ASSEMBLY_EXCEPTION" || fail 'JNI Assembly Exception source notice is missing'
grep -Fq 'set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSES/DISTRIBUTION-LICENSES.md")' "${ROOT}/CMakeLists.txt" || fail 'CPack license summary is wrong'
test ! -e "${ROOT}/packaging/flatpak" || fail 'Player Flatpak metadata remains in Studio'
test ! -e "${ROOT}/packaging/aur" || fail 'Player AUR metadata remains in Studio'
test ! -e "${ROOT}/flake.nix" || fail 'Player Nix packaging remains in Studio'
test ! -e "${ROOT}/src/player/aurora_player.cc" || fail 'Player launcher source remains in Studio'
test ! -e "${ROOT}/src/update/apkcombo_provider.cc" || fail 'Player APKCombo source remains in Studio'
test ! -e "${ROOT}/include/update/apkcombo_provider.h" || fail 'Player APKCombo header remains in Studio'
grep -Fq 'AURORA_ENABLE_APKCOMBO_PROVIDER' "${ROOT}/src/update/update_coordinator.cc" || fail 'provider build guard is missing'
grep -Fq 'space.bigrat.aurora-studio' "${ROOT}/include/aurora/platform/sdl_application_metadata.h" || fail 'Studio SDL application identity is wrong'
grep -Fq 'Aurora Studio' "${ROOT}/include/aurora/platform/sdl_application_metadata.h" || fail 'Studio SDL application name is wrong'
grep -Fq 'gtk_window_set_default_icon_name("space.bigrat.aurora-studio")' "${ROOT}/src/ui/aurora_failure_dialog.cc" || fail 'failure dialog icon is not Studio-specific'
grep -Fq 'g_set_application_name("Aurora Studio")' "${ROOT}/src/ui/aurora_failure_dialog.cc" || fail 'failure dialog app name is not Studio-specific'
if grep -Fq 'submodules:' "${ROOT}/.github/workflows/ci.yml"; then
  fail 'flattened CI checkout still requests submodules'
fi
test ! -e "${ROOT}/tests/system_install_test.sh" || fail 'combined product install test remains'
test ! -d "${ROOT}/patches" || [[ -z "$(find "${ROOT}/patches" -type f -print -quit)" ]] || fail 'unlicensed local patch stack remains'
if find "${ROOT}/.github/workflows" -maxdepth 1 -type f ! -name ci.yml -print -quit | grep -q .; then
  fail 'non-CI workflow remains in the publication candidate'
fi
printf 'Aurora Studio product contract passed\n'
