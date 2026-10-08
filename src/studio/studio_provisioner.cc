// SPDX-License-Identifier: GPL-3.0-only

#include "studio/studio_provisioner.h"

#include <openssl/evp.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <system_error>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "update/http_download.h"
#include "update/zip_archive.h"

namespace aurora::studio {
namespace {

using Json = nlohmann::json;

constexpr std::string_view kVersionEndpoint =
    "https://clientsettings.roblox.com/v2/client-version/WindowsStudio64";
constexpr std::string_view kSetupBase = "https://setup.rbxcdn.com/";
constexpr std::size_t kMaximumVersionResponse = 64U * 1024U;
constexpr std::size_t kMaximumManifestBytes = 2U * 1024U * 1024U;
constexpr std::uint64_t kMaximumPackageBytes = 2ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaximumExpandedPackageBytes =
    4ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr std::string_view kStudioAppSettingsXml =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<Settings>\n"
    "        <ContentFolder>content</ContentFolder>\n"
    "        <BaseUrl>http://www.roblox.com</BaseUrl>\n"
    "</Settings>\n";

const std::unordered_map<std::string, std::string>& PackageMap() {
  static const std::unordered_map<std::string, std::string> kMap = {
      {"Libraries.zip", ""},
      {"redist.zip", ""},
      {"shaders.zip", "shaders"},
      {"ssl.zip", "ssl"},
      {"WebView2.zip", ""},
      {"WebView2RuntimeInstaller.zip", "WebView2RuntimeInstaller"},
      {"content-avatar.zip", "content/avatar"},
      {"content-configs.zip", "content/configs"},
      {"content-fonts.zip", "content/fonts"},
      {"content-sky.zip", "content/sky"},
      {"content-sounds.zip", "content/sounds"},
      {"content-textures2.zip", "content/textures"},
      {"content-models.zip", "content/models"},
      {"content-textures3.zip", "PlatformContent/pc/textures"},
      {"content-terrain.zip", "PlatformContent/pc/terrain"},
      {"content-platform-fonts.zip", "PlatformContent/pc/fonts"},
      {"content-platform-dictionaries.zip",
       "PlatformContent/pc/shared_compression_dictionaries"},
      {"extracontent-luapackages.zip", "ExtraContent/LuaPackages"},
      {"extracontent-translations.zip", "ExtraContent/translations"},
      {"extracontent-models.zip", "ExtraContent/models"},
      {"extracontent-textures.zip", "ExtraContent/textures"},
      {"extracontent-places.zip", "ExtraContent/places"},
      {"RobloxStudio.zip", ""},
      {"LibrariesQt5.zip", ""},
      {"content-studio_svg_textures.zip", "content/studio_svg_textures"},
      {"content-qt_translations.zip", "content/qt_translations"},
      {"content-api-docs.zip", "content/api_docs"},
      {"extracontent-scripts.zip", "ExtraContent/scripts"},
      {"studiocontent-models.zip", "StudioContent/models"},
      {"studiocontent-textures.zip", "StudioContent/textures"},
      {"BuiltInPlugins.zip", "BuiltInPlugins"},
      {"BuiltInStandalonePlugins.zip", "BuiltInStandalonePlugins"},
      {"ApplicationConfig.zip", "ApplicationConfig"},
      {"Plugins.zip", "Plugins"},
      {"Qml.zip", "Qml"},
      {"StudioFonts.zip", "StudioFonts"},
      {"RibbonConfig.zip", "RibbonConfig"},
  };
  return kMap;
}

bool IsHex(std::string_view value, std::size_t exact) {
  return value.size() == exact &&
         std::all_of(value.begin(), value.end(), [](unsigned char c) {
           return std::isxdigit(c) != 0;
         });
}

bool IsSafeUpload(std::string_view upload) {
  if (upload.size() < 9 || upload.size() > 80 ||
      upload.rfind("version-", 0) != 0) {
    return false;
  }
  return std::all_of(upload.begin() + 8, upload.end(), [](unsigned char c) {
    return std::isxdigit(c) != 0;
  });
}

bool IsSafePackageName(std::string_view name) {
  return !name.empty() && name.size() <= 128 &&
         name.size() > 4 && name.substr(name.size() - 4) == ".zip" &&
         name.find('/') == std::string_view::npos &&
         name.find('\\') == std::string_view::npos &&
         name.find('\0') == std::string_view::npos &&
         name != "." && name != "..";
}

std::optional<std::uint64_t> ParseSize(std::string_view value) {
  if (value.empty() || value.size() > 24 ||
      !std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isdigit(c) != 0;
      })) {
    return std::nullopt;
  }
  errno = 0;
  char* end = nullptr;
  const std::string text(value);
  const unsigned long long parsed = std::strtoull(text.c_str(), &end, 10);
  if (errno != 0 || end == text.c_str() || *end != '\0') return std::nullopt;
  return static_cast<std::uint64_t>(parsed);
}

std::vector<std::string> Lines(std::string_view input) {
  std::vector<std::string> lines;
  std::size_t begin = 0;
  while (begin <= input.size()) {
    std::size_t end = input.find('\n', begin);
    if (end == std::string_view::npos) end = input.size();
    std::string line(input.substr(begin, end - begin));
    if (!line.empty() && line.back() == '\r') line.pop_back();
    lines.push_back(std::move(line));
    if (end == input.size()) break;
    begin = end + 1;
  }
  while (!lines.empty() && lines.back().empty()) lines.pop_back();
  return lines;
}

std::string Md5File(const std::filesystem::path& path, std::string* error) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    *error = "cannot open downloaded package for hashing: " + path.string();
    return {};
  }
  EVP_MD_CTX* raw = EVP_MD_CTX_new();
  if (raw == nullptr) {
    *error = "cannot allocate OpenSSL digest context";
    return {};
  }
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(
      raw, EVP_MD_CTX_free);
  if (EVP_DigestInit_ex(context.get(), EVP_md5(), nullptr) != 1) {
    *error = "cannot initialize package MD5 verification";
    return {};
  }
  std::array<char, 256U * 1024U> buffer{};
  while (input) {
    input.read(buffer.data(), buffer.size());
    const std::streamsize read = input.gcount();
    if (read > 0 &&
        EVP_DigestUpdate(context.get(), buffer.data(),
                         static_cast<std::size_t>(read)) != 1) {
      *error = "cannot hash downloaded package";
      return {};
    }
  }
  if (!input.eof()) {
    *error = "cannot read downloaded package";
    return {};
  }
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int digest_size = 0;
  if (EVP_DigestFinal_ex(context.get(), digest.data(), &digest_size) != 1 ||
      digest_size != 16) {
    *error = "cannot finish package MD5 verification";
    return {};
  }
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (unsigned int index = 0; index < digest_size; ++index) {
    output << std::setw(2) << static_cast<unsigned int>(digest[index]);
  }
  return output.str();
}

bool VerifyPackageFile(const StudioPackage& package,
                       const std::filesystem::path& path,
                       std::string* error) {
  std::error_code filesystem_error;
  if (!std::filesystem::is_regular_file(path, filesystem_error) ||
      filesystem_error) {
    *error = "downloaded package is not a regular file: " + path.string();
    return false;
  }
  const std::uint64_t size = std::filesystem::file_size(path, filesystem_error);
  if (filesystem_error || size != package.compressed_bytes) {
    *error = "downloaded package size mismatch: " + package.name;
    return false;
  }
  std::string hash_error;
  const std::string digest = Md5File(path, &hash_error);
  if (!hash_error.empty()) {
    *error = hash_error;
    return false;
  }
  std::string expected = package.md5;
  std::transform(expected.begin(), expected.end(), expected.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (digest != expected) {
    *error = "downloaded package MD5 mismatch: " + package.name;
    return false;
  }
  return true;
}

bool WriteAtomic(const std::filesystem::path& path, std::string_view bytes,
                 std::string* error) {
  std::error_code filesystem_error;
  std::filesystem::create_directories(path.parent_path(), filesystem_error);
  if (filesystem_error) {
    *error = "cannot create runtime metadata directory: " +
             filesystem_error.message();
    return false;
  }
  const std::filesystem::path temporary =
      path.string() + ".tmp-" + std::to_string(getpid());
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) {
      *error = "cannot create temporary runtime metadata";
      return false;
    }
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output) {
      std::filesystem::remove(temporary, filesystem_error);
      *error = "cannot finish runtime metadata";
      return false;
    }
  }
  std::filesystem::permissions(
      temporary, std::filesystem::perms::owner_read |
                     std::filesystem::perms::owner_write,
      std::filesystem::perm_options::replace, filesystem_error);
  filesystem_error.clear();
  std::filesystem::rename(temporary, path, filesystem_error);
  if (filesystem_error) {
    std::filesystem::remove(temporary, filesystem_error);
    *error = "cannot commit runtime metadata: " + filesystem_error.message();
    return false;
  }
  return true;
}

bool EnsureStudioAppSettings(const std::filesystem::path& version_directory,
                             bool* created, std::string* error) {
  if (created != nullptr) *created = false;

  const std::filesystem::path settings =
      version_directory / "AppSettings.xml";
  std::error_code filesystem_error;
  const bool exists = std::filesystem::exists(settings, filesystem_error);
  if (filesystem_error) {
    *error = "cannot inspect Studio AppSettings: " +
             filesystem_error.message();
    return false;
  }
  if (exists) {
    if (!std::filesystem::is_regular_file(settings, filesystem_error) ||
        filesystem_error) {
      *error = "Studio AppSettings is not a regular file: " +
               settings.string();
      return false;
    }
    return true;
  }

  if (!WriteAtomic(settings, kStudioAppSettingsXml, error)) return false;
  if (created != nullptr) *created = true;
  return true;
}

std::filesystem::path RuntimeRoot(const StudioProvisionOptions& options) {
  return options.runtime_root.empty() ? DefaultStudioRuntimeRoot()
                                      : options.runtime_root;
}

}  // namespace

std::filesystem::path DefaultStudioRuntimeRoot() {
  const char* configured = std::getenv("AURORA_STUDIO_RUNTIME_DATA");
  if (configured != nullptr && configured[0] == '/') {
    return std::filesystem::path(configured);
  }
  const char* data = std::getenv("XDG_DATA_HOME");
  if (data != nullptr && data[0] == '/') {
    return std::filesystem::path(data) / "aurora/studio-runtime";
  }
  const char* home = std::getenv("HOME");
  const std::filesystem::path root =
      home != nullptr && home[0] == '/' ? std::filesystem::path(home)
                                        : std::filesystem::path(".");
  return root / ".local/share/aurora/studio-runtime";
}

std::optional<std::filesystem::path> StudioPackageDestination(
    std::string_view package_name) {
  const auto found = PackageMap().find(std::string(package_name));
  if (found == PackageMap().end()) return std::nullopt;
  return std::filesystem::path(found->second);
}

StudioVersionInfo ParseStudioVersionResponse(std::string_view bytes) {
  StudioVersionInfo result;
  const Json document = Json::parse(bytes, nullptr, false);
  if (document.is_discarded()) {
    result.error = "cannot parse Roblox Studio version response";
    return result;
  }
  if (!document.is_object()) {
    result.error = "Roblox Studio version response is not an object";
    return result;
  }
  const auto version = document.find("version");
  const auto upload = document.find("clientVersionUpload");
  if (version == document.end() || !version->is_string() ||
      upload == document.end() || !upload->is_string()) {
    result.error = "Roblox Studio version response is missing identity";
    return result;
  }
  result.version = version->get<std::string>();
  result.upload = upload->get<std::string>();
  if (result.version.empty() || result.version.size() > 128 ||
      !IsSafeUpload(result.upload)) {
    result.error = "Roblox Studio version response has invalid identity";
  }
  return result;
}

StudioPackageManifest ParseStudioPackageManifest(std::string_view bytes) {
  StudioPackageManifest result;
  const std::vector<std::string> lines = Lines(bytes);
  if (lines.empty() || lines[0] != "v0" ||
      (lines.size() - 1U) % 4U != 0U) {
    result.error = "Roblox Studio package manifest has an unsupported layout";
    return result;
  }
  if ((lines.size() - 1U) / 4U > 256U) {
    result.error = "Roblox Studio package manifest has too many packages";
    return result;
  }
  for (std::size_t index = 1; index + 3 < lines.size(); index += 4) {
    StudioPackage package;
    package.name = lines[index];
    package.md5 = lines[index + 1];
    const auto compressed = ParseSize(lines[index + 2]);
    const auto uncompressed = ParseSize(lines[index + 3]);
    const auto destination = StudioPackageDestination(package.name);
    if (!IsSafePackageName(package.name) || !IsHex(package.md5, 32) ||
        !compressed.has_value() || !uncompressed.has_value() ||
        *compressed == 0 || *uncompressed == 0 ||
        *compressed > kMaximumPackageBytes ||
        *uncompressed > kMaximumExpandedPackageBytes) {
      result.error = "Roblox Studio package manifest has an invalid record";
      result.packages.clear();
      return result;
    }
    if (!destination.has_value()) {
      result.error = "Roblox Studio manifest contains an unmapped package: " +
                     package.name;
      result.packages.clear();
      return result;
    }
    package.compressed_bytes = *compressed;
    package.uncompressed_bytes = *uncompressed;
    package.destination = *destination;
    result.packages.push_back(std::move(package));
  }
  if (result.packages.empty()) {
    result.error = "Roblox Studio package manifest is empty";
  }
  return result;
}

StudioVersionInfo FetchLatestStudioVersion() {
  update::HttpTransferRequest request;
  request.url = std::string(kVersionEndpoint);
  request.allowed_hosts = {"clientsettings.roblox.com"};
  request.maximum_bytes = kMaximumVersionResponse;
  request.transfer_timeout_ms = 30000;
  const update::HttpBytesResult downloaded = update::DownloadBytes(request);
  if (!downloaded) return {{}, {}, downloaded.error};
  return ParseStudioVersionResponse(downloaded.bytes);
}

StudioPackageManifest FetchStudioPackageManifest(std::string_view upload) {
  if (!IsSafeUpload(upload)) {
    StudioPackageManifest result;
    result.error = "invalid Roblox Studio upload identity";
    return result;
  }
  update::HttpTransferRequest request;
  request.url = std::string(kSetupBase) + std::string(upload) +
                "-rbxPkgManifest.txt";
  request.allowed_hosts = {"setup.rbxcdn.com"};
  request.maximum_bytes = kMaximumManifestBytes;
  request.transfer_timeout_ms = 30000;
  const update::HttpBytesResult downloaded = update::DownloadBytes(request);
  if (!downloaded) {
    StudioPackageManifest result;
    result.error = downloaded.error;
    return result;
  }
  return ParseStudioPackageManifest(downloaded.bytes);
}

StudioProvisionResult ProvisionLatestStudio(
    const StudioProvisionOptions& options) {
  StudioProvisionResult result;
  const StudioVersionInfo version = FetchLatestStudioVersion();
  if (!version) {
    result.error = version.error;
    return result;
  }
  result.version = version.version;
  result.upload = version.upload;

  const StudioPackageManifest manifest =
      FetchStudioPackageManifest(version.upload);
  if (!manifest) {
    result.error = manifest.error;
    return result;
  }

  const std::filesystem::path root = RuntimeRoot(options);
  const std::filesystem::path versions = root / "versions";
  const std::filesystem::path target = versions / version.upload;
  const std::filesystem::path target_exe = target / "RobloxStudioBeta.exe";
  const std::filesystem::path target_mcp = target / "StudioMCP.exe";
  result.version_directory = target;
  result.studio_executable = target_exe;
  result.studio_mcp = target_mcp;

  std::error_code filesystem_error;
  if (!options.force &&
      std::filesystem::is_regular_file(target_exe, filesystem_error) &&
      !filesystem_error) {
    bool appsettings_created = false;
    std::string appsettings_error;
    if (!EnsureStudioAppSettings(target, &appsettings_created,
                                 &appsettings_error)) {
      result.error = appsettings_error;
      return result;
    }
    std::string write_error;
    if (!WriteAtomic(root / "current-version", version.upload + "\n",
                     &write_error)) {
      result.error = write_error;
      return result;
    }
    result.changed = appsettings_created;
    return result;
  }

  std::filesystem::create_directories(versions, filesystem_error);
  if (filesystem_error) {
    result.error = "cannot create Aurora Studio versions directory: " +
                   filesystem_error.message();
    return result;
  }
  const std::filesystem::path cache = root / "cache/packages";
  std::filesystem::create_directories(cache, filesystem_error);
  if (filesystem_error) {
    result.error = "cannot create Aurora Studio package cache: " +
                   filesystem_error.message();
    return result;
  }

  const std::filesystem::path staging =
      root / ("staging-" + version.upload + "-" + std::to_string(getpid()));
  std::filesystem::remove_all(staging, filesystem_error);
  filesystem_error.clear();
  std::filesystem::create_directories(staging, filesystem_error);
  if (filesystem_error) {
    result.error = "cannot create Aurora Studio staging directory: " +
                   filesystem_error.message();
    return result;
  }

  const auto fail = [&](std::string message) {
    std::error_code cleanup_error;
    std::filesystem::remove_all(staging, cleanup_error);
    result.error = std::move(message);
    return result;
  };

  for (const StudioPackage& package : manifest.packages) {
    const std::filesystem::path archive =
        cache / (package.md5 + ".zip");
    std::string verify_error;
    if (!VerifyPackageFile(package, archive, &verify_error)) {
      std::filesystem::remove(archive, filesystem_error);
      update::HttpTransferRequest request;
      request.url = std::string(kSetupBase) + version.upload + "-" +
                    package.name;
      request.allowed_hosts = {"setup.rbxcdn.com"};
      if (package.compressed_bytes >
          static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return fail("Studio package is too large for this platform");
      }
      request.maximum_bytes =
          static_cast<std::size_t>(package.compressed_bytes);
      request.transfer_timeout_ms = 0;
      request.low_speed_bytes_per_second = 4096;
      request.low_speed_seconds = 90;
      const update::HttpDownloadResult downloaded =
          update::DownloadFile(request, archive, options.progress_fd);
      if (!downloaded) return fail(downloaded.error);
      verify_error.clear();
      if (!VerifyPackageFile(package, archive, &verify_error)) {
        std::filesystem::remove(archive, filesystem_error);
        return fail(verify_error);
      }
    }

    std::size_t extracted_files = 0;
    std::string extract_error;
    if (package.uncompressed_bytes >
        static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
      return fail("expanded Studio package is too large for this platform");
    }
    const std::filesystem::path destination =
        package.destination.empty() ? staging : staging / package.destination;
    if (!update::ExtractWindowsZipArchive(
            archive, destination,
            static_cast<std::size_t>(package.uncompressed_bytes),
            &extracted_files, &extract_error)) {
      return fail("cannot extract " + package.name + ": " + extract_error);
    }
    if (extracted_files == 0) {
      return fail("Studio package extracted no files: " + package.name);
    }
  }

  if (!std::filesystem::is_regular_file(staging / "RobloxStudioBeta.exe",
                                        filesystem_error) ||
      filesystem_error) {
    return fail("installed Studio payload has no RobloxStudioBeta.exe");
  }

  std::string appsettings_error;
  if (!EnsureStudioAppSettings(staging, nullptr, &appsettings_error)) {
    return fail(appsettings_error);
  }

  Json metadata = {
      {"schema_version", 1},
      {"product", "Roblox Studio"},
      {"version", version.version},
      {"upload", version.upload},
      {"source", "Roblox WindowsStudio64 official deployment"},
      {"package_count", manifest.packages.size()},
  };
  std::string metadata_error;
  if (!WriteAtomic(staging / ".aurora-studio-runtime.json",
                   metadata.dump(2) + "\n", &metadata_error)) {
    return fail(metadata_error);
  }

  const std::filesystem::path old =
      root / ("old-" + version.upload + "-" + std::to_string(getpid()));
  if (std::filesystem::exists(target, filesystem_error)) {
    filesystem_error.clear();
    std::filesystem::rename(target, old, filesystem_error);
    if (filesystem_error) {
      return fail("cannot move existing Studio version aside: " +
                  filesystem_error.message());
    }
  }
  filesystem_error.clear();
  std::filesystem::rename(staging, target, filesystem_error);
  if (filesystem_error) {
    if (std::filesystem::exists(old)) {
      std::error_code restore_error;
      std::filesystem::rename(old, target, restore_error);
    }
    return fail("cannot commit Studio version atomically: " +
                filesystem_error.message());
  }
  if (std::filesystem::exists(old)) {
    std::filesystem::remove_all(old, filesystem_error);
  }

  std::string current_error;
  if (!WriteAtomic(root / "current-version", version.upload + "\n",
                   &current_error)) {
    result.error = current_error;
    return result;
  }

  result.changed = true;
  result.version_directory = target;
  result.studio_executable = target / "RobloxStudioBeta.exe";
  result.studio_mcp = target / "StudioMCP.exe";
  return result;
}

}  // namespace aurora::studio
