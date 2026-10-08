// SPDX-License-Identifier: GPL-3.0-only

#ifndef AURORA_STUDIO_WINE_THEME_H_
#define AURORA_STUDIO_WINE_THEME_H_

#include <filesystem>
#include <string>
#include <vector>

namespace aurora {
namespace studio {

struct HostThemeResolution {
  bool dark = false;
  std::string source = "fallback-light";
};

// Resolve the host's current color scheme. Explicit AURORA_THEME or
// AURORA_WINE_THEME values win; otherwise KDE, GTK, and GSettings are
// consulted in that order.
HostThemeResolution ResolveHostTheme();

// Detect the host desktop color scheme and persist the matching Windows/Wine
// theme in the isolated Aurora Studio prefix. The operation is intentionally
// performed before Studio starts so every new Wine process sees one coherent
// theme from its first window.
bool SyncWineTheme(const std::filesystem::path& wine,
                   const std::filesystem::path& prefix,
                   std::string* description,
                   std::string* error);

// Variant used by compatibility wrappers such as Proton. runner_arguments
// are placed before the registry command (for example, Proton's
// "runinprefix" verb), and environment contains the complete runtime
// environment for that wrapper.
bool SyncWineThemeWithRuntime(
    const std::filesystem::path& runner,
    const std::vector<std::string>& runner_arguments,
    const std::filesystem::path& prefix,
    const std::vector<std::string>& environment,
    std::string* description,
    std::string* error);

}  // namespace studio
}  // namespace aurora

#endif  // AURORA_STUDIO_WINE_THEME_H_
