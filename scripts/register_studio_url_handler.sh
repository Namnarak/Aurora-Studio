#!/usr/bin/env bash
# Copyright 2026 Aurora Project Authors
# SPDX-License-Identifier: Apache-2.0

set -Eeuo pipefail
umask 077

readonly DESKTOP_ID="space.bigrat.aurora-studio.desktop"
readonly STUDIO_PLACE_MIME="application/x-roblox-place"
readonly STUDIO_MODEL_MIME="application/x-roblox-model"
readonly STUDIO_SCHEME="x-scheme-handler/roblox-studio"
readonly STUDIO_AUTH_SCHEME="x-scheme-handler/roblox-studio-auth"
readonly STUDIO_AUTH_ROUTER_DESKTOP_ID="space.bigrat.aurora-studio-auth-router.desktop"
readonly LEGACY_SETTINGS_DESKTOP_ID="space.bigrat.aurora-studio-settings.desktop"

Die() {
  printf 'aurora-studio-url-handler: %s\n' "$*" >&2
  exit 1
}

Usage() {
  cat <<'EOF'
Usage: register_studio_url_handler.sh MODE [OPTIONS]

Install a per-user Aurora Studio desktop entry for Roblox Studio links.

Modes:
  --install-only          Install the desktop entry without changing defaults.
  --set-default           Install it and select Aurora Studio for Studio URL
                          schemes and Roblox place/model files.

Options:
  --executable PATH       Aurora Studio ELF or AppImage to launch. By default
                          the script finds the matching build or installed binary.
  --desktop-file PATH     Aurora Studio desktop template.
  --auth-desktop-file PATH
                          Aurora Studio auth-router desktop template.
  -h, --help              Show this help.

Run this command as the desktop user, never through sudo.
EOF
}

RequireValue() {
  local option="$1"
  local value="${2:-}"
  [[ -n "${value}" && "${value}" != --* ]] ||
    Die "${option} requires a value"
}

ResolveRegularFile() {
  local path="$1"
  local label="$2"
  [[ -f "${path}" && -r "${path}" ]] ||
    Die "${label} is not a readable regular file: ${path}"
  realpath -e -- "${path}" 2>/dev/null ||
    Die "cannot resolve ${label}: ${path}"
}

ResolveExecutable() {
  local path="$1"
  [[ -f "${path}" && -x "${path}" ]] ||
    Die "Aurora Studio executable is not a regular executable file: ${path}"
  realpath -e -- "${path}" 2>/dev/null ||
    Die "cannot resolve Aurora Studio executable: ${path}"
}

mode=""
studio_executable=""
desktop_source=""
auth_desktop_source=""
while (( $# > 0 )); do
  case "$1" in
    --install-only|--set-default)
      [[ -z "${mode}" ]] || Die "select exactly one registration mode"
      mode="$1"
      shift
      ;;
    --executable)
      RequireValue "$1" "${2:-}"
      studio_executable="$2"
      shift 2
      ;;
    --desktop-file)
      RequireValue "$1" "${2:-}"
      desktop_source="$2"
      shift 2
      ;;
    --auth-desktop-file)
      RequireValue "$1" "${2:-}"
      auth_desktop_source="$2"
      shift 2
      ;;
    -h|--help)
      Usage
      exit 0
      ;;
    *) Die "unknown option: $1" ;;
  esac
done

[[ -n "${mode}" ]] || Die "select --install-only or --set-default"
(( EUID != 0 )) || Die "run this command as the desktop user, not root"

script_dir="$(cd -P -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
if [[ -z "${studio_executable}" ]]; then
  for candidate in \
      "${script_dir}/aurora_studio" \
      "${script_dir}/../build/aurora_studio"; do
    if [[ -f "${candidate}" && -x "${candidate}" ]]; then
      studio_executable="${candidate}"
      break
    fi
  done
  if [[ -z "${studio_executable}" ]]; then
    studio_executable="$(command -v aurora_studio 2>/dev/null || true)"
  fi
fi
[[ -n "${studio_executable}" ]] ||
  Die "build or install Aurora Studio, or pass --executable PATH"
studio_executable="$(ResolveExecutable "${studio_executable}")"

if [[ -z "${desktop_source}" ]]; then
  for candidate in \
      "${script_dir}/../packaging/${DESKTOP_ID}" \
      "${script_dir}/../share/applications/${DESKTOP_ID}"; do
    if [[ -f "${candidate}" && -r "${candidate}" ]]; then
      desktop_source="${candidate}"
      break
    fi
  done
fi
[[ -n "${desktop_source}" ]] ||
  Die "cannot find the Aurora Studio desktop template; pass --desktop-file PATH"
desktop_source="$(ResolveRegularFile "${desktop_source}" "desktop template")"

if [[ -z "${auth_desktop_source}" ]]; then
  for candidate in \
      "${script_dir}/../packaging/${STUDIO_AUTH_ROUTER_DESKTOP_ID}" \
      "${script_dir}/../share/applications/${STUDIO_AUTH_ROUTER_DESKTOP_ID}" \
      "/usr/local/share/applications/${STUDIO_AUTH_ROUTER_DESKTOP_ID}" \
      "/usr/share/applications/${STUDIO_AUTH_ROUTER_DESKTOP_ID}"; do
    if [[ -f "${candidate}" && -r "${candidate}" ]]; then
      auth_desktop_source="${candidate}"
      break
    fi
  done
fi
[[ -n "${auth_desktop_source}" ]] ||
  Die "cannot find the Aurora Studio auth-router template; pass --auth-desktop-file PATH"
auth_desktop_source="$(ResolveRegularFile "${auth_desktop_source}" "auth-router desktop template")"

[[ "$(grep -Fxc '[Desktop Entry]' "${desktop_source}")" == 1 &&
   "$(grep -Fxc 'Type=Application' "${desktop_source}")" == 1 &&
   "$(grep -Fxc 'Icon=space.bigrat.aurora-studio' "${desktop_source}")" == 1 &&
   "$(grep -Fxc 'Exec=aurora_studio %u' "${desktop_source}")" == 1 &&
   "$(grep -Fxc 'MimeType=application/x-roblox-place;application/x-roblox-model;x-scheme-handler/roblox-studio;' \
       "${desktop_source}")" == 1 &&
   "$(grep -Fxc 'X-Aurora-Managed=true' "${desktop_source}")" == 1 &&
   "$(grep -c '^Exec=' "${desktop_source}")" == 1 ]] ||
  Die "desktop template does not match the Aurora Studio URL-handler contract"

[[ "$(grep -Fxc '[Desktop Entry]' "${auth_desktop_source}")" == 1 &&
   "$(grep -Fxc 'Type=Application' "${auth_desktop_source}")" == 1 &&
   "$(grep -Fxc 'Icon=space.bigrat.aurora-studio' "${auth_desktop_source}")" == 1 &&
   "$(grep -Fxc 'Exec=aurora_studio %u' "${auth_desktop_source}")" == 1 &&
   "$(grep -Fxc 'NoDisplay=true' "${auth_desktop_source}")" == 1 &&
   "$(grep -Fxc 'MimeType=x-scheme-handler/roblox-studio-auth;' \
       "${auth_desktop_source}")" == 1 &&
   "$(grep -Fxc 'X-Aurora-Managed=true' "${auth_desktop_source}")" == 1 &&
   "$(grep -c '^Exec=' "${auth_desktop_source}")" == 1 ]] ||
  Die "auth-router desktop template does not match the Aurora Studio contract"

[[ -n "${HOME:-}" && "${HOME}" == /* ]] ||
  Die "HOME must be an absolute desktop-user path"
data_home="${XDG_DATA_HOME:-${HOME}/.local/share}"
[[ "${data_home}" == /* ]] || Die "XDG_DATA_HOME must be absolute"
applications_dir="${data_home}/applications"
mkdir -p -- "${applications_dir}"
applications_dir="$(realpath -e -- "${applications_dir}")" ||
  Die "cannot resolve the per-user applications directory"
[[ -d "${applications_dir}" && ! -L "${applications_dir}" ]] ||
  Die "per-user applications path is not a regular directory"
[[ "$(stat -c '%u' -- "${applications_dir}")" == "$(id -u)" ]] ||
  Die "per-user applications directory is not owned by the current user"

config_home="${XDG_CONFIG_HOME:-${HOME}/.config}"
[[ "${config_home}" == /* ]] || Die "XDG_CONFIG_HOME must be absolute"
mkdir -p -- "${config_home}"

desktop_target="${applications_dir}/${DESKTOP_ID}"
if [[ -e "${desktop_target}" || -L "${desktop_target}" ]]; then
  [[ -f "${desktop_target}" && ! -L "${desktop_target}" &&
     "$(stat -c '%u' -- "${desktop_target}")" == "$(id -u)" ]] ||
    Die "refusing to replace an unsafe desktop entry: ${desktop_target}"
  grep -Fxq 'X-Aurora-Managed=true' "${desktop_target}" ||
    Die "refusing to replace an unmanaged desktop entry"
fi

temporary="$(mktemp "${applications_dir}/.${DESKTOP_ID}.XXXXXX.desktop")"
trap 'rm -f -- "${temporary}"' EXIT
while IFS= read -r line || [[ -n "${line}" ]]; do
  if [[ "${line}" == 'Exec=aurora_studio %u' ]]; then
    printf 'Exec=%s %%u\n' "${studio_executable}"
  else
    printf '%s\n' "${line}"
  fi
done <"${desktop_source}" >"${temporary}"
chmod 0644 "${temporary}"
if command -v desktop-file-validate >/dev/null 2>&1; then
  desktop-file-validate "${temporary}" ||
    Die "generated desktop entry failed validation"
fi
mv -f -- "${temporary}" "${desktop_target}"
trap - EXIT
grep -Fxq "Exec=${studio_executable} %u" "${desktop_target}" ||
  Die "generated desktop entry has the wrong Studio launcher"

legacy_settings_target="${applications_dir}/${LEGACY_SETTINGS_DESKTOP_ID}"
if [[ -e "${legacy_settings_target}" || -L "${legacy_settings_target}" ]]; then
  [[ -f "${legacy_settings_target}" && ! -L "${legacy_settings_target}" &&
     "$(stat -c '%u' -- "${legacy_settings_target}")" == "$(id -u)" ]] ||
    Die "refusing to remove an unsafe legacy Settings entry: ${legacy_settings_target}"
  grep -Fxq 'X-Aurora-Managed=true' "${legacy_settings_target}" ||
    Die "refusing to remove an unmanaged legacy Settings entry"
  rm -f -- "${legacy_settings_target}"
fi

auth_desktop_target="${applications_dir}/${STUDIO_AUTH_ROUTER_DESKTOP_ID}"
if [[ -e "${auth_desktop_target}" || -L "${auth_desktop_target}" ]]; then
  [[ -f "${auth_desktop_target}" && ! -L "${auth_desktop_target}" &&
     "$(stat -c '%u' -- "${auth_desktop_target}")" == "$(id -u)" ]] ||
    Die "refusing to replace an unsafe auth-router desktop entry: ${auth_desktop_target}"
  grep -Fxq 'X-Aurora-Managed=true' "${auth_desktop_target}" ||
    Die "refusing to replace an unmanaged auth-router desktop entry"
fi

auth_temporary="$(mktemp "${applications_dir}/.${STUDIO_AUTH_ROUTER_DESKTOP_ID}.XXXXXX.desktop")"
trap 'rm -f -- "${auth_temporary}"' EXIT
while IFS= read -r line || [[ -n "${line}" ]]; do
  if [[ "${line}" == 'Exec=aurora_studio %u' ]]; then
    printf 'Exec=%s %%u\n' "${studio_executable}"
  else
    printf '%s\n' "${line}"
  fi
done <"${auth_desktop_source}" >"${auth_temporary}"
chmod 0644 "${auth_temporary}"
if command -v desktop-file-validate >/dev/null 2>&1; then
  desktop-file-validate "${auth_temporary}" ||
    Die "generated auth-router desktop entry failed validation"
fi
mv -f -- "${auth_temporary}" "${auth_desktop_target}"
trap - EXIT
grep -Fxq "Exec=${studio_executable} %u" "${auth_desktop_target}" ||
  Die "generated auth-router desktop entry has the wrong Aurora Studio launcher"

if command -v update-desktop-database >/dev/null 2>&1; then
  update-desktop-database -q "${applications_dir}" ||
    Die "could not refresh the per-user desktop database"
fi

if [[ "${mode}" == --set-default ]]; then
  command -v xdg-mime >/dev/null 2>&1 ||
    Die "xdg-mime is required to select the Studio URL handler"
  for association in "${STUDIO_PLACE_MIME}" "${STUDIO_MODEL_MIME}" \
                     "${STUDIO_SCHEME}"; do
    xdg-mime default "${DESKTOP_ID}" "${association}" ||
      Die "could not select Aurora Studio for ${association}"
    selected="$(xdg-mime query default "${association}" 2>/dev/null || true)"
    [[ "${selected}" == "${DESKTOP_ID}" ]] ||
      Die "desktop environment did not retain Aurora Studio for ${association}"
  done

  xdg-mime default "${STUDIO_AUTH_ROUTER_DESKTOP_ID}" "${STUDIO_AUTH_SCHEME}" ||
    Die "could not select ${STUDIO_AUTH_ROUTER_DESKTOP_ID} for ${STUDIO_AUTH_SCHEME}"
  selected="$(xdg-mime query default "${STUDIO_AUTH_SCHEME}" 2>/dev/null || true)"
  [[ "${selected}" == "${STUDIO_AUTH_ROUTER_DESKTOP_ID}" ]] ||
    Die "desktop environment did not retain ${STUDIO_AUTH_ROUTER_DESKTOP_ID} for ${STUDIO_AUTH_SCHEME}"

  printf 'Aurora Studio handles Studio links; auth callbacks use the Aurora auth router.\n'
else
  printf 'Installed %s and %s without changing URL-handler defaults.\n' \
    "${desktop_target}" "${auth_desktop_target}"
fi
