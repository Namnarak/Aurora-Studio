// SPDX-License-Identifier: GPL-3.0-only

#include "studio_runtime.h"

#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aurora {
namespace studio {
namespace {

using Path = std::filesystem::path;

Path HomePath() {
  const char* home = std::getenv("HOME");
  return home == nullptr || home[0] == '\0' ? Path(".") : Path(home);
}

std::optional<std::string> EnvironmentValue(const char* name) {
  const char* value = std::getenv(name);
  if (value == nullptr || value[0] == '\0') return std::nullopt;
  return std::string(value);
}

bool IsExecutable(const Path& path) {
  return !path.empty() && access(path.c_str(), X_OK) == 0;
}

std::optional<Path> FindOnPath(const std::string& name) {
  const char* configured_path = std::getenv("PATH");
  const std::string search_path =
      configured_path == nullptr ? "/usr/local/bin:/usr/bin:/bin"
                                 : std::string(configured_path);
  std::size_t start = 0;
  while (start <= search_path.size()) {
    const std::size_t separator = search_path.find(':', start);
    const std::string directory =
        separator == std::string::npos
            ? search_path.substr(start)
            : search_path.substr(start, separator - start);
    const Path candidate = Path(directory.empty() ? "." : directory) / name;
    if (IsExecutable(candidate)) return candidate;
    if (separator == std::string::npos) break;
    start = separator + 1;
  }
  return std::nullopt;
}

void AddCandidate(std::vector<Path>* candidates, const Path& path) {
  if (candidates == nullptr || path.empty()) return;
  if (std::find(candidates->begin(), candidates->end(), path) ==
      candidates->end()) {
    candidates->push_back(path);
  }
}

void AddCompatibilityTools(const Path& root, std::vector<Path>* candidates) {
  if (candidates == nullptr) return;
  std::error_code error;
  if (!std::filesystem::is_directory(root, error) || error) return;

  std::vector<Path> discovered;
  for (std::filesystem::directory_iterator iterator(
           root, std::filesystem::directory_options::skip_permission_denied,
           error),
       end;
       iterator != end && !error; iterator.increment(error)) {
    const Path candidate = iterator->path() / "proton";
    if (IsExecutable(candidate)) discovered.push_back(candidate);
  }
  std::sort(discovered.begin(), discovered.end());
  for (const Path& candidate : discovered) AddCandidate(candidates, candidate);
}

}  // namespace

std::optional<std::filesystem::path> DetectWine() {
  for (const char* name : {"AURORA_ROBLOX_WINE", "AURORA_WINE"}) {
    if (const auto configured = EnvironmentValue(name); configured.has_value()) {
      const Path path(*configured);
      if (path.has_parent_path()) {
        if (IsExecutable(path)) return path;
      } else if (const auto on_path = FindOnPath(*configured);
                 on_path.has_value()) {
        return on_path;
      }
    }
  }
  return FindOnPath("wine");
}

std::optional<std::filesystem::path> DetectProton() {
  for (const char* name : {"AURORA_STUDIO_PROTON", "AURORA_PROTON"}) {
    if (const auto configured = EnvironmentValue(name); configured.has_value()) {
      const Path path(*configured);
      if (path.has_parent_path()) {
        if (IsExecutable(path)) return path;
      } else if (const auto on_path = FindOnPath(*configured);
                 on_path.has_value()) {
        return on_path;
      }
    }
  }

  std::vector<Path> candidates;
  // Prefer the Proton tool explicitly installed by the local Steam setup.
  AddCandidate(&candidates,
               Path("/usr/share/steam/compatibilitytools.d/proton-cachyos-slr/proton"));

  const Path home = HomePath();
  const std::vector<Path> compatibility_roots = {
      Path("/usr/share/steam/compatibilitytools.d"),
      home / ".local/share/Steam/compatibilitytools.d",
      home / ".steam/root/compatibilitytools.d",
      home / ".steam/steam/compatibilitytools.d",
      home / ".var/app/com.valvesoftware.Steam/data/Steam/compatibilitytools.d",
  };
  for (const Path& root : compatibility_roots) {
    AddCompatibilityTools(root, &candidates);
  }
  for (const Path& candidate : candidates) {
    if (IsExecutable(candidate)) return candidate;
  }
  return std::nullopt;
}

std::filesystem::path DefaultProtonCompatData() {
  const char* configured = std::getenv("XDG_DATA_HOME");
  const Path root = configured != nullptr && configured[0] == '/'
                        ? Path(configured)
                        : HomePath() / ".local/share";
  return root / "aurora/studio-proton-compatdata";
}

std::filesystem::path ProtonClientInstallPath(
    const std::filesystem::path& proton) {
  Path current = proton.parent_path();
  while (!current.empty() && current != current.parent_path()) {
    if (current.filename() == "compatibilitytools.d") {
      return current.parent_path();
    }
    current = current.parent_path();
  }
  // Keep a useful fallback for custom Proton layouts that do not use Steam's
  // conventional compatibilitytools.d directory name.
  const Path fallback = proton.parent_path().parent_path().parent_path();
  return fallback.empty() ? Path("/usr/share/steam") : fallback;
}

}  // namespace studio
}  // namespace aurora
