// SPDX-License-Identifier: GPL-3.0-only

#ifndef AURORA_STUDIO_KOMBUCHA_PROVISIONER_H_
#define AURORA_STUDIO_KOMBUCHA_PROVISIONER_H_

#include <filesystem>
#include <string>

#include "studio/studio_launch_settings.h"

namespace aurora::studio {

struct CompatibilityProvisionResult {
  std::string kombucha_tag;
  std::string dxvk_version;
  std::filesystem::path wine_executable;
  std::filesystem::path dxvk_x64_directory;
  bool changed = false;
  std::string error;

  explicit operator bool() const { return error.empty(); }
};

// Managed compatibility components live below the Studio runtime root so
// Studio remains self-contained and never needs Vinegar's data directory.
std::filesystem::path DefaultManagedKombuchaRoot();
std::filesystem::path DefaultManagedKombuchaWine();
std::filesystem::path DefaultManagedDxvkRoot();
std::filesystem::path DefaultManagedDxvkLegacyRoot();

// Fetch/update Aurora's Kombucha fork and the pinned, verified DXVK runtime.
// Network access is only required by this explicit update path.
CompatibilityProvisionResult UpdateManagedStudioCompatibility(
    bool force = false, int progress_fd = -1);

// Ensure the managed runtime exists and place DXVK beside the selected Studio
// executable. If the runtime is missing, this performs the first-run update.
CompatibilityProvisionResult EnsureManagedStudioCompatibility(
    const std::filesystem::path& studio_executable, int progress_fd = -1);

// Ensure the managed runtime and the selected renderer's DLLs are ready.
CompatibilityProvisionResult EnsureManagedStudioCompatibilityForMode(
    const std::filesystem::path& studio_executable,
    StudioGraphicsMode graphics_mode, int progress_fd = -1);

// Copy the already provisioned DXVK x64 D3D11/DXGI pair beside Studio. This is
// cheap on repeat launches because a version marker avoids redundant copies.
bool SyncManagedDxvkToStudio(
    const std::filesystem::path& studio_executable,
    bool* changed = nullptr, std::string* error = nullptr);

}  // namespace aurora::studio

#endif  // AURORA_STUDIO_KOMBUCHA_PROVISIONER_H_
