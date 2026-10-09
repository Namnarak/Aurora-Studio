<!-- CYTECH_README_REFRESH:START -->
<div align="center">

<a href="https://github.com/Namnarak/Aurora-Studio"><img width="100%" alt="Aurora Studio banner" src="https://capsule-render.vercel.app/api?type=waving&color=0:19112C,100:A855F7&height=210&section=header&text=Aurora%20Studio&fontSize=43&fontColor=ffffff&fontAlignY=36&desc=Roblox%20Studio%20on%20Linux%2C%20powered%20by%20Wine&descAlignY=59&descSize=16"></a>

<img alt="PROJECT: Developer Tools" src="https://img.shields.io/badge/PROJECT-Developer%20Tools-A855F7?style=flat-square&labelColor=19112C&color=A855F7"> <img alt="STACK: Wine · C++17" src="https://img.shields.io/badge/STACK-Wine%20%C2%B7%20C%2B%2B17-A855F7?style=flat-square&labelColor=19112C&color=A855F7">

<p><strong>Roblox Studio on Linux, powered by Wine</strong></p>

<a href="https://github.com/Namnarak/Aurora-Studio/releases">Releases</a> · <a href="https://github.com/Namnarak/Aurora-Studio/issues">Issues</a> · <a href="https://github.com/Namnarak/Aurora-Studio">Source</a>

</div>

<!-- CYTECH_README_REFRESH:END -->

---

[![CI](https://github.com/Namnarak/Aurora-Studio/actions/workflows/ci.yml/badge.svg)](https://github.com/Namnarak/Aurora-Studio/actions/workflows/ci.yml)
[![License: GPL-3.0-only](https://img.shields.io/badge/license-GPL--3.0--only-blue.svg)](LICENSE)

Aurora Studio launches Roblox Studio on Linux through Wine. It provisions its
own Studio runtime from Roblox's official Windows Studio deployment feed,
verifies packages against the feed manifest, and keeps the Wine prefix and
Studio runtime separate from other launchers. The Windows Studio payload is
not included in this source repository.

Aurora Studio is an independent community project. It is not affiliated with
Roblox Corporation and does not distribute Roblox Studio.

## Packages

[Stable/Nightly downloads](https://github.com/Namnarak/Aurora-Studio/releases) | [Packaging details](docs/DISTRIBUTION.md)

## Build

The build uses CMake, Ninja, a C++17 compiler, pkg-config, GTK 4, libadwaita,
Vulkan headers, curl, OpenSSL, nlohmann-json, libyaml, minizip, and capstone.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build --target aurora_studio aurora_studio_runtime \
  aurora_studio_mcp aurora_studio_control_mcp --parallel
cmake --install build --prefix /usr/local
```

To build and run tests, configure with `-DBUILD_TESTING=ON`, build the default
targets, then run `ctest --test-dir build --output-on-failure`.

## Studio MCP tools

Aurora Studio provides two MCP entry points for distinct purposes:

- `aurora_studio_mcp` launches Roblox's Studio MCP payload through the managed
  Wine runtime.
- `aurora_studio_control_mcp` provides Aurora process, launch, client-status,
  and log tools. Its Player launch tool can target a separately installed
  Aurora Player with `AURORA_PLAYER_PATH`.

Example configuration for the Studio payload bridge:

```json
{
  "mcpServers": {
    "Aurora_Roblox_Studio": {
      "command": "aurora_studio_mcp"
    }
  }
}
```

## License scope

Aurora Studio's product license is GPL-3.0-only. Files that retain
Apache-2.0 are enumerated path by path in
[`LICENSES/APACHE-FILES.txt`](LICENSES/APACHE-FILES.txt), with source revision
and candidate SHA-256 for each entry. Third-party components retain their own
licenses and notices. Packages install all license texts and notices under
`/usr/share/doc/aurora-studio/`.

See [LICENSE](LICENSE), [LICENSE-APACHE](LICENSE-APACHE),
[LICENSES/PROVENANCE.md](LICENSES/PROVENANCE.md), and
[LICENSES/DISTRIBUTION-LICENSES.md](LICENSES/DISTRIBUTION-LICENSES.md) for
the license boundaries and source history. The exact Vinegar source revision
could not be verified from the pre-split checkout.

Aurora Player is maintained separately in
[Namnarak/Aurora-Player](https://github.com/Namnarak/Aurora-Player).

Read the [Terms of Use](TERMS.md) and [Privacy Policy](PRIVACY.md) before use.
