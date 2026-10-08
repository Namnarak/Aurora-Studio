// SPDX-License-Identifier: GPL-3.0-only

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

#include "studio/kombucha_provisioner.h"
#include "studio/studio_provisioner.h"

namespace {

void Usage() {
  std::cerr
      << "Usage: aurora_studio_runtime <status|check|install|compat-status|compat-install> [options]\n"
      << "Options:\n"
      << "  --root PATH   Override Aurora Studio runtime root\n"
      << "  --force       Reinstall the current Roblox Studio version\n";
}

std::string ReadFirstLine(const std::filesystem::path& path) {
  std::ifstream input(path);
  std::string value;
  std::getline(input, value);
  return value;
}

int Status(const std::filesystem::path& root) {
  const std::string current = ReadFirstLine(root / "current-version");
  if (current.empty()) {
    std::cout << "installed=false\n";
    std::cout << "root=" << root.string() << "\n";
    return 1;
  }
  const std::filesystem::path version = root / "versions" / current;
  const std::filesystem::path executable = version / "RobloxStudioBeta.exe";
  const std::filesystem::path mcp = version / "StudioMCP.exe";
  std::error_code error;
  const bool installed =
      std::filesystem::is_regular_file(executable, error) && !error;
  std::cout << "installed=" << (installed ? "true" : "false") << "\n";
  std::cout << "root=" << root.string() << "\n";
  std::cout << "current=" << current << "\n";
  std::cout << "studio=" << executable.string() << "\n";
  std::cout << "mcp=" << mcp.string() << "\n";
  return installed ? 0 : 1;
}

int Check(const std::filesystem::path& root) {
  const aurora::studio::StudioVersionInfo version =
      aurora::studio::FetchLatestStudioVersion();
  if (!version) {
    std::cerr << "Aurora Studio runtime: " << version.error << "\n";
    return 1;
  }
  const aurora::studio::StudioPackageManifest manifest =
      aurora::studio::FetchStudioPackageManifest(version.upload);
  if (!manifest) {
    std::cerr << "Aurora Studio runtime: " << manifest.error << "\n";
    return 1;
  }
  std::uint64_t compressed = 0;
  std::uint64_t expanded = 0;
  for (const auto& package : manifest.packages) {
    compressed += package.compressed_bytes;
    expanded += package.uncompressed_bytes;
  }
  const std::filesystem::path executable =
      root / "versions" / version.upload / "RobloxStudioBeta.exe";
  std::error_code filesystem_error;
  const bool installed =
      std::filesystem::is_regular_file(executable, filesystem_error) &&
      !filesystem_error;
  std::cout << "version=" << version.version << "\n";
  std::cout << "upload=" << version.upload << "\n";
  std::cout << "packages=" << manifest.packages.size() << "\n";
  std::cout << "compressed_bytes=" << compressed << "\n";
  std::cout << "expanded_bytes=" << expanded << "\n";
  std::cout << "installed=" << (installed ? "true" : "false") << "\n";
  return 0;
}

int CompatibilityStatus() {
  const std::filesystem::path wine =
      aurora::studio::DefaultManagedKombuchaWine();
  const std::filesystem::path dxvk =
      aurora::studio::DefaultManagedDxvkRoot() / "x64";
  std::error_code error;
  const bool wine_installed =
      std::filesystem::is_regular_file(wine, error) && !error;
  error.clear();
  const bool d3d11_installed =
      std::filesystem::is_regular_file(dxvk / "d3d11.dll", error) && !error;
  error.clear();
  const bool dxgi_installed =
      std::filesystem::is_regular_file(dxvk / "dxgi.dll", error) && !error;
  const bool installed = wine_installed && d3d11_installed && dxgi_installed;
  std::cout << "compat_installed=" << (installed ? "true" : "false") << "\n";
  std::cout << "wine=" << wine.string() << "\n";
  std::cout << "dxvk=" << dxvk.string() << "\n";
  return installed ? 0 : 1;
}

int CompatibilityInstall(bool force) {
  const aurora::studio::CompatibilityProvisionResult compatibility =
      aurora::studio::UpdateManagedStudioCompatibility(force);
  if (!compatibility) {
    std::cerr << "Aurora Studio compatibility: " << compatibility.error << "\n";
    return 1;
  }
  std::cout << "aurorawine_tag=" << compatibility.kombucha_tag << "\n";
  std::cout << "dxvk_version=" << compatibility.dxvk_version << "\n";
  std::cout << "compat_changed="
            << (compatibility.changed ? "true" : "false") << "\n";
  std::cout << "wine=" << compatibility.wine_executable.string() << "\n";
  std::cout << "dxvk=" << compatibility.dxvk_x64_directory.string() << "\n";
  return 0;
}

int Install(const std::filesystem::path& root, bool force) {
  aurora::studio::StudioProvisionOptions options;
  options.runtime_root = root;
  options.force = force;
  const aurora::studio::StudioProvisionResult result =
      aurora::studio::ProvisionLatestStudio(options);
  if (!result) {
    std::cerr << "Aurora Studio runtime: " << result.error << "\n";
    return 1;
  }
  const aurora::studio::CompatibilityProvisionResult compatibility =
      aurora::studio::UpdateManagedStudioCompatibility(force);
  if (!compatibility) {
    std::cerr << "Aurora Studio compatibility: " << compatibility.error << "\n";
    return 1;
  }
  bool dxvk_changed = false;
  std::string dxvk_error;
  if (!aurora::studio::SyncManagedDxvkToStudio(
          result.studio_executable, &dxvk_changed, &dxvk_error)) {
    std::cerr << "Aurora Studio DXVK: " << dxvk_error << "\n";
    return 1;
  }
  std::cout << "version=" << result.version << "\n";
  std::cout << "upload=" << result.upload << "\n";
  std::cout << "changed="
            << ((result.changed || compatibility.changed || dxvk_changed)
                    ? "true"
                    : "false")
            << "\n";
  std::cout << "studio=" << result.studio_executable.string() << "\n";
  std::cout << "mcp=" << result.studio_mcp.string() << "\n";
  std::cout << "aurorawine_tag=" << compatibility.kombucha_tag << "\n";
  std::cout << "dxvk_version=" << compatibility.dxvk_version << "\n";
  std::cout << "wine=" << compatibility.wine_executable.string() << "\n";
  return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc < 2) {
    Usage();
    return 2;
  }
  const std::string command = argv[1] == nullptr ? "" : argv[1];
  std::filesystem::path root = aurora::studio::DefaultStudioRuntimeRoot();
  bool force = false;
  for (int index = 2; index < argc; ++index) {
    const std::string argument = argv[index] == nullptr ? "" : argv[index];
    if (argument == "--force") {
      force = true;
      continue;
    }
    if (argument == "--root" && index + 1 < argc && argv[index + 1] != nullptr) {
      root = argv[++index];
      if (!root.is_absolute()) {
        std::cerr << "Aurora Studio runtime: --root must be absolute\n";
        return 2;
      }
      continue;
    }
    Usage();
    return 2;
  }

  if (command == "status") return Status(root);
  if (command == "check") return Check(root);
  if (command == "install") return Install(root, force);
  if (command == "compat-status") return CompatibilityStatus();
  if (command == "compat-install") return CompatibilityInstall(force);
  Usage();
  return 2;
}
