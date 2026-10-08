// SPDX-License-Identifier: GPL-3.0-only

#include <unistd.h>

#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "studio_runtime.h"

namespace {

using Path = std::filesystem::path;

std::optional<std::string> EnvironmentValue(const char* name) {
  const char* value = std::getenv(name);
  if (value == nullptr || value[0] == '\0') return std::nullopt;
  return std::string(value);
}

Path HomePath() {
  const auto home = EnvironmentValue("HOME");
  return home.has_value() ? Path(*home) : Path();
}

Path ExpandUser(std::string value) {
  if (value.rfind("~/", 0) == 0) return HomePath() / value.substr(2);
  if (value == "~") return HomePath();
  return Path(std::move(value));
}

Path ConfigPath() {
  const auto configured = EnvironmentValue("XDG_CONFIG_HOME");
  const Path root = configured.has_value() ? Path(*configured)
                                           : HomePath() / ".config";
  return root / "aurora/studio.conf";
}

Path AuroraDataPath() {
  const auto configured = EnvironmentValue("XDG_DATA_HOME");
  const Path root = configured.has_value() ? Path(*configured)
                                           : HomePath() / ".local/share";
  return root / "aurora";
}

Path AuroraStudioRuntimePath() {
  const auto configured = EnvironmentValue("AURORA_STUDIO_RUNTIME_DATA");
  return configured.has_value() ? ExpandUser(*configured)
                                : AuroraDataPath() / "studio-runtime";
}

std::string ReadSetting(const Path& path, const char* key) {
  std::ifstream input(path);
  if (!input.is_open()) return {};
  const std::string prefix = std::string(key) + "=";
  std::string line;
  while (std::getline(input, line)) {
    if (line.rfind(prefix, 0) == 0) return line.substr(prefix.size());
  }
  return {};
}

std::string LowerAscii(std::string value) {
  for (char& character : value) {
    character = static_cast<char>(std::tolower(
        static_cast<unsigned char>(character)));
  }
  return value;
}

bool IsExecutable(const Path& path) {
  return !path.empty() && access(path.c_str(), X_OK) == 0;
}

bool IsRegularFile(const Path& path) {
  std::error_code error;
  return !path.empty() && std::filesystem::is_regular_file(path, error) &&
         !error;
}

std::optional<Path> FindOnPath(const std::string& name) {
  const auto configured_path = EnvironmentValue("PATH");
  const std::string search_path =
      configured_path.value_or("/usr/local/bin:/usr/bin:/bin");
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

std::optional<Path> ResolveWine() {
  for (const char* name : {"AURORA_ROBLOX_WINE", "AURORA_WINE"}) {
    if (const auto configured = EnvironmentValue(name); configured.has_value()) {
      const Path path(*configured);
      if (path.has_parent_path()) {
        return IsExecutable(path) ? std::optional<Path>(path)
                                  : std::nullopt;
      }
      return FindOnPath(*configured);
    }
  }

  const std::string saved_wine = ReadSetting(ConfigPath(), "wine");
  if (!saved_wine.empty()) {
    const Path path = ExpandUser(saved_wine);
    if (path.has_parent_path()) {
      if (IsExecutable(path)) return path;
    } else if (const auto on_path = FindOnPath(saved_wine); on_path.has_value()) {
      return on_path;
    }
  }

  const Path default_path = AuroraStudioRuntimePath() / "wine/bin/wine";
  if (IsExecutable(default_path)) return default_path;
  return FindOnPath("wine");
}

std::optional<Path> ResolveStudioMcp() {
  for (const char* name : {"AURORA_ROBLOX_STUDIO_MCP",
                           "AURORA_STUDIO_MCP"}) {
    if (const auto configured = EnvironmentValue(name); configured.has_value()) {
      const Path path(*configured);
      return IsRegularFile(path) ? std::optional<Path>(path) : std::nullopt;
    }
  }

  const Path versions = AuroraStudioRuntimePath() / "versions";

  std::error_code iterator_error;
  std::optional<Path> newest;
  std::filesystem::file_time_type newest_time{};
  for (std::filesystem::directory_iterator iterator(
           versions, std::filesystem::directory_options::skip_permission_denied,
           iterator_error),
       end;
       iterator != end && !iterator_error; iterator.increment(iterator_error)) {
    const Path candidate = iterator->path() / "StudioMCP.exe";
    if (!IsRegularFile(candidate)) continue;
    std::error_code time_error;
    const auto modified = std::filesystem::last_write_time(candidate, time_error);
    if (!newest.has_value() || (!time_error && modified > newest_time)) {
      newest = candidate;
      if (!time_error) newest_time = modified;
    }
  }
  return newest;
}

Path DefaultStudioPrefix() {
  const auto configured_data = EnvironmentValue("XDG_DATA_HOME");
  const Path root = configured_data.has_value()
                        ? Path(*configured_data)
                        : HomePath() / ".local/share";
  return root / "aurora/studio-prefix";
}

bool UseProton() {
  const char* environment = std::getenv("AURORA_STUDIO_RUNTIME");
  const std::string configured =
      environment != nullptr && environment[0] != '\0'
          ? std::string(environment)
          : ReadSetting(ConfigPath(), "runtime");
  if (!configured.empty()) return LowerAscii(configured) == "proton";
  return !ResolveWine().has_value() &&
         aurora::studio::DetectProton().has_value();
}

std::optional<Path> ResolveProton() {
  for (const char* name : {"AURORA_STUDIO_PROTON", "AURORA_PROTON"}) {
    if (const auto configured = EnvironmentValue(name); configured.has_value()) {
      const Path path(*configured);
      if (path.has_parent_path()) {
        const Path expanded = ExpandUser(path.string());
        return IsExecutable(expanded) ? std::optional<Path>(expanded)
                                      : std::nullopt;
      }
      if (const auto on_path = FindOnPath(*configured); on_path.has_value()) {
        return on_path;
      }
    }
  }
  const std::string configured = ReadSetting(ConfigPath(), "proton");
  if (!configured.empty()) {
    const Path path(configured);
    if (path.has_parent_path()) {
      const Path expanded = ExpandUser(path.string());
      if (IsExecutable(expanded)) return expanded;
    } else if (const auto on_path = FindOnPath(configured); on_path.has_value()) {
      return on_path;
    }
  }
  return aurora::studio::DetectProton();
}

Path ResolveProtonCompatData() {
  const auto environment = EnvironmentValue("AURORA_STUDIO_COMPATDATA");
  auto configured_prefix = EnvironmentValue("AURORA_STUDIO_WINEPREFIX");
  if (!configured_prefix.has_value()) {
    configured_prefix = EnvironmentValue("AURORA_ROBLOX_WINEPREFIX");
  }
  const std::string saved_prefix = ReadSetting(ConfigPath(), "prefix");
  const Path candidate = environment.has_value()
                             ? ExpandUser(*environment)
                             : configured_prefix.has_value()
                                   ? ExpandUser(*configured_prefix)
                                   : saved_prefix.empty()
                                         ? Path()
                                         : ExpandUser(saved_prefix);
  if (candidate.empty() || candidate == DefaultStudioPrefix()) {
    return aurora::studio::DefaultProtonCompatData();
  }
  return candidate;
}

void SetDefaultEnvironment(const char* name, const std::string& value) {
  if (std::getenv(name) == nullptr) (void)setenv(name, value.c_str(), 1);
}

void PrintUsage() {
  std::puts("Aurora native Roblox Studio MCP launcher");
  std::puts("");
  std::puts("Usage:");
  std::puts("  aurora_studio_mcp [StudioMCP arguments]");
  std::puts("  aurora_studio_mcp --check");
  std::puts("  aurora_studio_mcp --print-config");
}

void PrintConfig(const Path& self) {
  std::printf(
      "{\n"
      "  \"mcpServers\": {\n"
      "    \"Aurora_Roblox_Studio\": {\n"
      "      \"command\": \"%s\"\n"
      "    }\n"
      "  }\n"
      "}\n",
      self.c_str());
}

int Main(int argc, char* argv[]) {
  Path self;
  char self_buffer[4096] = {};
  const ssize_t self_length =
      readlink("/proc/self/exe", self_buffer, sizeof(self_buffer) - 1);
  if (self_length > 0) {
    self_buffer[self_length] = '\0';
    self = Path(self_buffer);
  }

  bool check = false;
  bool print_config = false;
  for (int index = 1; index < argc; ++index) {
    if (std::strcmp(argv[index], "--help") == 0 ||
        std::strcmp(argv[index], "-h") == 0) {
      PrintUsage();
      return 0;
    }
    if (std::strcmp(argv[index], "--check") == 0) check = true;
    if (std::strcmp(argv[index], "--print-config") == 0) print_config = true;
  }

  if (print_config) {
    PrintConfig(self.empty() ? Path("aurora_studio_mcp") : self);
    return 0;
  }

  const bool use_proton = UseProton();
  const auto runtime = use_proton ? ResolveProton() : ResolveWine();
  if (!runtime.has_value()) {
    std::fprintf(stderr,
                 "Aurora Studio MCP: %s executable was not found. "
                 "Configure Aurora Studio's runtime first.\n",
                 use_proton ? "Proton" : "Wine");
    return 127;
  }
  const auto studio_mcp = ResolveStudioMcp();
  if (!studio_mcp.has_value()) {
    std::fprintf(stderr,
                 "Aurora Studio MCP: StudioMCP.exe was not found. "
                 "Set AURORA_ROBLOX_STUDIO_MCP.\n");
    return 127;
  }

  if (check) {
    std::printf("runtime=%s\n%s=%s\nStudioMCP=%s\n",
                use_proton ? "proton" : "wine",
                use_proton ? "proton" : "wine", runtime->c_str(),
                studio_mcp->c_str());
    return 0;
  }

  Path prefix;
  std::vector<std::string> arguments;
  arguments.reserve(static_cast<std::size_t>(argc) + 4);
  if (use_proton) {
    const Path compatdata = ResolveProtonCompatData();
    prefix = compatdata / "pfx";
    (void)setenv("STEAM_COMPAT_DATA_PATH", compatdata.c_str(), 1);
    (void)setenv("STEAM_COMPAT_CLIENT_INSTALL_PATH",
                 aurora::studio::ProtonClientInstallPath(*runtime).c_str(), 1);
    (void)setenv("WINEPREFIX", prefix.c_str(), 1);
    arguments.push_back(runtime->string());
    arguments.push_back("runinprefix");
  } else {
    auto configured_prefix = EnvironmentValue("AURORA_STUDIO_WINEPREFIX");
    if (!configured_prefix.has_value()) {
      configured_prefix = EnvironmentValue("AURORA_ROBLOX_WINEPREFIX");
    }
    prefix = configured_prefix.has_value() ? Path(*configured_prefix)
                                           : DefaultStudioPrefix();
    (void)setenv("WINEPREFIX", prefix.c_str(), 1);
    arguments.push_back(runtime->string());
  }
  SetDefaultEnvironment("WINEDEBUG", "-all");
  SetDefaultEnvironment("WINELOADERNOEXEC", "1");

  // Do not keep a shell or supervisor resident. The MCP host owns this
  // stdio transport and can restart it when StudioMCP exits.
  arguments.push_back(studio_mcp->string());
  for (int index = 1; index < argc; ++index) {
    if (std::strcmp(argv[index], "--check") == 0 ||
        std::strcmp(argv[index], "--print-config") == 0) {
      continue;
    }
    arguments.emplace_back(argv[index]);
  }

  std::vector<char*> child_argv;
  child_argv.reserve(arguments.size() + 1);
  for (std::string& argument : arguments) child_argv.push_back(argument.data());
  child_argv.push_back(nullptr);
  execv(runtime->c_str(), child_argv.data());
  const int error_number = errno;
  std::fprintf(stderr, "Aurora Studio MCP: cannot exec runtime: %s\n",
               std::strerror(error_number));
  return 127;
}

}  // namespace

int main(int argc, char* argv[]) { return Main(argc, argv); }
