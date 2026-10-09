#!/usr/bin/env bash
set -Eeuo pipefail
cd "$(dirname -- "${BASH_SOURCE[0]}")/.."
python3 -m json.tool packaging/flatpak/space.bigrat.aurora-studio.json >/dev/null
bash tests/studio_product_contract_test.sh .
for p in aurora-studio aurora-studio-git;do
 bash -n "packaging/aur/$p/PKGBUILD"
 test -s "packaging/aur/$p/.SRCINFO"
done
bash -n scripts/package_studio_appimage.sh
echo Studio-distribution-contract-passed
