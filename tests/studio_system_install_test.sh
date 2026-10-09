#!/usr/bin/env bash
# Copyright 2026 Aurora Project Authors
# SPDX-License-Identifier: GPL-3.0-only

set -Eeuo pipefail

readonly build_dir="${1:?build directory is required}"
readonly source_dir="${2:?source directory is required}"
readonly install_root="$(mktemp -d)"
trap 'rm -rf -- "${install_root}"' EXIT

DESTDIR="${install_root}" cmake --install "${build_dir}" --prefix /usr \
  >/dev/null

readonly studio_binary="${install_root}/usr/bin/aurora_studio"
readonly control_mcp="${install_root}/usr/bin/aurora_studio_control_mcp"
readonly studio_mcp="${install_root}/usr/bin/aurora_studio_mcp"
readonly runtime_binary="${install_root}/usr/bin/aurora_studio_runtime"
readonly doc="${install_root}/usr/share/doc/aurora-studio"
readonly data="${install_root}/usr/share/aurora"
readonly desktop="${install_root}/usr/share/applications/space.bigrat.aurora-studio.desktop"
readonly auth_desktop="${install_root}/usr/share/applications/space.bigrat.aurora-studio-auth-router.desktop"
readonly metainfo="${install_root}/usr/share/metainfo/space.bigrat.aurora-studio.metainfo.xml"

[[ -x "${studio_binary}" && -x "${control_mcp}" && -x "${studio_mcp}" && -x "${runtime_binary}" ]]
[[ ! -e "${install_root}/usr/bin/aurora" ]]
[[ ! -e "${install_root}/usr/bin/aurora_mcp" ]]
[[ ! -e "${install_root}/usr/bin/aurora_updater" ]]
[[ -f "${data}/metadata/roblox_compatibility.json" ]]
[[ -f "${data}/metadata/roblox_signing_certificates.json" ]]
[[ -s "${doc}/LICENSE" && -s "${doc}/LICENSE-APACHE" ]]
[[ -s "${doc}/TERMS.md" && -s "${doc}/PRIVACY.md" && -s "${doc}/PROVENANCE.md" ]]
[[ -s "${doc}/APACHE-FILES.txt" && -s "${doc}/DISTRIBUTION-LICENSES.md" ]]
bash "${source_dir}/tests/third_party_notices_contract.sh" \
  "${source_dir}/third_party" "${doc}/third-party"

readelf -h "${studio_binary}" | grep -Fq 'ELF64'
readelf -h "${studio_mcp}" | grep -Fq 'ELF64'
readelf -h "${runtime_binary}" | grep -Fq 'ELF64'
runtime_usage="$("${runtime_binary}" 2>&1 || true)"
grep -Fq 'Usage: aurora_studio_runtime' <<<"${runtime_usage}"

[[ -f "${desktop}" ]]
grep -Fq 'Name=Aurora Studio' "${desktop}"
grep -Fxq 'Icon=space.bigrat.aurora-studio' "${desktop}"
grep -Fxq 'Exec=aurora_studio %u' "${desktop}"
grep -Fxq 'Exec=aurora_studio --settings' "${desktop}"
[[ -f "${auth_desktop}" ]]
grep -Fq 'Name=Aurora Studio Sign-In Router' "${auth_desktop}"
[[ -f "${metainfo}" ]]
grep -Fq '<id>space.bigrat.aurora-studio</id>' "${metainfo}"
[[ -f "${install_root}/usr/share/icons/hicolor/128x128/apps/space.bigrat.aurora-studio.png" ]]

if find "${install_root}/usr" -type f \( -name '*.py' -o -name '*.sh' \) \
    -print -quit | grep -q .; then
  echo 'system install contains a first-party Python or shell runtime' >&2
  exit 1
fi

printf 'Studio system install layout test passed\n'
