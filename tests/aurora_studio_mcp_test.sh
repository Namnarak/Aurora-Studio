#!/usr/bin/env bash
set -euo pipefail

binary=${1:?aurora_studio_mcp binary is required}
test_root=$(mktemp -d "${TMPDIR:-/tmp}/aurora_studio_mcp.XXXXXX")
trap 'rm -rf "$test_root"' EXIT

export HOME="$test_root/home"
export XDG_CONFIG_HOME="$test_root/config"
mkdir -p "$HOME" "$XDG_CONFIG_HOME"

wine="$test_root/fake-wine"
studio_mcp="$test_root/StudioMCP.exe"
log="$test_root/exec.log"
touch "$studio_mcp"
cat >"$wine" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
{
  printf 'prefix=%s\n' "${WINEPREFIX:-}"
  printf 'debug=%s\n' "${WINEDEBUG:-}"
  printf 'loader=%s\n' "${WINELOADERNOEXEC:-}"
  printf 'argc=%s\n' "$#"
  printf 'arg='; printf '%s\n' "$1"
  printf 'arg='; printf '%s\n' "$2"
} >"${AURORA_TEST_LOG}"
EOF
chmod 0755 "$wine"

check_output=$(
  AURORA_ROBLOX_WINE="$wine" \
  AURORA_ROBLOX_STUDIO_MCP="$studio_mcp" \
  "$binary" --check
)
grep -Fq "wine=$wine" <<<"$check_output"
grep -Fq "StudioMCP=$studio_mcp" <<<"$check_output"

config_output=$(
  AURORA_ROBLOX_WINE="$wine" \
  AURORA_ROBLOX_STUDIO_MCP="$studio_mcp" \
  "$binary" --print-config
)
grep -Fq 'Aurora_Roblox_Studio' <<<"$config_output"
grep -Fq 'aurora_studio_mcp' <<<"$config_output"

AURORA_ROBLOX_WINE="$wine" \
AURORA_ROBLOX_STUDIO_MCP="$studio_mcp" \
AURORA_ROBLOX_WINEPREFIX="$test_root/prefix" \
AURORA_TEST_LOG="$log" \
  "$binary" --probe

grep -Fq "prefix=$test_root/prefix" "$log"
grep -Fq 'debug=-all' "$log"
grep -Fq 'loader=1' "$log"
grep -Fq 'arg='"$studio_mcp" "$log"
grep -Fq 'arg=--probe' "$log"

default_log="$test_root/default-exec.log"
HOME="$test_root/home" \
XDG_DATA_HOME="$test_root/data" \
AURORA_ROBLOX_WINE="$wine" \
AURORA_ROBLOX_STUDIO_MCP="$studio_mcp" \
AURORA_TEST_LOG="$default_log" \
  "$binary" --probe

grep -Fq "prefix=$test_root/data/aurora/studio-prefix" "$default_log"
grep -Fq 'debug=-all' "$default_log"
grep -Fq 'loader=1' "$default_log"

proton="$test_root/fake-proton"
proton_log="$test_root/proton-exec.log"
mkdir -p "$test_root/empty-path"
cat >"$proton" <<'EOF'
#!/bin/bash
set -euo pipefail
{
  printf 'runtime=proton\n'
  printf 'compat=%s\n' "${STEAM_COMPAT_DATA_PATH:-}"
  printf 'prefix=%s\n' "${WINEPREFIX:-}"
  printf 'arg1=%s\n' "${1:-}"
  printf 'arg2=%s\n' "${2:-}"
  printf 'arg3=%s\n' "${3:-}"
} >"${AURORA_TEST_LOG}"
EOF
chmod 0755 "$proton"

proton_check=$(
  HOME="$test_root/home" \
  XDG_DATA_HOME="$test_root/data" \
  PATH="$test_root/empty-path" \
  AURORA_STUDIO_PROTON="$proton" \
  AURORA_ROBLOX_STUDIO_MCP="$studio_mcp" \
    "$binary" --check
)
grep -Fq 'runtime=proton' <<<"$proton_check"
grep -Fq "proton=$proton" <<<"$proton_check"

mkdir -p "$test_root/empty-path"
HOME="$test_root/home" \
XDG_DATA_HOME="$test_root/data" \
PATH="$test_root/empty-path" \
AURORA_STUDIO_PROTON="$proton" \
AURORA_ROBLOX_STUDIO_MCP="$studio_mcp" \
AURORA_TEST_LOG="$proton_log" \
  "$binary" --probe

grep -Fq 'runtime=proton' "$proton_log"
grep -Fq "compat=$test_root/data/aurora/studio-proton-compatdata" "$proton_log"
grep -Fq "prefix=$test_root/data/aurora/studio-proton-compatdata/pfx" "$proton_log"
grep -Fq 'arg1=runinprefix' "$proton_log"
grep -Fq "arg2=$studio_mcp" "$proton_log"
grep -Fq 'arg3=--probe' "$proton_log"
