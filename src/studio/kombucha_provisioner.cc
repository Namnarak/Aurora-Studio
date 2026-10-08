// SPDX-License-Identifier: GPL-3.0-only

#include "studio/kombucha_provisioner.h"

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "runtime/managed_process.h"
#include "studio/studio_provisioner.h"
#include "update/http_download.h"

namespace aurora::studio {
namespace {

using Json = nlohmann::json;
using Path = std::filesystem::path;

constexpr std::string_view kKombuchaLatestApi =
    "https://api.github.com/repos/Namnarak/AuroraWine/releases/latest";
constexpr std::string_view kDxvkReleaseApi =
    "https://api.github.com/repos/doitsujin/dxvk/releases/tags/v3.1";
constexpr std::string_view kDxvkLegacyReleaseApi =
    "https://api.github.com/repos/pythonlover02/DXVK-Sarek/releases/tags/v1.13.0";
constexpr std::string_view kDxvkVersion = "3.1";
constexpr std::string_view kDxvkLegacyVersion = "1.13.0";
constexpr std::size_t kMaximumReleaseMetadataBytes = 2U * 1024U * 1024U;
constexpr std::uint64_t kMaximumKombuchaArchiveBytes =
    512ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaximumDxvkArchiveBytes =
    128ULL * 1024ULL * 1024ULL;

struct ReleaseAsset {
  std::string tag;
  std::string name;
  std::string url;
  std::string sha256;
  std::uint64_t size = 0;
  std::string error;

  explicit operator bool() const { return error.empty(); }
};

Path CacheRoot() {
  return DefaultStudioRuntimeRoot() / "cache/compatibility";
}

std::vector<std::string> GithubHeaders() {
  return {
      "Accept: application/vnd.github+json",
      "X-GitHub-Api-Version: 2022-11-28",
      "User-Agent: Aurora-Studio",
  };
}

std::vector<std::string> GithubDownloadHosts() {
  return {
      "github.com",
      "release-assets.githubusercontent.com",
      "objects.githubusercontent.com",
      "github-production-release-asset-2e65be.s3.amazonaws.com",
  };
}

std::optional<Path> FindOnPath(const std::string& name) {
  const char* raw = std::getenv("PATH");
  const std::string path =
      raw == nullptr ? "/usr/local/bin:/usr/bin:/bin" : std::string(raw);
  std::size_t begin = 0;
  while (begin <= path.size()) {
    const std::size_t separator = path.find(':', begin);
    const std::string directory =
        separator == std::string::npos
            ? path.substr(begin)
            : path.substr(begin, separator - begin);
    const Path candidate = Path(directory.empty() ? "." : directory) / name;
    if (access(candidate.c_str(), X_OK) == 0) return candidate;
    if (separator == std::string::npos) break;
    begin = separator + 1;
  }
  return std::nullopt;
}

bool WriteAtomic(const Path& path, std::string_view bytes, std::string* error) {
  std::error_code filesystem_error;
  std::filesystem::create_directories(path.parent_path(), filesystem_error);
  if (filesystem_error) {
    if (error != nullptr) {
      *error = "cannot create compatibility metadata directory: " +
               filesystem_error.message();
    }
    return false;
  }
  const Path temporary =
      path.string() + ".tmp-" + std::to_string(static_cast<long long>(getpid()));
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) {
      if (error != nullptr) *error = "cannot create compatibility metadata";
      return false;
    }
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output) {
      std::filesystem::remove(temporary, filesystem_error);
      if (error != nullptr) *error = "cannot finish compatibility metadata";
      return false;
    }
  }
  chmod(temporary.c_str(), 0600);
  std::filesystem::rename(temporary, path, filesystem_error);
  if (filesystem_error) {
    std::filesystem::remove(temporary, filesystem_error);
    if (error != nullptr) {
      *error = "cannot commit compatibility metadata: " +
               filesystem_error.message();
    }
    return false;
  }
  return true;
}

std::string ReadMetadataString(const Path& path, std::string_view key) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return {};
  Json document = Json::parse(input, nullptr, false);
  if (document.is_discarded() || !document.is_object()) return {};
  const auto found = document.find(std::string(key));
  return found != document.end() && found->is_string()
             ? found->get<std::string>()
             : std::string();
}

bool IsRegularExecutable(const Path& path) {
  std::error_code error;
  return std::filesystem::is_regular_file(path, error) && !error &&
         access(path.c_str(), X_OK) == 0;
}

bool IsRegularNonEmpty(const Path& path) {
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error) || error) return false;
  const auto size = std::filesystem::file_size(path, error);
  return !error && size > 0;
}

std::string DigestHex(std::string_view digest) {
  constexpr std::string_view prefix = "sha256:";
  if (digest.rfind(prefix, 0) != 0 || digest.size() != prefix.size() + 64U) {
    return {};
  }
  std::string value(digest.substr(prefix.size()));
  if (!std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return (character >= '0' && character <= '9') ||
               (character >= 'a' && character <= 'f') ||
               (character >= 'A' && character <= 'F');
      })) {
    return {};
  }
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  return value;
}

ReleaseAsset FetchReleaseAsset(
    std::string_view api_url,
    const std::function<bool(std::string_view)>& select_asset,
    std::uint64_t maximum_asset_bytes) {
  ReleaseAsset result;
  update::HttpTransferRequest request;
  request.url = std::string(api_url);
  request.headers = GithubHeaders();
  request.allowed_hosts = {"api.github.com"};
  request.exact_host_matches = true;
  request.maximum_bytes = kMaximumReleaseMetadataBytes;
  request.transfer_timeout_ms = 30000;
  const update::HttpBytesResult downloaded = update::DownloadBytes(request);
  if (!downloaded) {
    result.error = downloaded.error;
    return result;
  }

  const Json document = Json::parse(downloaded.bytes, nullptr, false);
  if (document.is_discarded() || !document.is_object()) {
    result.error = "cannot parse GitHub release metadata";
    return result;
  }
  const auto tag = document.find("tag_name");
  const auto assets = document.find("assets");
  if (tag == document.end() || !tag->is_string() ||
      assets == document.end() || !assets->is_array()) {
    result.error = "GitHub release metadata is missing tag/assets";
    return result;
  }
  result.tag = tag->get<std::string>();
  for (const Json& asset : *assets) {
    if (!asset.is_object()) continue;
    const auto name = asset.find("name");
    const auto url = asset.find("browser_download_url");
    const auto size = asset.find("size");
    if (name == asset.end() || !name->is_string() ||
        url == asset.end() || !url->is_string() ||
        size == asset.end() || !size->is_number_unsigned()) {
      continue;
    }
    const std::string candidate_name = name->get<std::string>();
    if (!select_asset(candidate_name)) continue;
    const std::uint64_t candidate_size = size->get<std::uint64_t>();
    if (candidate_size == 0 || candidate_size > maximum_asset_bytes) {
      result.error = "GitHub release asset has an unsafe size";
      return result;
    }
    result.name = candidate_name;
    result.url = url->get<std::string>();
    result.size = candidate_size;
    const auto digest = asset.find("digest");
    if (digest != asset.end() && digest->is_string()) {
      result.sha256 = DigestHex(digest->get<std::string>());
    }
    return result;
  }
  result.error = "required GitHub release asset was not found";
  return result;
}

bool DownloadReleaseAsset(const ReleaseAsset& asset, const Path& destination,
                          int progress_fd, std::string* error) {
  update::HttpTransferRequest request;
  request.url = asset.url;
  request.headers = GithubHeaders();
  request.allowed_hosts = GithubDownloadHosts();
  request.exact_host_matches = true;
  request.maximum_bytes = static_cast<std::size_t>(asset.size);
  request.transfer_timeout_ms = 0;
  request.total_timeout_ms = 0;
  request.low_speed_bytes_per_second = 4096;
  request.low_speed_seconds = 90;
  const update::HttpDownloadResult downloaded =
      update::DownloadFile(request, destination, progress_fd);
  if (!downloaded) {
    if (error != nullptr) *error = downloaded.error;
    return false;
  }
  if (downloaded.bytes_written != asset.size) {
    if (error != nullptr) *error = "downloaded release asset size mismatch";
    return false;
  }
  if (!asset.sha256.empty() && downloaded.sha256 != asset.sha256) {
    std::error_code remove_error;
    std::filesystem::remove(destination, remove_error);
    if (error != nullptr) *error = "downloaded release asset SHA-256 mismatch";
    return false;
  }
  return true;
}

bool RunTar(const std::vector<std::string>& arguments, std::string* error) {
  const auto tar = FindOnPath("tar");
  if (!tar.has_value()) {
    if (error != nullptr) *error = "tar is required to install compatibility runtime";
    return false;
  }
  runtime::ManagedProcessSpec spec;
  spec.executable = tar->string();
  spec.arguments = arguments;
  spec.inherit_stdio = false;
  std::string start_error;
  runtime::ManagedProcess process =
      runtime::ManagedProcess::Start(spec, &start_error);
  if (!process.valid()) {
    if (error != nullptr) {
      *error = start_error.empty() ? "cannot start tar" : start_error;
    }
    return false;
  }
  int status = 0;
  std::string wait_error;
  if (!process.Wait(&status, &wait_error)) {
    if (error != nullptr) {
      *error = wait_error.empty() ? "cannot wait for tar" : wait_error;
    }
    return false;
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    if (error != nullptr) *error = "tar failed while extracting compatibility runtime";
    return false;
  }
  return true;
}

bool ReplaceDirectory(const Path& staging, const Path& target,
                      std::string* error) {
  std::error_code filesystem_error;
  std::filesystem::create_directories(target.parent_path(), filesystem_error);
  if (filesystem_error) {
    if (error != nullptr) {
      *error = "cannot create compatibility runtime directory: " +
               filesystem_error.message();
    }
    return false;
  }
  const Path old =
      target.string() + ".old-" + std::to_string(static_cast<long long>(getpid()));
  std::filesystem::remove_all(old, filesystem_error);
  filesystem_error.clear();
  if (std::filesystem::exists(target, filesystem_error) && !filesystem_error) {
    std::filesystem::rename(target, old, filesystem_error);
    if (filesystem_error) {
      if (error != nullptr) {
        *error = "cannot move previous compatibility runtime aside: " +
                 filesystem_error.message();
      }
      return false;
    }
  }
  filesystem_error.clear();
  std::filesystem::rename(staging, target, filesystem_error);
  if (filesystem_error) {
    std::error_code restore_error;
    if (std::filesystem::exists(old, restore_error) && !restore_error) {
      std::filesystem::rename(old, target, restore_error);
    }
    if (error != nullptr) {
      *error = "cannot activate compatibility runtime: " +
               filesystem_error.message();
    }
    return false;
  }
  std::filesystem::remove_all(old, filesystem_error);
  return true;
}

bool InstallKombucha(const ReleaseAsset& release, int progress_fd,
                     bool force, bool* changed, std::string* error) {
  const Path target = DefaultManagedKombuchaRoot();
  const Path wine = target / "bin/wine";
  const Path metadata = target / ".aurorawine.json";
  if (!force && IsRegularExecutable(wine) &&
      ReadMetadataString(metadata, "tag") == release.tag) {
    return true;
  }

  std::error_code filesystem_error;
  const Path cache = CacheRoot();
  std::filesystem::create_directories(cache, filesystem_error);
  if (filesystem_error) {
    if (error != nullptr) *error = filesystem_error.message();
    return false;
  }
  const Path archive = cache / release.name;
  if (!DownloadReleaseAsset(release, archive, progress_fd, error)) return false;

  const Path staging =
      DefaultStudioRuntimeRoot() /
      ("staging-kombucha-" + std::to_string(static_cast<long long>(getpid())));
  std::filesystem::remove_all(staging, filesystem_error);
  filesystem_error.clear();
  std::filesystem::create_directories(staging, filesystem_error);
  if (filesystem_error) {
    if (error != nullptr) *error = filesystem_error.message();
    return false;
  }
  if (!RunTar({"-xJf", archive.string(), "-C", staging.string(),
               "--strip-components=1", "--no-same-owner", "--no-same-permissions"},
              error)) {
    std::filesystem::remove_all(staging, filesystem_error);
    return false;
  }
  if (!IsRegularExecutable(staging / "bin/wine")) {
    std::filesystem::remove_all(staging, filesystem_error);
    if (error != nullptr) *error = "Kombucha archive does not contain bin/wine";
    return false;
  }

  Json metadata_document = {
      {"schema_version", 1},
      {"component", "AuroraWine"},
      {"repository", "Namnarak/AuroraWine"},
      {"upstream", "vinegarhq/kombucha"},
      {"license", "LGPL-2.1"},
      {"tag", release.tag},
      {"asset", release.name},
      {"sha256", release.sha256},
  };
  std::string metadata_error;
  if (!WriteAtomic(staging / ".aurorawine.json",
                   metadata_document.dump(2) + "\n", &metadata_error)) {
    std::filesystem::remove_all(staging, filesystem_error);
    if (error != nullptr) *error = metadata_error;
    return false;
  }
  if (!ReplaceDirectory(staging, target, error)) {
    std::filesystem::remove_all(staging, filesystem_error);
    return false;
  }
  if (changed != nullptr) *changed = true;
  return true;
}

bool InstallDxvk(int progress_fd, bool force, bool* changed,
                 std::string* error) {
  const Path target = DefaultManagedDxvkRoot();
  const Path d3d11 = target / "x64/d3d11.dll";
  const Path dxgi = target / "x64/dxgi.dll";
  const Path metadata = target / ".aurora-dxvk.json";
  if (!force && IsRegularNonEmpty(d3d11) && IsRegularNonEmpty(dxgi) &&
      ReadMetadataString(metadata, "version") == kDxvkVersion) {
    return true;
  }

  const ReleaseAsset release = FetchReleaseAsset(
      kDxvkReleaseApi,
      [](std::string_view name) { return name == "dxvk-3.1.tar.gz"; },
      kMaximumDxvkArchiveBytes);
  if (!release) {
    if (error != nullptr) *error = release.error;
    return false;
  }

  std::error_code filesystem_error;
  const Path cache = CacheRoot();
  std::filesystem::create_directories(cache, filesystem_error);
  if (filesystem_error) {
    if (error != nullptr) *error = filesystem_error.message();
    return false;
  }
  const Path archive = cache / release.name;
  if (!DownloadReleaseAsset(release, archive, progress_fd, error)) return false;

  const Path staging =
      DefaultStudioRuntimeRoot() /
      ("staging-dxvk-" + std::to_string(static_cast<long long>(getpid())));
  std::filesystem::remove_all(staging, filesystem_error);
  filesystem_error.clear();
  std::filesystem::create_directories(staging, filesystem_error);
  if (filesystem_error) {
    if (error != nullptr) *error = filesystem_error.message();
    return false;
  }
  if (!RunTar({"-xzf", archive.string(), "-C", staging.string(),
               "--strip-components=1", "--no-same-owner", "--no-same-permissions",
               "dxvk-3.1/x64/d3d11.dll", "dxvk-3.1/x64/dxgi.dll"},
              error)) {
    std::filesystem::remove_all(staging, filesystem_error);
    return false;
  }
  if (!IsRegularNonEmpty(staging / "x64/d3d11.dll") ||
      !IsRegularNonEmpty(staging / "x64/dxgi.dll")) {
    std::filesystem::remove_all(staging, filesystem_error);
    if (error != nullptr) *error = "DXVK archive is missing x64 D3D11/DXGI";
    return false;
  }

  Json metadata_document = {
      {"schema_version", 1},
      {"component", "dxvk"},
      {"repository", "doitsujin/dxvk"},
      {"version", std::string(kDxvkVersion)},
      {"asset", release.name},
      {"sha256", release.sha256},
  };
  std::string metadata_error;
  if (!WriteAtomic(staging / ".aurora-dxvk.json",
                   metadata_document.dump(2) + "\n", &metadata_error)) {
    std::filesystem::remove_all(staging, filesystem_error);
    if (error != nullptr) *error = metadata_error;
    return false;
  }
  if (!ReplaceDirectory(staging, target, error)) {
    std::filesystem::remove_all(staging, filesystem_error);
    return false;
  }
  if (changed != nullptr) *changed = true;
  return true;
}

bool InstallDxvkLegacy(int progress_fd, bool force, bool* changed,
                       std::string* error) {
  const Path target = DefaultManagedDxvkLegacyRoot();
  const Path d3d11 = target / "x64/d3d11.dll";
  const Path dxgi = target / "x64/dxgi.dll";
  const Path metadata = target / ".aurora-dxvk-sarek.json";
  if (!force && IsRegularNonEmpty(d3d11) && IsRegularNonEmpty(dxgi) &&
      ReadMetadataString(metadata, "version") == kDxvkLegacyVersion) {
    return true;
  }

  const std::string archive_name =
      "dxvk-sarek-" + std::string(kDxvkLegacyVersion) + ".tar.gz";
  const std::string archive_root =
      "dxvk-sarek-" + std::string(kDxvkLegacyVersion);
  const ReleaseAsset release = FetchReleaseAsset(
      kDxvkLegacyReleaseApi,
      [&archive_name](std::string_view name) { return name == archive_name; },
      kMaximumDxvkArchiveBytes);
  if (!release) {
    if (error != nullptr) *error = release.error;
    return false;
  }

  std::error_code filesystem_error;
  const Path cache = CacheRoot();
  std::filesystem::create_directories(cache, filesystem_error);
  if (filesystem_error) {
    if (error != nullptr) *error = filesystem_error.message();
    return false;
  }
  const Path archive = cache / release.name;
  if (!DownloadReleaseAsset(release, archive, progress_fd, error)) return false;

  const Path staging =
      DefaultStudioRuntimeRoot() /
      ("staging-dxvk-sarek-" +
       std::to_string(static_cast<long long>(getpid())));
  std::filesystem::remove_all(staging, filesystem_error);
  filesystem_error.clear();
  std::filesystem::create_directories(staging, filesystem_error);
  if (filesystem_error) {
    if (error != nullptr) *error = filesystem_error.message();
    return false;
  }
  const std::string d3d11_member = archive_root + "/build/x64/d3d11.dll";
  const std::string dxgi_member = archive_root + "/build/x64/dxgi.dll";
  if (!RunTar({"-xzf", archive.string(), "-C", staging.string(),
               "--strip-components=2", "--no-same-owner",
               "--no-same-permissions", d3d11_member, dxgi_member},
              error)) {
    std::filesystem::remove_all(staging, filesystem_error);
    return false;
  }
  if (!IsRegularNonEmpty(staging / "x64/d3d11.dll") ||
      !IsRegularNonEmpty(staging / "x64/dxgi.dll")) {
    std::filesystem::remove_all(staging, filesystem_error);
    if (error != nullptr) *error = "DXVK Legacy archive is missing x64 D3D11/DXGI";
    return false;
  }

  Json metadata_document = {
      {"schema_version", 1},
      {"component", "dxvk-sarek"},
      {"repository", "pythonlover02/DXVK-Sarek"},
      {"version", std::string(kDxvkLegacyVersion)},
      {"asset", release.name},
      {"sha256", release.sha256},
  };
  std::string metadata_error;
  if (!WriteAtomic(staging / ".aurora-dxvk-sarek.json",
                   metadata_document.dump(2) + "\n", &metadata_error)) {
    std::filesystem::remove_all(staging, filesystem_error);
    if (error != nullptr) *error = metadata_error;
    return false;
  }
  if (!ReplaceDirectory(staging, target, error)) {
    std::filesystem::remove_all(staging, filesystem_error);
    return false;
  }
  if (changed != nullptr) *changed = true;
  return true;
}

bool CopyFileAtomic(const Path& source, const Path& destination,
                    std::string* error) {
  std::error_code filesystem_error;
  const Path temporary = destination.string() + ".aurora-tmp-" +
                         std::to_string(static_cast<long long>(getpid()));
  std::filesystem::copy_file(source, temporary,
                             std::filesystem::copy_options::overwrite_existing,
                             filesystem_error);
  if (filesystem_error) {
    if (error != nullptr) {
      *error = "cannot stage " + destination.filename().string() + ": " +
               filesystem_error.message();
    }
    return false;
  }
  std::filesystem::rename(temporary, destination, filesystem_error);
  if (filesystem_error) {
    std::filesystem::remove(temporary, filesystem_error);
    if (error != nullptr) {
      *error = "cannot install " + destination.filename().string() + ": " +
               filesystem_error.message();
    }
    return false;
  }
  return true;
}

}  // namespace

std::filesystem::path DefaultManagedKombuchaRoot() {
  return DefaultStudioRuntimeRoot() / "wine";
}

std::filesystem::path DefaultManagedKombuchaWine() {
  return DefaultManagedKombuchaRoot() / "bin/wine";
}

std::filesystem::path DefaultManagedDxvkRoot() {
  return DefaultStudioRuntimeRoot() / "dxvk" / std::string(kDxvkVersion);
}

std::filesystem::path DefaultManagedDxvkLegacyRoot() {
  return DefaultStudioRuntimeRoot() / "dxvk-sarek" /
         std::string(kDxvkLegacyVersion);
}

CompatibilityProvisionResult UpdateManagedStudioCompatibility(
    bool force, int progress_fd) {
  CompatibilityProvisionResult result;
  const ReleaseAsset kombucha = FetchReleaseAsset(
      kKombuchaLatestApi,
      [](std::string_view name) {
        const bool aurorawine = name.rfind("aurorawine-stable+", 0) == 0;
        const bool legacy_kombucha =
            name.rfind("kombucha-stable+", 0) == 0;
        return (aurorawine || legacy_kombucha) && name.size() > 7 &&
               name.substr(name.size() - 7) == ".tar.xz";
      },
      kMaximumKombuchaArchiveBytes);
  if (!kombucha) {
    result.error = "AuroraWine: " + kombucha.error;
    return result;
  }

  bool kombucha_changed = false;
  if (!InstallKombucha(kombucha, progress_fd, force, &kombucha_changed,
                       &result.error)) {
    result.error = "AuroraWine: " + result.error;
    return result;
  }
  bool dxvk_changed = false;
  if (!InstallDxvk(progress_fd, force, &dxvk_changed, &result.error)) {
    result.error = "DXVK: " + result.error;
    return result;
  }

  result.kombucha_tag = kombucha.tag;
  result.dxvk_version = std::string(kDxvkVersion);
  result.wine_executable = DefaultManagedKombuchaWine();
  result.dxvk_x64_directory = DefaultManagedDxvkRoot() / "x64";
  result.changed = kombucha_changed || dxvk_changed;
  return result;
}

namespace {

bool SyncDxvkDirectoryToStudio(const std::filesystem::path& studio_executable,
                               const Path& dxvk_root,
                               std::string_view version, bool* changed,
                               std::string* error) {
  if (changed != nullptr) *changed = false;
  std::error_code filesystem_error;
  if (!std::filesystem::is_regular_file(studio_executable, filesystem_error) ||
      filesystem_error) {
    if (error != nullptr) *error = "RobloxStudioBeta.exe is missing";
    return false;
  }

  const Path source = dxvk_root / "x64";
  const Path source_d3d11 = source / "d3d11.dll";
  const Path source_dxgi = source / "dxgi.dll";
  if (!IsRegularNonEmpty(source_d3d11) || !IsRegularNonEmpty(source_dxgi)) {
    if (error != nullptr) *error = "managed DXVK runtime is incomplete";
    return false;
  }

  const Path directory = studio_executable.parent_path();
  const Path marker = directory / ".aurora-dxvk-version";
  std::ifstream input(marker);
  std::string installed_version;
  std::getline(input, installed_version);
  if (installed_version == version &&
      IsRegularNonEmpty(directory / "d3d11.dll") &&
      IsRegularNonEmpty(directory / "dxgi.dll")) {
    return true;
  }

  if (!CopyFileAtomic(source_d3d11, directory / "d3d11.dll", error) ||
      !CopyFileAtomic(source_dxgi, directory / "dxgi.dll", error)) {
    return false;
  }
  if (!WriteAtomic(marker, std::string(version) + "\n", error)) {
    return false;
  }
  if (changed != nullptr) *changed = true;
  return true;
}

}  // namespace

bool SyncManagedDxvkToStudio(const std::filesystem::path& studio_executable,
                             bool* changed, std::string* error) {
  return SyncDxvkDirectoryToStudio(studio_executable,
                                   DefaultManagedDxvkRoot(), kDxvkVersion,
                                   changed, error);
}

CompatibilityProvisionResult EnsureManagedStudioCompatibility(
    const std::filesystem::path& studio_executable, int progress_fd) {
  CompatibilityProvisionResult result;
  const Path wine = DefaultManagedKombuchaWine();
  const Path dxvk = DefaultManagedDxvkRoot() / "x64";
  if (!IsRegularExecutable(wine) ||
      !IsRegularNonEmpty(dxvk / "d3d11.dll") ||
      !IsRegularNonEmpty(dxvk / "dxgi.dll")) {
    result = UpdateManagedStudioCompatibility(false, progress_fd);
    if (!result) return result;
  } else {
    result.kombucha_tag = ReadMetadataString(
        DefaultManagedKombuchaRoot() / ".aurorawine.json", "tag");
    result.dxvk_version = ReadMetadataString(
        DefaultManagedDxvkRoot() / ".aurora-dxvk.json", "version");
    result.wine_executable = wine;
    result.dxvk_x64_directory = dxvk;
  }

  bool dxvk_changed = false;
  std::string sync_error;
  if (!SyncManagedDxvkToStudio(studio_executable, &dxvk_changed, &sync_error)) {
    result.error = "DXVK: " + sync_error;
    return result;
  }
  result.changed = result.changed || dxvk_changed;
  return result;
}

CompatibilityProvisionResult EnsureManagedStudioCompatibilityForMode(
    const std::filesystem::path& studio_executable,
    StudioGraphicsMode graphics_mode, int progress_fd) {
  CompatibilityProvisionResult result =
      EnsureManagedStudioCompatibility(studio_executable, progress_fd);
  if (!result || graphics_mode != StudioGraphicsMode::kDxvkLegacy) {
    return result;
  }

  bool legacy_changed = false;
  if (!InstallDxvkLegacy(progress_fd, false, &legacy_changed, &result.error)) {
    result.error = "DXVK Legacy: " + result.error;
    return result;
  }
  bool sync_changed = false;
  if (!SyncDxvkDirectoryToStudio(studio_executable,
                                 DefaultManagedDxvkLegacyRoot(),
                                 kDxvkLegacyVersion, &sync_changed,
                                 &result.error)) {
    result.error = "DXVK Legacy: " + result.error;
    return result;
  }
  result.dxvk_version = "Sarek " + std::string(kDxvkLegacyVersion);
  result.dxvk_x64_directory = DefaultManagedDxvkLegacyRoot() / "x64";
  result.changed = result.changed || legacy_changed || sync_changed;
  return result;
}

}  // namespace aurora::studio
