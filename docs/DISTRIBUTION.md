# Aurora Studio Distribution

Builds are published at https://github.com/Namnarak/Aurora-Studio/releases.

- DEB: Ubuntu 26.04
- RPM: Fedora 44
- AppImage: thin, requires compatible host GTK/SDL3 libraries and Wine/Proton.
- Flatpak: experimental; host Wine integration has NOT been verified.
- Arch AUR source recipes are in packaging/aur. They are NOT published
  to aur.archlinux.org until the maintainer submits them.

GitHub Releases are distinct from Flathub, signed APT/DNF repositories and AUR.
The project never includes Roblox Studio proprietary files. It downloads them
at runtime via the official Studio deployment feed.
