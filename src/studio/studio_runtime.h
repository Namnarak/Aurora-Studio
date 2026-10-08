// SPDX-License-Identifier: GPL-3.0-only

#ifndef AURORA_STUDIO_RUNTIME_H_
#define AURORA_STUDIO_RUNTIME_H_

#include <filesystem>
#include <optional>

namespace aurora {
namespace studio {

// Locate Wine from Aurora-specific overrides or PATH.
std::optional<std::filesystem::path> DetectWine();

// Locate a Proton compatibility tool without making Proton a requirement for
// the native Wine workflow. The result is intentionally optional: Aurora
// Studio must remain usable on machines that only have Wine installed.
std::optional<std::filesystem::path> DetectProton();

// Aurora keeps Proton's compatdata root separate from the classic Wine
// prefix. Proton itself creates/uses <root>/pfx.
std::filesystem::path DefaultProtonCompatData();

// Proton's prefix setup needs the Steam installation root even when Studio is
// not launched as a Steam game.
std::filesystem::path ProtonClientInstallPath(
    const std::filesystem::path& proton);

}  // namespace studio
}  // namespace aurora

#endif  // AURORA_STUDIO_RUNTIME_H_
