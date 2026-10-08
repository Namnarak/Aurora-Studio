#ifndef AURORA_STUDIO_STUDIO_PROVISIONER_H_
#define AURORA_STUDIO_STUDIO_PROVISIONER_H_

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aurora::studio {

struct StudioVersionInfo {
  std::string version;
  std::string upload;
  std::string error;

  explicit operator bool() const { return error.empty(); }
};

struct StudioPackage {
  std::string name;
  std::string md5;
  std::uint64_t compressed_bytes = 0;
  std::uint64_t uncompressed_bytes = 0;
  std::filesystem::path destination;
};

struct StudioPackageManifest {
  std::vector<StudioPackage> packages;
  std::string error;

  explicit operator bool() const { return error.empty(); }
};

struct StudioProvisionOptions {
  std::filesystem::path runtime_root;
  bool force = false;
  int progress_fd = -1;
};

struct StudioProvisionResult {
  bool changed = false;
  std::string version;
  std::string upload;
  std::filesystem::path version_directory;
  std::filesystem::path studio_executable;
  std::filesystem::path studio_mcp;
  std::string error;

  explicit operator bool() const { return error.empty(); }
};

std::filesystem::path DefaultStudioRuntimeRoot();

std::optional<std::filesystem::path> StudioPackageDestination(
    std::string_view package_name);

StudioVersionInfo ParseStudioVersionResponse(std::string_view json);
StudioPackageManifest ParseStudioPackageManifest(std::string_view manifest);

StudioVersionInfo FetchLatestStudioVersion();
StudioPackageManifest FetchStudioPackageManifest(std::string_view upload);

StudioProvisionResult ProvisionLatestStudio(
    const StudioProvisionOptions& options = {});

}  // namespace aurora::studio

#endif  // AURORA_STUDIO_STUDIO_PROVISIONER_H_
