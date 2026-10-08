#!/usr/bin/env bash
set -Eeuo pipefail

registration_script=${1:?registration script is required}
desktop_source=${2:?Studio desktop source is required}
auth_desktop_source=${3:?Studio auth-router desktop source is required}

test_root="$(mktemp -d "${TMPDIR:-/tmp}/aurora_studio_url_handler.XXXXXX")"
trap 'rm -rf -- "$test_root"' EXIT

mkdir -p "$test_root/bin" "$test_root/home" "$test_root/data"
studio_binary="$test_root/aurora_studio"
printf '#!/usr/bin/env bash\nexit 0\n' >"$studio_binary"
chmod 0755 "$studio_binary"
mkdir -p "$test_root/data/applications"
cat >"$test_root/data/applications/space.bigrat.aurora-studio-settings.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=Aurora Studio Settings
Exec=aurora_studio --settings
X-Aurora-Managed=true
EOF

cat >"$test_root/bin/update-desktop-database" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "$*" >>"${DESKTOP_DATABASE_CALLS:?}"
EOF
chmod 0755 "$test_root/bin/update-desktop-database"

cat >"$test_root/bin/desktop-file-validate" <<'EOF'
#!/usr/bin/env bash
grep -Fxq 'Type=Application' "$1"
grep -Fq 'X-Aurora-Managed=true' "$1"
EOF
chmod 0755 "$test_root/bin/desktop-file-validate"

cat >"$test_root/bin/xdg-mime" <<'EOF'
#!/usr/bin/env bash
case "${1:-}" in
  default)
    printf '%s|%s\n' "$2" "$3" >>"${XDG_MIME_CALLS:?}"
    ;;
  query)
    [[ "${2:-}" == default ]] || exit 2
    case "${3:-}" in
      x-scheme-handler/roblox-studio-auth)
        printf '%s\n' space.bigrat.aurora-studio-auth-router.desktop
        ;;
      *)
        printf '%s\n' space.bigrat.aurora-studio.desktop
        ;;
    esac
    ;;
  *) exit 2 ;;
esac
EOF
chmod 0755 "$test_root/bin/xdg-mime"

export HOME="$test_root/home"
export XDG_DATA_HOME="$test_root/data"
export DESKTOP_DATABASE_CALLS="$test_root/desktop-database-calls"
export XDG_MIME_CALLS="$test_root/xdg-mime-calls"
export PATH="$test_root/bin:$PATH"

"$registration_script" --set-default \
  --executable "$studio_binary" \
  --desktop-file "$desktop_source" \
  --auth-desktop-file "$auth_desktop_source" >/dev/null

applications="$XDG_DATA_HOME/applications"
studio_desktop="$applications/space.bigrat.aurora-studio.desktop"
auth_desktop="$applications/space.bigrat.aurora-studio-auth-router.desktop"

[[ -f "$studio_desktop" && ! -L "$studio_desktop" ]]
[[ -f "$auth_desktop" && ! -L "$auth_desktop" ]]
grep -Fxq "Exec=$studio_binary --settings %u" "$studio_desktop"
[[ ! -e "$applications/space.bigrat.aurora-studio-settings.desktop" ]]
grep -Fxq 'MimeType=application/x-roblox-place;application/x-roblox-model;x-scheme-handler/roblox-studio;' "$studio_desktop"
! grep -Fq 'x-scheme-handler/roblox-studio-auth' "$studio_desktop"

grep -Fxq "Exec=$studio_binary %u" "$auth_desktop"
grep -Fxq 'NoDisplay=true' "$auth_desktop"
grep -Fxq 'MimeType=x-scheme-handler/roblox-studio-auth;' "$auth_desktop"

grep -Fxq 'space.bigrat.aurora-studio.desktop|application/x-roblox-place' "$XDG_MIME_CALLS"
grep -Fxq 'space.bigrat.aurora-studio.desktop|application/x-roblox-model' "$XDG_MIME_CALLS"
grep -Fxq 'space.bigrat.aurora-studio.desktop|x-scheme-handler/roblox-studio' "$XDG_MIME_CALLS"
grep -Fxq 'space.bigrat.aurora-studio-auth-router.desktop|x-scheme-handler/roblox-studio-auth' "$XDG_MIME_CALLS"

before="$(wc -l <"$XDG_MIME_CALLS")"
"$registration_script" --install-only \
  --executable "$studio_binary" \
  --desktop-file "$desktop_source" \
  --auth-desktop-file "$auth_desktop_source" >/dev/null
[[ "$(wc -l <"$XDG_MIME_CALLS")" == "$before" ]]

printf 'Aurora Studio URL/auth handler registration checks passed\n'
