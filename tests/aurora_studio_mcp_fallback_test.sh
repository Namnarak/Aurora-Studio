#!/usr/bin/env bash
set -Eeuo pipefail

launcher=${1:?roblox_studio_mcp script is required}
test_root="$(mktemp -d "${TMPDIR:-/tmp}/aurora_studio_mcp_fallback.XXXXXX")"
trap 'rm -rf -- "$test_root"' EXIT

mkdir -p "$test_root/home" "$test_root/data/aurora/studio-runtime/wine/bin" \
         "$test_root/data/aurora/studio-runtime/versions/version-1" \
         "$test_root/scripts"

cp -- "$launcher" "$test_root/scripts/roblox_studio_mcp"
chmod 0755 "$test_root/scripts/roblox_studio_mcp"

wine="$test_root/data/aurora/studio-runtime/wine/bin/wine"
studio_mcp="$test_root/data/aurora/studio-runtime/versions/version-1/StudioMCP.exe"
log="$test_root/fallback.log"
touch "$studio_mcp"

cat >"$wine" <<'EOF'
#!/usr/bin/env bash
set -Eeuo pipefail
{
  printf 'prefix=%s\n' "${WINEPREFIX:-}"
  printf 'arg1=%s\n' "${1:-}"
  printf 'arg2=%s\n' "${2:-}"
} >"${AURORA_TEST_LOG:?}"
EOF
chmod 0755 "$wine"

HOME="$test_root/home" \
XDG_DATA_HOME="$test_root/data" \
AURORA_STUDIO_MCP_LAUNCHER="$test_root/does-not-exist" \
AURORA_TEST_LOG="$log" \
  "$test_root/scripts/roblox_studio_mcp" --probe

grep -Fxq "prefix=$test_root/data/aurora/studio-prefix" "$log"
grep -Fxq "arg1=$studio_mcp" "$log"
grep -Fxq 'arg2=--probe' "$log"

if grep -Eqi 'vinegar|org\.vinegarhq' "$test_root/scripts/roblox_studio_mcp"; then
  echo 'fallback Studio MCP launcher still contains Vinegar runtime coupling' >&2
  exit 1
fi

printf 'Aurora Studio MCP fallback runtime ownership checks passed\n'
