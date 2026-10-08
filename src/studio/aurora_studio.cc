// SPDX-License-Identifier: GPL-3.0-only

#include <adwaita.h>

#include <dlfcn.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include <vulkan/vulkan.h>

#include "runtime/discord_rpc.h"
#include "runtime/managed_process.h"
#include "runtime/single_instance_lock.h"
#include "studio/kombucha_provisioner.h"
#include "studio/studio_launch_settings.h"
#include "studio_runtime.h"
#include "wine_theme.h"

namespace {

struct State {
  GtkWindow* window = nullptr;
  GtkEntry* executable = nullptr;
  GtkEntry* wine = nullptr;
  GtkSwitch* runtime = nullptr;
  GtkEntry* proton = nullptr;
  GtkEntry* prefix = nullptr;
  GtkEntry* place = nullptr;
  GtkEntry* memory_limit = nullptr;
  GtkDropDown* graphics_mode = nullptr;
  GtkDropDown* graphics_card = nullptr;
  std::vector<aurora::studio::GraphicsAdapterChoice> graphics_adapters;
  GtkSwitch* discord_activity = nullptr;
  GtkLabel* status = nullptr;
  GMainLoop* loop = nullptr;
  guint poll_source = 0;
  guint theme_poll_source = 0;
  bool has_theme_snapshot = false;
  bool dark_theme = false;
  bool settings_mode = false;
  // A browser callback or a .rbxl/.rbxm file can arrive as the single
  // desktop-entry argument. Keep the original URI for auth callbacks and
  // pass a decoded filesystem path for Roblox place/model files.
  std::string launch_target;
  bool launch_target_is_file = false;
  bool forwarded_launch = false;
  std::size_t memory_limit_bytes = 0;
  std::chrono::steady_clock::time_point next_memory_check;
  std::optional<aurora::runtime::ManagedProcess> process;
  std::optional<aurora::runtime::SingleInstanceLock> studio_lock;
  std::unique_ptr<aurora::runtime::DiscordRpcSession> discord_presence;
  std::filesystem::path config;
};

std::vector<aurora::studio::GraphicsAdapterChoice> EnumerateGraphicsAdapters() {
  std::vector<aurora::studio::GraphicsAdapterChoice> adapters;
  void* loader = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
  if (loader == nullptr) return adapters;

  const auto create_instance = reinterpret_cast<PFN_vkCreateInstance>(
      dlsym(loader, "vkCreateInstance"));
  const auto enumerate_devices =
      reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
          dlsym(loader, "vkEnumeratePhysicalDevices"));
  const auto get_properties =
      reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
          dlsym(loader, "vkGetPhysicalDeviceProperties"));
  const auto destroy_instance = reinterpret_cast<PFN_vkDestroyInstance>(
      dlsym(loader, "vkDestroyInstance"));
  if (create_instance == nullptr || enumerate_devices == nullptr ||
      get_properties == nullptr || destroy_instance == nullptr) {
    dlclose(loader);
    return adapters;
  }

  VkApplicationInfo app_info{};
  app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app_info.pApplicationName = "Aurora Studio Settings";
  app_info.applicationVersion = 1;
  app_info.pEngineName = "Aurora";
  app_info.engineVersion = 1;
  app_info.apiVersion = VK_API_VERSION_1_0;
  VkInstanceCreateInfo create_info{};
  create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  create_info.pApplicationInfo = &app_info;
  VkInstance instance = VK_NULL_HANDLE;
  if (create_instance(&create_info, nullptr, &instance) != VK_SUCCESS) {
    dlclose(loader);
    return adapters;
  }

  std::uint32_t count = 0;
  if (enumerate_devices(instance, &count, nullptr) == VK_SUCCESS && count > 0) {
    std::vector<VkPhysicalDevice> devices(count);
    if (enumerate_devices(instance, &count, devices.data()) == VK_SUCCESS) {
      devices.resize(count);
      for (const VkPhysicalDevice device : devices) {
        VkPhysicalDeviceProperties properties{};
        get_properties(device, &properties);
        if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ||
            properties.deviceName[0] == '\0') {
          continue;
        }
        const std::string name(properties.deviceName);
        const bool duplicate = std::any_of(
            adapters.begin(), adapters.end(),
            [&name, &properties](const auto& adapter) {
              return adapter.name == name &&
                     adapter.vendor_id == properties.vendorID &&
                     adapter.device_id == properties.deviceID;
            });
        if (!duplicate) {
          adapters.push_back({name, properties.vendorID, properties.deviceID});
        }
      }
    }
  }

  destroy_instance(instance, nullptr);
  dlclose(loader);
  std::sort(adapters.begin(), adapters.end(),
            [](const auto& first, const auto& second) {
              return first.name < second.name;
            });
  return adapters;
}

std::filesystem::path HomePath() {
  const char* home = std::getenv("HOME");
  return home == nullptr || home[0] == '\0' ? std::filesystem::path(".")
                                             : std::filesystem::path(home);
}

std::filesystem::path ConfigPath() {
  const char* configured = std::getenv("XDG_CONFIG_HOME");
  const std::filesystem::path root =
      configured != nullptr && configured[0] == '/' ?
          std::filesystem::path(configured) : HomePath() / ".config";
  return root / "aurora/studio.conf";
}

std::filesystem::path ExpandUser(std::string value) {
  if (value.rfind("~/", 0) == 0) {
    return HomePath() / value.substr(2);
  }
  if (value == "~") return HomePath();
  return std::filesystem::path(std::move(value));
}

std::filesystem::path AuroraDataPath() {
  const char* configured = std::getenv("XDG_DATA_HOME");
  const std::filesystem::path root =
      configured != nullptr && configured[0] == '/' ?
          std::filesystem::path(configured) : HomePath() / ".local/share";
  return root / "aurora";
}

std::filesystem::path AuroraStudioRuntimePath() {
  const char* configured = std::getenv("AURORA_STUDIO_RUNTIME_DATA");
  if (configured != nullptr && configured[0] != '\0') {
    return ExpandUser(configured);
  }
  return AuroraDataPath() / "studio-runtime";
}

std::optional<std::filesystem::path> NewestStudioExecutable() {
  const std::filesystem::path versions =
      AuroraStudioRuntimePath() / "versions";
  std::error_code iterator_error;
  std::optional<std::filesystem::path> newest;
  std::filesystem::file_time_type newest_time{};
  for (std::filesystem::directory_iterator iterator(
           versions, std::filesystem::directory_options::skip_permission_denied,
           iterator_error),
       end;
       iterator != end && !iterator_error; iterator.increment(iterator_error)) {
    const std::filesystem::path candidate =
        iterator->path() / "RobloxStudioBeta.exe";
    std::error_code file_error;
    if (!std::filesystem::is_regular_file(candidate, file_error) ||
        file_error) {
      continue;
    }
    std::error_code time_error;
    const auto modified = std::filesystem::last_write_time(candidate, time_error);
    if (!newest.has_value() || (!time_error && modified > newest_time)) {
      newest = candidate;
      if (!time_error) newest_time = modified;
    }
  }
  return newest;
}

std::filesystem::path DefaultWine() {
  // Aurora owns its Studio Wine runtime. First launch provisions this path
  // from Namnarak/AuroraWine instead of borrowing Vinegar's runtime.
  return aurora::studio::DefaultManagedKombuchaWine();
}

bool IsLegacyVinegarWine(const std::string& value) {
  return value.find("/.local/share/vinegar/kombucha") != std::string::npos ||
         value.find("/.local/share/vinegar/kombucha-proton-") !=
             std::string::npos;
}

std::filesystem::path LegacyDefaultPrefix() {
  return AuroraDataPath() / "studio-prefix";
}

std::filesystem::path DefaultPrefix() {
  // Keep Aurora-managed Kombucha on its own prefix. Older Aurora builds used
  // studio-prefix with Proton/Kombucha variants whose registry layout can
  // deadlock a newer stable Wine during wineboot/reg import.
  return AuroraDataPath() / "studio-prefix-kombucha";
}

bool IsProtonSelected(const State* state) {
  return state != nullptr && state->runtime != nullptr &&
         gtk_switch_get_active(state->runtime);
}

const char* SelectedRuntimeName(const State* state) {
  return IsProtonSelected(state) ? "proton" : "wine";
}

std::size_t ParseMemoryLimitMb(const std::string& value,
                               std::size_t fallback) {
  if (value.empty()) return fallback;
  errno = 0;
  char* end = nullptr;
  const unsigned long long parsed = std::strtoull(value.c_str(), &end, 10);
  if (errno != 0 || end == value.c_str() || *end != '\0') return fallback;
  if (parsed > 65536) return fallback;
  return static_cast<std::size_t>(parsed);
}

std::size_t MemoryLimitBytes(const State* state) {
  const std::string configured =
      state == nullptr || state->memory_limit == nullptr
          ? ""
          : gtk_editable_get_text(GTK_EDITABLE(state->memory_limit));
  const std::size_t megabytes = ParseMemoryLimitMb(configured, 0);
  if (megabytes == 0 ||
      megabytes > std::numeric_limits<std::size_t>::max() / (1024 * 1024)) {
    return 0;
  }
  return megabytes * 1024 * 1024;
}

void SetEntry(GtkEntry* entry, const std::string& value);

bool IsDefaultStudioPrefix(const std::filesystem::path& path) {
  return path.empty() || path == DefaultPrefix();
}

void UpdateRuntimeFields(State* state) {
  if (state == nullptr || state->prefix == nullptr) return;
  const bool proton = IsProtonSelected(state);
  gtk_entry_set_placeholder_text(
      state->prefix,
      proton ? "~/.local/share/aurora/studio-proton-compatdata"
             : "~/.local/share/aurora/studio-prefix");

  const std::filesystem::path current = ExpandUser(
      gtk_editable_get_text(GTK_EDITABLE(state->prefix)));
  if (IsDefaultStudioPrefix(current)) {
    SetEntry(state->prefix,
             (proton ? aurora::studio::DefaultProtonCompatData()
                     : DefaultPrefix())
                 .string());
  } else if (!proton && current == aurora::studio::DefaultProtonCompatData()) {
    SetEntry(state->prefix, DefaultPrefix().string());
  }
}

void RuntimeChanged(GtkSwitch*, GParamSpec*, gpointer data) {
  UpdateRuntimeFields(static_cast<State*>(data));
}

std::filesystem::path StudioLockPath() {
  const char* runtime = std::getenv("XDG_RUNTIME_DIR");
  if (runtime != nullptr && runtime[0] == '/') {
    return std::filesystem::path(runtime) / "aurora-studio.instance.lock";
  }
  const char* state = std::getenv("XDG_STATE_HOME");
  const std::filesystem::path root =
      state != nullptr && state[0] == '/' ? std::filesystem::path(state)
                                          : HomePath() / ".local/state";
  return root / "aurora/studio.instance.lock";
}

bool IsWindowsExecutable(const std::filesystem::path& path) {
  const std::string value = path.extension().string();
  return value == ".exe" || value == ".EXE";
}

bool IsStudioLaunchUri(const char* value) {
  if (value == nullptr) return false;
  const std::string uri(value);
  return uri.rfind("roblox-studio-auth:", 0) == 0 ||
         uri.rfind("roblox-studio:", 0) == 0;
}

bool IsSupportedStudioFile(const std::filesystem::path& path) {
  std::string extension = path.extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  return extension == ".rbxl" || extension == ".rbxlx" ||
         extension == ".rbxm" || extension == ".rbxmx";
}

std::optional<std::filesystem::path> DecodeFileUri(const std::string& value) {
  GError* error = nullptr;
  gchar* filename = g_filename_from_uri(value.c_str(), nullptr, &error);
  if (filename == nullptr) {
    if (error != nullptr) g_error_free(error);
    return std::nullopt;
  }
  std::filesystem::path path(filename);
  g_free(filename);
  if (!IsSupportedStudioFile(path)) return std::nullopt;
  return path;
}

std::string FindStudioLaunchTarget(int argc, char* argv[],
                                   bool* is_file) {
  if (is_file != nullptr) *is_file = false;
  for (int index = 1; index < argc; ++index) {
    if (IsStudioLaunchUri(argv[index])) return argv[index];
    const std::string argument = argv[index] == nullptr ? "" : argv[index];
    if (argument.rfind("file:", 0) == 0) {
      const auto path = DecodeFileUri(argument);
      if (path.has_value()) {
        if (is_file != nullptr) *is_file = true;
        return path->string();
      }
      continue;
    }
    const std::filesystem::path path(argument);
    if (IsSupportedStudioFile(path)) {
      if (is_file != nullptr) *is_file = true;
      return path.string();
    }
  }
  return {};
}

std::string ReadSetting(const std::filesystem::path& path,
                        const std::string& key) {
  return aurora::studio::ReadConfigSetting(path, key);
}

void SetEntry(GtkEntry* entry, const std::string& value) {
  if (entry != nullptr && !value.empty()) gtk_editable_set_text(
      GTK_EDITABLE(entry), value.c_str());
}

constexpr std::array<aurora::studio::StudioGraphicsMode, 5>
    kGraphicsModes = {
        aurora::studio::StudioGraphicsMode::kWineD3d,
        aurora::studio::StudioGraphicsMode::kWineD3dLegacy,
        aurora::studio::StudioGraphicsMode::kDxvk,
        aurora::studio::StudioGraphicsMode::kDxvkLegacy,
        aurora::studio::StudioGraphicsMode::kVulkan,
    };

aurora::studio::StudioGraphicsMode SelectedGraphicsMode(const State* state) {
  if (state == nullptr || state->graphics_mode == nullptr) {
    return aurora::studio::StudioGraphicsMode::kDxvk;
  }
  const guint selected = gtk_drop_down_get_selected(state->graphics_mode);
  return selected < kGraphicsModes.size()
             ? kGraphicsModes[selected]
             : aurora::studio::StudioGraphicsMode::kDxvk;
}

void SetGraphicsMode(State* state, std::string_view value) {
  if (state == nullptr || state->graphics_mode == nullptr) return;
  const auto mode = aurora::studio::ParseGraphicsMode(value);
  if (!mode.has_value()) return;
  const auto found = std::find(kGraphicsModes.begin(), kGraphicsModes.end(),
                               *mode);
  if (found != kGraphicsModes.end()) {
    gtk_drop_down_set_selected(
        state->graphics_mode,
        static_cast<guint>(std::distance(kGraphicsModes.begin(), found)));
  }
}

const aurora::studio::GraphicsAdapterChoice* SelectedGraphicsAdapter(
    const State* state) {
  if (state == nullptr || state->graphics_card == nullptr) return nullptr;
  const guint selected = gtk_drop_down_get_selected(state->graphics_card);
  if (selected == GTK_INVALID_LIST_POSITION || selected == 0 ||
      selected > state->graphics_adapters.size()) {
    return nullptr;
  }
  return &state->graphics_adapters[selected - 1];
}

std::string SelectedGraphicsAdapterName(const State* state) {
  const auto* adapter = SelectedGraphicsAdapter(state);
  return adapter == nullptr ? std::string() : adapter->name;
}

void SetGraphicsAdapter(State* state, std::string_view name) {
  if (state == nullptr || state->graphics_card == nullptr || name.empty()) {
    return;
  }
  for (std::size_t index = 0; index < state->graphics_adapters.size(); ++index) {
    if (state->graphics_adapters[index].name == name) {
      gtk_drop_down_set_selected(state->graphics_card,
                                 static_cast<guint>(index + 1));
      return;
    }
  }
}

void StopStudioDiscordPresence(State* state) {
  if (state == nullptr || state->discord_presence == nullptr) return;
  state->discord_presence->Stop();
  state->discord_presence.reset();
}

void StartStudioDiscordPresence(State* state) {
  if (state == nullptr || state->discord_activity == nullptr ||
      !gtk_switch_get_active(state->discord_activity) ||
      AURORA_STUDIO_DISCORD_APPLICATION_ID[0] == '\0') {
    return;
  }

  aurora::runtime::DiscordRpcConfig config;
  config.enabled = true;
  config.application_id = AURORA_STUDIO_DISCORD_APPLICATION_ID;
  config.show_elapsed_time = false;
  config.join_enabled = false;
  config.text.browsing = "In the Roblox Studio app";
  config.text.state.clear();
  state->discord_presence =
      std::make_unique<aurora::runtime::DiscordRpcSession>(std::move(config));
  std::string detail;
  if (!state->discord_presence->Start(&detail)) {
    g_warning("[aurora-studio] Discord Rich Presence unavailable: %s",
              detail.c_str());
    StopStudioDiscordPresence(state);
    return;
  }
  g_message("[aurora-studio] Discord Rich Presence started");
}

void LoadConfig(State* state) {
  const std::string configured_executable =
      ReadSetting(state->config, "executable");
  const std::string configured_wine = ReadSetting(state->config, "wine");
  const std::string configured_runtime = ReadSetting(state->config, "runtime");
  const std::string configured_proton = ReadSetting(state->config, "proton");
  const std::string configured_prefix = ReadSetting(state->config, "prefix");
  const std::string configured_place = ReadSetting(state->config, "place");
  const std::string configured_memory_limit =
      ReadSetting(state->config, "memory_limit_mb");
  const std::string configured_graphics_mode =
      ReadSetting(state->config,
                  std::string(aurora::studio::kGraphicsModeSetting));
  const std::string configured_graphics_card =
      ReadSetting(state->config,
                  std::string(aurora::studio::kGraphicsCardSetting));
  const std::string legacy_dxvk_device_filter = ReadSetting(
      state->config,
      std::string(aurora::studio::kDxvkDeviceFilterSetting));
  const std::string configured_discord_activity =
      ReadSetting(state->config, "discord_rpc_enabled");
  const char* environment_executable = std::getenv("AURORA_STUDIO_EXE");
  const char* environment_runtime = std::getenv("AURORA_STUDIO_RUNTIME");
  const char* environment_wine = std::getenv("AURORA_ROBLOX_WINE");
  if (environment_wine == nullptr || environment_wine[0] == '\0') {
    environment_wine = std::getenv("AURORA_WINE");
  }
  const char* environment_proton = std::getenv("AURORA_STUDIO_PROTON");
  if (environment_proton == nullptr || environment_proton[0] == '\0') {
    environment_proton = std::getenv("AURORA_PROTON");
  }
  const char* environment_prefix = std::getenv("AURORA_STUDIO_WINEPREFIX");
  if (environment_prefix == nullptr || environment_prefix[0] == '\0') {
    environment_prefix = std::getenv("AURORA_ROBLOX_WINEPREFIX");
  }
  const char* environment_memory_limit =
      std::getenv("AURORA_STUDIO_MEMORY_LIMIT_MB");
  const auto detected_executable = NewestStudioExecutable();
  const std::string detected_executable_value =
      detected_executable.has_value() ? detected_executable->string() : "";
  const std::string detected_prefix = DefaultPrefix().string();
  const bool has_environment_prefix =
      environment_prefix != nullptr && environment_prefix[0] != '\0';
  SetEntry(state->executable,
           environment_executable != nullptr && environment_executable[0] != '\0'
               ? environment_executable
               : configured_executable.empty() ? detected_executable_value
                                               : configured_executable);
  const auto detected_proton = aurora::studio::DetectProton();
  const std::filesystem::path default_wine = DefaultWine();
  const std::string runtime_value =
      environment_runtime != nullptr && environment_runtime[0] != '\0'
          ? environment_runtime
          : configured_runtime;
  // Managed Kombucha is the default even before it is installed; Launch()
  // provisions it on first use. Proton stays an explicit opt-in fallback.
  gtk_switch_set_active(
      state->runtime,
      runtime_value == "proton" || runtime_value == "Proton");
  const bool migrate_vinegar_wine =
      environment_wine == nullptr && IsLegacyVinegarWine(configured_wine);
  SetEntry(state->wine,
           environment_wine != nullptr && environment_wine[0] != '\0'
               ? environment_wine
               : configured_wine.empty() || migrate_vinegar_wine
                     ? default_wine.string()
                     : configured_wine);
  SetEntry(state->proton,
           environment_proton != nullptr && environment_proton[0] != '\0'
               ? environment_proton
               : configured_proton.empty()
                     ? detected_proton.has_value() ? detected_proton->string()
                                                   : ""
                     : configured_proton);
  const bool migrate_legacy_prefix =
      !has_environment_prefix &&
      ExpandUser(configured_prefix) == LegacyDefaultPrefix() &&
      !(runtime_value == "proton" || runtime_value == "Proton");
  SetEntry(state->prefix,
           has_environment_prefix
               ? environment_prefix
               : configured_prefix.empty() || migrate_legacy_prefix
                     ? detected_prefix
                     : configured_prefix);
  SetEntry(state->place, configured_place);
  SetEntry(state->memory_limit,
           environment_memory_limit != nullptr &&
                   environment_memory_limit[0] != '\0'
               ? environment_memory_limit
               : configured_memory_limit.empty() ? "0"
                                                 : configured_memory_limit);
  SetGraphicsMode(state, configured_graphics_mode);
  SetGraphicsAdapter(state, configured_graphics_card.empty()
                                ? legacy_dxvk_device_filter
                                : configured_graphics_card);
  gtk_switch_set_active(state->discord_activity,
                        configured_discord_activity == "true" ||
                            configured_discord_activity == "1");
  UpdateRuntimeFields(state);
}

bool SaveConfig(State* state, std::string* error) {
  const auto graphics_mode = SelectedGraphicsMode(state);
  const std::string selected_adapter = SelectedGraphicsAdapterName(state);
  const auto dxvk_filter_setting = aurora::studio::FormatConfigSetting(
      aurora::studio::kDxvkDeviceFilterSetting,
      aurora::studio::UsesDxvk(graphics_mode) ? selected_adapter : "");
  const auto graphics_mode_setting = aurora::studio::FormatConfigSetting(
      aurora::studio::kGraphicsModeSetting,
      aurora::studio::GraphicsModeConfigValue(graphics_mode));
  const auto graphics_card_setting = aurora::studio::FormatConfigSetting(
      aurora::studio::kGraphicsCardSetting, selected_adapter);
  if (!dxvk_filter_setting.has_value() ||
      !graphics_mode_setting.has_value() ||
      !graphics_card_setting.has_value()) {
    *error = "Graphics card name cannot contain line breaks";
    return false;
  }

  std::error_code filesystem_error;
  std::filesystem::create_directories(state->config.parent_path(),
                                       filesystem_error);
  if (filesystem_error) {
    *error = "cannot create Aurora Studio config directory: " +
             filesystem_error.message();
    return false;
  }
  const std::filesystem::path temporary =
      state->config.string() + ".tmp-" + std::to_string(getpid());
  std::ofstream output(temporary, std::ios::trunc);
  if (!output) {
    *error = "cannot write Aurora Studio config";
    return false;
  }
  output << "executable=" << gtk_editable_get_text(GTK_EDITABLE(state->executable))
         << '\n'
         << "runtime=" << SelectedRuntimeName(state) << '\n'
         << "wine=" << gtk_editable_get_text(GTK_EDITABLE(state->wine)) << '\n'
         << "proton=" << gtk_editable_get_text(GTK_EDITABLE(state->proton))
         << '\n'
         << "prefix=" << gtk_editable_get_text(GTK_EDITABLE(state->prefix))
         << '\n'
         << "place=" << gtk_editable_get_text(GTK_EDITABLE(state->place)) << '\n';
  output << "memory_limit_mb="
         << gtk_editable_get_text(GTK_EDITABLE(state->memory_limit)) << '\n'
         << *graphics_mode_setting << '\n'
         << *graphics_card_setting << '\n'
         << *dxvk_filter_setting << '\n'
         << "discord_rpc_enabled="
         << (gtk_switch_get_active(state->discord_activity) ? "true" : "false")
         << '\n';
  output.close();
  if (!output) {
    std::filesystem::remove(temporary, filesystem_error);
    *error = "cannot finish writing Aurora Studio config";
    return false;
  }
  chmod(temporary.c_str(), 0600);
  std::filesystem::rename(temporary, state->config, filesystem_error);
  if (filesystem_error) {
    std::filesystem::remove(temporary, filesystem_error);
    *error = "cannot commit Aurora Studio config: " +
             filesystem_error.message();
    return false;
  }
  return true;
}

void SetStatus(State* state, const std::string& value) {
  gtk_label_set_text(state->status, value.c_str());
  // URL/file launches are background desktop actions. Only surface the
  // settings window when a background launch failed before a child was
  // created. A normal launcher click is also a background launch; settings
  // mode is reserved for the desktop action below.
  if ((!state->settings_mode || !state->launch_target.empty()) &&
      !state->process.has_value()) {
    gtk_window_present(state->window);
  }
}

std::optional<std::size_t> ProcKilobytes(const std::string& line) {
  const std::size_t separator = line.find(':');
  if (separator == std::string::npos) return std::nullopt;
  std::istringstream input(line.substr(separator + 1));
  unsigned long long value = 0;
  input >> value;
  if (!input || value >
                    std::numeric_limits<std::size_t>::max() / 1024) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(value) * 1024;
}

std::optional<std::pair<pid_t, std::size_t>> ReadProcessGroupMemory(
    const std::filesystem::path& process_directory) {
  const std::filesystem::path stat_path = process_directory / "stat";
  std::ifstream stat_file(stat_path);
  std::string stat_line;
  if (!stat_file.is_open() || !std::getline(stat_file, stat_line)) {
    return std::nullopt;
  }
  const std::size_t comm_end = stat_line.rfind(')');
  if (comm_end == std::string::npos) return std::nullopt;
  std::istringstream stat_fields(stat_line.substr(comm_end + 1));
  char process_state = 0;
  long long parent_pid = 0;
  long long process_group = 0;
  if (!(stat_fields >> process_state >> parent_pid >> process_group) ||
      process_group <= 0 || process_group > std::numeric_limits<pid_t>::max()) {
    return std::nullopt;
  }

  std::ifstream status_file(process_directory / "status");
  if (!status_file.is_open()) return std::nullopt;
  std::size_t memory = 0;
  std::string line;
  while (std::getline(status_file, line)) {
    if (line.rfind("VmRSS:", 0) != 0 && line.rfind("VmSwap:", 0) != 0) {
      continue;
    }
    const auto bytes = ProcKilobytes(line);
    if (!bytes.has_value()) continue;
    if (memory > std::numeric_limits<std::size_t>::max() - *bytes) {
      memory = std::numeric_limits<std::size_t>::max();
      break;
    }
    memory += *bytes;
  }
  return std::make_pair(static_cast<pid_t>(process_group), memory);
}

std::optional<std::size_t> ProcessGroupMemoryBytes(pid_t process_group) {
  if (process_group <= 0) return std::nullopt;
  std::error_code iterator_error;
  std::filesystem::directory_iterator iterator(
      "/proc", std::filesystem::directory_options::skip_permission_denied,
      iterator_error);
  std::size_t total = 0;
  bool found = false;
  for (const std::filesystem::directory_iterator end; iterator != end &&
                                                        !iterator_error;
       iterator.increment(iterator_error)) {
    const std::string name = iterator->path().filename().string();
    if (name.empty() ||
        !std::all_of(name.begin(), name.end(), [](unsigned char character) {
          return std::isdigit(character) != 0;
        })) {
      continue;
    }
    const auto result = ReadProcessGroupMemory(iterator->path());
    if (!result.has_value() || result->first != process_group) continue;
    found = true;
    if (total > std::numeric_limits<std::size_t>::max() - result->second) {
      total = std::numeric_limits<std::size_t>::max();
      break;
    }
    total += result->second;
  }
  return found ? std::optional<std::size_t>(total) : std::nullopt;
}

std::optional<std::size_t> DedicatedCgroupMemoryBytes(pid_t pid) {
  if (pid <= 0) return std::nullopt;
  std::ifstream cgroup_file(std::filesystem::path("/proc") /
                            std::to_string(pid) / "cgroup");
  if (!cgroup_file.is_open()) return std::nullopt;

  std::string line;
  std::string relative_path;
  while (std::getline(cgroup_file, line)) {
    if (line.rfind("0::", 0) == 0) {
      relative_path = line.substr(3);
      break;
    }
  }
  // Desktop-launched applications get their own systemd app unit. A
  // terminal/session cgroup also contains unrelated processes, so do not use
  // it as a Studio limit or we could terminate Studio because of the shell.
  if (relative_path.find("/app.slice/") == std::string::npos ||
      relative_path.find("..") != std::string::npos) {
    return std::nullopt;
  }
  if (!relative_path.empty() && relative_path.front() == '/') {
    relative_path.erase(0, 1);
  }
  const std::filesystem::path cgroup_root =
      std::filesystem::path("/sys/fs/cgroup") / relative_path;
  auto ReadCounter = [](const std::filesystem::path& path)
      -> std::optional<std::size_t> {
    std::ifstream input(path);
    unsigned long long value = 0;
    input >> value;
    if (!input || value > std::numeric_limits<std::size_t>::max()) {
      return std::nullopt;
    }
    return static_cast<std::size_t>(value);
  };
  const auto current = ReadCounter(cgroup_root / "memory.current");
  if (!current.has_value()) return std::nullopt;
  const auto swap = ReadCounter(cgroup_root / "memory.swap.current");
  if (!swap.has_value() ||
      *current > std::numeric_limits<std::size_t>::max() - *swap) {
    return *current;
  }
  return *current + *swap;
}

bool StopStudioForMemory(State* state, std::size_t used,
                         std::size_t limit) {
  if (state == nullptr || !state->process.has_value()) return false;
  std::string ignored;
  (void)state->process->Terminate(std::chrono::milliseconds(500), &ignored);
  state->process.reset();
  StopStudioDiscordPresence(state);
  state->studio_lock.reset();
  state->forwarded_launch = false;
  state->poll_source = 0;
  const std::size_t used_mb = used / (1024 * 1024);
  const std::size_t limit_mb = limit / (1024 * 1024);
  SetStatus(state, "Roblox Studio stopped before system OOM (" +
                       std::to_string(used_mb) + " / " +
                       std::to_string(limit_mb) + " MiB)");
  return true;
}

gboolean PollStudio(gpointer data) {
  auto* state = static_cast<State*>(data);
  if (!state->process.has_value() || !state->process->running()) {
    StopStudioDiscordPresence(state);
    state->poll_source = 0;
    return G_SOURCE_REMOVE;
  }
  const auto now = std::chrono::steady_clock::now();
  if (state->memory_limit_bytes > 0 && now >= state->next_memory_check) {
    state->next_memory_check = now + std::chrono::seconds(1);
    const auto cgroup_memory =
        DedicatedCgroupMemoryBytes(state->process->pid());
    const auto process_memory =
        ProcessGroupMemoryBytes(state->process->pid());
    const auto& memory = cgroup_memory.has_value() ? cgroup_memory
                                                   : process_memory;
    if (memory.has_value() && *memory > state->memory_limit_bytes) {
      StopStudioForMemory(state, *memory, state->memory_limit_bytes);
      return G_SOURCE_REMOVE;
    }
  }
  std::string error;
  const std::optional<int> status = state->process->Poll(&error);
  if (!error.empty()) {
    SetStatus(state, error);
    state->process.reset();
    StopStudioDiscordPresence(state);
    state->poll_source = 0;
    return G_SOURCE_REMOVE;
  }
  if (!status.has_value()) return G_SOURCE_CONTINUE;
  if (WIFEXITED(*status)) {
    SetStatus(state, "Roblox Studio exited with code " +
                         std::to_string(WEXITSTATUS(*status)));
  } else if (WIFSIGNALED(*status)) {
    SetStatus(state, "Roblox Studio stopped by signal " +
                         std::to_string(WTERMSIG(*status)));
  } else {
    SetStatus(state, "Roblox Studio exited");
  }
  const bool forwarded_launch = state->forwarded_launch;
  state->process.reset();
  StopStudioDiscordPresence(state);
  state->studio_lock.reset();
  state->forwarded_launch = false;
  state->poll_source = 0;
  if (forwarded_launch) {
    // A second desktop callback only exists to hand the URI/file to the
    // already-running Studio workflow. Do not leave a hidden Aurora helper
    // around after Roblox has accepted or rejected that argument.
    g_main_loop_quit(state->loop);
  }
  return G_SOURCE_REMOVE;
}

gboolean PollHostTheme(gpointer data) {
  auto* state = static_cast<State*>(data);
  const aurora::studio::HostThemeResolution theme =
      aurora::studio::ResolveHostTheme();
  if (!state->has_theme_snapshot || state->dark_theme != theme.dark) {
    state->has_theme_snapshot = true;
    state->dark_theme = theme.dark;
    adw_style_manager_set_color_scheme(
        adw_style_manager_get_default(),
        theme.dark ? ADW_COLOR_SCHEME_FORCE_DARK
                   : ADW_COLOR_SCHEME_FORCE_LIGHT);
  }
  return G_SOURCE_CONTINUE;
}

bool AcquireStudioLock(State* state, std::string* error,
                       bool* already_running) {
  if (already_running != nullptr) *already_running = false;
  if (state->studio_lock.has_value() && state->studio_lock->acquired()) {
    return true;
  }
  const std::filesystem::path lock_path = StudioLockPath();
  std::error_code filesystem_error;
  std::filesystem::create_directories(lock_path.parent_path(),
                                      filesystem_error);
  if (filesystem_error) {
    *error = "cannot create Aurora Studio lock directory: " +
             filesystem_error.message();
    return false;
  }
  auto lock = aurora::runtime::SingleInstanceLock::Acquire(lock_path);
  if (!lock.acquired()) {
    if (already_running != nullptr) *already_running = lock.already_running();
    *error = lock.already_running()
                 ? "Roblox Studio is already running from Aurora"
                 : lock.error();
    return false;
  }
  state->studio_lock.emplace(std::move(lock));
  return true;
}

void Launch(GtkButton*, gpointer data) {
  auto* state = static_cast<State*>(data);
  if (state->process.has_value() && state->process->running()) {
    SetStatus(state, "Aurora Studio is already running");
    return;
  }
  std::string save_error;
  if (!SaveConfig(state, &save_error)) {
    SetStatus(state, save_error);
    return;
  }
  state->memory_limit_bytes = MemoryLimitBytes(state);
  state->next_memory_check = std::chrono::steady_clock::now();
  const std::filesystem::path executable = ExpandUser(
      gtk_editable_get_text(GTK_EDITABLE(state->executable)));
  if (executable.empty()) {
    SetStatus(state, "เลือก RobloxStudioBeta.exe ก่อน");
    return;
  }
  if (IsWindowsExecutable(executable)) {
    std::error_code filesystem_error;
    if (!std::filesystem::is_regular_file(executable, filesystem_error)) {
      SetStatus(state, "ไม่พบไฟล์ Roblox Studio: " + executable.string());
      return;
    }
  }

  const std::filesystem::path place = ExpandUser(
      gtk_editable_get_text(GTK_EDITABLE(state->place)));
  // Launching Aurora Studio from the desktop must behave like launching the
  // normal Studio application: it opens the start page. A saved place is only
  // an explicit setting-page convenience and must never hijack a plain app
  // launch. File/URI handlers still pass their requested target below.
  const bool use_configured_place = state->settings_mode &&
                                    state->launch_target.empty();
  const std::filesystem::path requested_file =
      state->launch_target_is_file ?
          ExpandUser(state->launch_target) : std::filesystem::path();
  if (state->launch_target_is_file) {
    std::error_code filesystem_error;
    if (!std::filesystem::is_regular_file(requested_file, filesystem_error) ||
        filesystem_error) {
      SetStatus(state, "ไม่พบไฟล์ Roblox: " + requested_file.string());
      return;
    }
  } else if (use_configured_place && !place.empty()) {
    std::error_code filesystem_error;
    if (!std::filesystem::is_regular_file(place, filesystem_error) ||
        filesystem_error) {
      SetStatus(state, "ไม่พบไฟล์ Roblox place: " + place.string());
      return;
    }
  }

  std::string lock_error;
  bool existing_instance = false;
  if (!AcquireStudioLock(state, &lock_error, &existing_instance)) {
    // Roblox's browser sign-in callback is delivered as a second desktop
    // launch while the first Studio process is still showing the sign-in
    // window. Start the callback through the same isolated prefix instead of
    // reporting a misleading "already running" error. Roblox Studio itself
    // forwards the argument to its existing workflow when appropriate.
    if (state->launch_target.empty() || !existing_instance) {
      SetStatus(state, lock_error);
      return;
    }
  }

  aurora::runtime::ManagedProcessSpec spec;
  const std::filesystem::path wine = ExpandUser(
      gtk_editable_get_text(GTK_EDITABLE(state->wine)));
  const std::filesystem::path proton = ExpandUser(
      gtk_editable_get_text(GTK_EDITABLE(state->proton)));
  const bool use_proton = IsProtonSelected(state);
  const auto graphics_mode = SelectedGraphicsMode(state);
  const auto* graphics_adapter = SelectedGraphicsAdapter(state);
  const std::string dxvk_state_cache =
      (AuroraStudioRuntimePath() / "cache/dxvk-state").string();
  const std::filesystem::path configured_prefix = ExpandUser(
      gtk_editable_get_text(GTK_EDITABLE(state->prefix)));
  std::filesystem::path runtime_prefix = configured_prefix;
  std::filesystem::path runtime_executable = use_proton ? proton : wine;
  std::string wine_theme;
  std::string wine_theme_error;
  if (IsWindowsExecutable(executable)) {
    if (use_proton &&
        graphics_mode == aurora::studio::StudioGraphicsMode::kDxvkLegacy) {
      SetStatus(state,
                "DXVK (Legacy) ต้องใช้ Aurora Wine; Proton จะใช้ DXVK ของตัวเอง");
      return;
    }
    std::vector<std::string> runtime_environment = {
        "WINEDEBUG=-all", "WINELOADERNOEXEC=1"};
    if (use_proton) {
      if (runtime_executable.empty()) {
        const auto detected_proton = aurora::studio::DetectProton();
        if (detected_proton.has_value()) runtime_executable = *detected_proton;
      }
      if (runtime_executable.empty()) {
        SetStatus(state, "ไม่พบ Proton: เลือก Proton executable ก่อน");
        return;
      }
      if (IsDefaultStudioPrefix(runtime_prefix)) {
        runtime_prefix = aurora::studio::DefaultProtonCompatData();
      }
      const std::filesystem::path wine_prefix = runtime_prefix / "pfx";
      runtime_environment.push_back("STEAM_COMPAT_DATA_PATH=" +
                                    runtime_prefix.string());
      runtime_environment.push_back(
          "STEAM_COMPAT_CLIENT_INSTALL_PATH=" +
          aurora::studio::ProtonClientInstallPath(runtime_executable).string());
      runtime_environment.push_back("WINEPREFIX=" + wine_prefix.string());
      runtime_environment.push_back("PROTON_LOG=0");
      if (!aurora::studio::SyncWineThemeWithRuntime(
              runtime_executable, {"runinprefix"}, wine_prefix,
              runtime_environment, &wine_theme, &wine_theme_error)) {
        g_printerr("[aurora-studio] Proton theme sync failed: %s\n",
                   wine_theme_error.c_str());
      }
      const auto graphics_environment =
          aurora::studio::GraphicsModeEnvironment(
              graphics_mode, graphics_adapter, dxvk_state_cache);
      runtime_environment.insert(runtime_environment.end(),
                                 graphics_environment.begin(),
                                 graphics_environment.end());
      spec.executable = runtime_executable.string();
      spec.arguments = {"runinprefix", executable.string()};
    } else {
      const bool managed_wine =
          runtime_executable == aurora::studio::DefaultManagedKombuchaWine();
      if (managed_wine || aurora::studio::UsesDxvk(graphics_mode)) {
        const aurora::studio::CompatibilityProvisionResult compatibility =
            aurora::studio::EnsureManagedStudioCompatibilityForMode(
                executable, graphics_mode);
        if (!compatibility) {
          SetStatus(state, "AuroraWine/DXVK: " + compatibility.error);
          return;
        }
        if (managed_wine) {
          runtime_executable = compatibility.wine_executable;
          SetEntry(state->wine, runtime_executable.string());
        }
      }
      if (!aurora::studio::SyncWineTheme(runtime_executable, runtime_prefix,
                                         &wine_theme, &wine_theme_error)) {
        g_printerr("[aurora-studio] Wine theme sync failed: %s\n",
                   wine_theme_error.c_str());
      }
      spec.executable = runtime_executable.empty() ? "wine"
                                                   : runtime_executable.string();
      spec.arguments.push_back(executable.string());
      if (!runtime_prefix.empty()) {
        runtime_environment.push_back("WINEPREFIX=" + runtime_prefix.string());
      }
      const auto graphics_environment =
          aurora::studio::GraphicsModeEnvironment(
              graphics_mode, graphics_adapter, dxvk_state_cache);
      runtime_environment.insert(runtime_environment.end(),
                                 graphics_environment.begin(),
                                 graphics_environment.end());
    }
    for (const std::string& environment : runtime_environment) {
      spec.environment.push_back(environment);
    }
    if (!state->launch_target.empty()) {
      spec.arguments.push_back(state->launch_target);
    } else if (use_configured_place && !place.empty()) {
      spec.arguments.push_back(place.string());
    }
  } else {
    spec.executable = executable.string();
  }
  if (executable.has_parent_path()) spec.working_directory = executable.parent_path();
  spec.inherit_stdio = false;
  std::string error;
  aurora::runtime::ManagedProcess process =
      aurora::runtime::ManagedProcess::Start(spec, &error);
  if (!process.valid()) {
    state->studio_lock.reset();
    SetStatus(state, error.empty() ? "cannot start Roblox Studio" : error);
    return;
  }
  state->process = std::move(process);
  state->forwarded_launch = existing_instance;
  if (!existing_instance) StartStudioDiscordPresence(state);
  if (!state->launch_target.empty() || !state->settings_mode) {
    gtk_widget_set_visible(GTK_WIDGET(state->window), FALSE);
  }
  if (state->poll_source != 0) g_source_remove(state->poll_source);
  state->poll_source = g_timeout_add(100, PollStudio, state);
  std::string launch_status =
      state->launch_target.empty()
          ? "Aurora Studio is running"
          : state->forwarded_launch ? "Aurora forwarded the Studio callback"
                                    : "Aurora Studio sign-in is opening";
  if (!wine_theme.empty()) {
    launch_status += use_proton ? " (Proton " + wine_theme + ")"
                                : " (Wine " + wine_theme + ")";
  }
  if (!wine_theme_error.empty()) {
    launch_status += use_proton ? " (Proton theme sync failed)"
                                : " (Wine theme sync failed)";
  }
  SetStatus(state, launch_status);
}

gboolean LaunchOnIdle(gpointer data) {
  auto* state = static_cast<State*>(data);
  Launch(nullptr, state);
  return G_SOURCE_REMOVE;
}

bool HasSettingsFlag(int argc, char* argv[]) {
  for (int index = 1; index < argc; ++index) {
    if (argv[index] != nullptr &&
        (std::string(argv[index]) == "--settings" ||
         std::string(argv[index]) == "-s")) {
      return true;
    }
  }
  return false;
}

void Save(GtkButton*, gpointer data) {
  auto* state = static_cast<State*>(data);
  std::string error;
  SetStatus(state, SaveConfig(state, &error) ? "บันทึกการตั้งค่าแล้ว" : error);
}

void Close(GtkButton*, gpointer data) {
  auto* state = static_cast<State*>(data);
  if (state->process.has_value()) {
    std::string ignored;
    (void)state->process->Terminate(std::chrono::milliseconds(1500), &ignored);
    state->process.reset();
  }
  StopStudioDiscordPresence(state);
  state->studio_lock.reset();
  gtk_window_destroy(state->window);
}

void Destroy(GtkWidget*, gpointer data) {
  auto* state = static_cast<State*>(data);
  if (state->loop != nullptr) g_main_loop_quit(state->loop);
}

constexpr char kSettingsStyle[] = R"css(
window.studio-settings-window {
  background-color: @window_bg_color;
  color: @window_fg_color;
}

.studio-settings-header {
  padding: 28px 32px 18px;
}

.studio-settings-title {
  font-size: 26px;
  font-weight: 700;
}

.studio-settings-subtitle,
.studio-settings-section-description,
.studio-settings-row-description {
  color: alpha(@window_fg_color, 0.68);
}

.studio-settings-page {
  padding: 4px 28px 24px;
}

.studio-settings-section-heading {
  font-size: 15px;
  font-weight: 700;
}

.studio-settings-card {
  background-color: @card_bg_color;
  color: @card_fg_color;
  border: 1px solid alpha(@borders, 0.7);
  border-radius: 13px;
  background-clip: padding-box;
}

.studio-settings-row {
  padding: 12px 14px;
  border-bottom: 1px solid alpha(@borders, 0.62);
}

.studio-settings-row:last-child {
  border-bottom: none;
}

.studio-settings-row-title {
  font-weight: 600;
}

.studio-settings-row-description {
  font-size: 0.88em;
}

.studio-settings-entry {
  min-width: 270px;
}

.studio-settings-selector {
  min-width: 240px;
}

.studio-settings-footer {
  padding: 12px 28px 22px;
  border-top: 1px solid alpha(@borders, 0.65);
}

.studio-settings-status {
  padding: 2px 32px 8px;
  min-height: 22px;
}
)css";

void InstallSettingsStyle(GdkDisplay* display) {
  GtkCssProvider* provider = gtk_css_provider_new();
  gtk_css_provider_load_from_string(provider, kSettingsStyle);
  gtk_style_context_add_provider_for_display(
      display, GTK_STYLE_PROVIDER(provider),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(provider);
}

GtkWidget* SettingsSection(GtkWidget* page, const char* title_text,
                           const char* description_text = nullptr) {
  GtkWidget* section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  GtkWidget* heading = gtk_label_new(title_text);
  gtk_widget_set_halign(heading, GTK_ALIGN_START);
  gtk_widget_add_css_class(heading, "studio-settings-section-heading");
  gtk_box_append(GTK_BOX(section), heading);
  if (description_text != nullptr && description_text[0] != '\0') {
    GtkWidget* description = gtk_label_new(description_text);
    gtk_widget_set_halign(description, GTK_ALIGN_START);
    gtk_widget_add_css_class(description,
                             "studio-settings-section-description");
    gtk_box_append(GTK_BOX(section), description);
  }
  GtkWidget* card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(card, "studio-settings-card");
  gtk_box_append(GTK_BOX(section), card);
  gtk_box_append(GTK_BOX(page), section);
  return card;
}

GtkWidget* SettingsRow(GtkWidget* card, const char* title_text,
                       const char* description_text) {
  GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_valign(row, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(row, "studio-settings-row");
  GtkWidget* text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  GtkWidget* title = gtk_label_new(title_text);
  gtk_widget_set_halign(title, GTK_ALIGN_START);
  gtk_widget_add_css_class(title, "studio-settings-row-title");
  gtk_box_append(GTK_BOX(text), title);
  if (description_text != nullptr && description_text[0] != '\0') {
    GtkWidget* description = gtk_label_new(description_text);
    gtk_widget_set_halign(description, GTK_ALIGN_START);
    gtk_label_set_wrap(GTK_LABEL(description), TRUE);
    gtk_widget_add_css_class(description, "studio-settings-row-description");
    gtk_box_append(GTK_BOX(text), description);
  }
  gtk_widget_set_hexpand(text, TRUE);
  gtk_box_append(GTK_BOX(row), text);
  gtk_box_append(GTK_BOX(card), row);
  return row;
}

GtkWidget* SettingsEntryRow(GtkWidget* card, const char* title_text,
                            const char* description_text, GtkEntry** output,
                            const char* placeholder) {
  GtkWidget* row = SettingsRow(card, title_text, description_text);
  GtkWidget* entry = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(entry), placeholder);
  gtk_widget_add_css_class(entry, "studio-settings-entry");
  gtk_widget_set_valign(entry, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(row), entry);
  *output = GTK_ENTRY(entry);
  return entry;
}

GtkWidget* SettingsSelectorRow(GtkWidget* card, const char* title_text,
                               const char* description_text,
                               GtkDropDown* selector) {
  GtkWidget* row = SettingsRow(card, title_text, description_text);
  gtk_widget_add_css_class(GTK_WIDGET(selector), "studio-settings-selector");
  gtk_widget_set_valign(GTK_WIDGET(selector), GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(row), GTK_WIDGET(selector));
  return row;
}

GtkWidget* GraphicsSelectorRow(GtkWidget* card, const char* title_text,
                               const char* description_text,
                               GtkDropDown* selector) {
  return SettingsSelectorRow(card, title_text, description_text, selector);
}

GtkWidget* RuntimeSelectorRow(GtkWidget* card, GtkSwitch** runtime) {
  GtkWidget* row = SettingsRow(
      card, "Compatibility Runtime",
      "Wine is recommended. Proton is available as an experimental fallback.");
  GtkWidget* choices = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  GtkWidget* wine_label = gtk_label_new("Wine");
  GtkWidget* proton_label = gtk_label_new("Proton");
  gtk_widget_add_css_class(wine_label, "dim-label");
  gtk_widget_add_css_class(proton_label, "dim-label");
  *runtime = GTK_SWITCH(gtk_switch_new());
  gtk_widget_set_valign(GTK_WIDGET(*runtime), GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(choices), wine_label);
  gtk_box_append(GTK_BOX(choices), GTK_WIDGET(*runtime));
  gtk_box_append(GTK_BOX(choices), proton_label);
  gtk_box_append(GTK_BOX(row), choices);
  return row;
}

GtkWidget* SettingsSwitchRow(GtkWidget* card, const char* title_text,
                             const char* description_text,
                             GtkSwitch** output) {
  GtkWidget* row = SettingsRow(card, title_text, description_text);
  *output = GTK_SWITCH(gtk_switch_new());
  gtk_widget_set_valign(GTK_WIDGET(*output), GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(row), GTK_WIDGET(*output));
  return row;
}

int Run(int argc, char* argv[]) {
  gtk_init();
  adw_init();
  const aurora::studio::HostThemeResolution host_theme =
      aurora::studio::ResolveHostTheme();
  adw_style_manager_set_color_scheme(adw_style_manager_get_default(),
                                     host_theme.dark
                                         ? ADW_COLOR_SCHEME_FORCE_DARK
                                         : ADW_COLOR_SCHEME_FORCE_LIGHT);
  gtk_window_set_default_icon_name("space.bigrat.aurora-studio");

  State state;
  state.has_theme_snapshot = true;
  state.dark_theme = host_theme.dark;
  state.config = ConfigPath();
  state.settings_mode = HasSettingsFlag(argc, argv);
  state.graphics_adapters = EnumerateGraphicsAdapters();
  state.launch_target =
      FindStudioLaunchTarget(argc, argv, &state.launch_target_is_file);
  state.loop = g_main_loop_new(nullptr, FALSE);
  state.window = GTK_WINDOW(gtk_window_new());
  gtk_window_set_title(state.window, "Aurora Studio");
  gtk_window_set_default_size(state.window, 840, 820);
  gtk_widget_add_css_class(GTK_WIDGET(state.window),
                           "studio-settings-window");
  InstallSettingsStyle(gdk_display_get_default());
  g_signal_connect(state.window, "destroy", G_CALLBACK(Destroy), &state);

  GtkWidget* outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget* header = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_add_css_class(header, "studio-settings-header");
  GtkWidget* title = gtk_label_new("Aurora Studio");
  gtk_widget_add_css_class(title, "studio-settings-title");
  gtk_widget_set_halign(title, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(header), title);
  GtkWidget* subtitle = gtk_label_new(
      "Configure your Roblox Studio runtime, graphics and launch behavior.");
  gtk_widget_add_css_class(subtitle, "studio-settings-subtitle");
  gtk_widget_set_halign(subtitle, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(header), subtitle);
  gtk_box_append(GTK_BOX(outer), header);

  GtkWidget* scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(scroll, TRUE);
  GtkWidget* page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 22);
  gtk_widget_add_css_class(page, "studio-settings-page");
  gtk_widget_set_size_request(page, 680, -1);
  gtk_widget_set_halign(page, GTK_ALIGN_CENTER);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), page);
  gtk_box_append(GTK_BOX(outer), scroll);

  GtkWidget* runtime_card = SettingsSection(
      page, "Components", "Runtime and executable used to start Studio.");
  RuntimeSelectorRow(runtime_card, &state.runtime);
  g_signal_connect(state.runtime, "notify::active", G_CALLBACK(RuntimeChanged),
                   &state);
  SettingsEntryRow(runtime_card, "Roblox Studio executable",
                   "Path to RobloxStudioBeta.exe", &state.executable,
                   "/path/to/RobloxStudioBeta.exe");
  SettingsEntryRow(runtime_card, "Wine executable",
                   "Wine binary used by the recommended runtime", &state.wine,
                   "wine");
  SettingsEntryRow(runtime_card, "Proton executable",
                   "Optional; Aurora can detect an installed Proton", &state.proton,
                   "Auto-detect Proton");

  GtkWidget* display_card = SettingsSection(
      page, "Display", "Choose the renderer and graphics adapter for Studio.");
  GtkStringList* graphics_mode_options = gtk_string_list_new(nullptr);
  for (const auto mode : kGraphicsModes) {
    const std::string label(aurora::studio::GraphicsModeLabel(mode));
    gtk_string_list_append(graphics_mode_options, label.c_str());
  }
  state.graphics_mode = GTK_DROP_DOWN(gtk_drop_down_new(
      G_LIST_MODEL(graphics_mode_options), nullptr));
  gtk_drop_down_set_selected(state.graphics_mode, 2);
  GraphicsSelectorRow(display_card, "Graphics Mode",
                      "Determines performance and compatibility",
                      state.graphics_mode);

  GtkStringList* graphics_card_options = gtk_string_list_new(nullptr);
  gtk_string_list_append(graphics_card_options, "System Default");
  for (const auto& adapter : state.graphics_adapters) {
    gtk_string_list_append(graphics_card_options, adapter.name.c_str());
  }
  state.graphics_card = GTK_DROP_DOWN(gtk_drop_down_new(
      G_LIST_MODEL(graphics_card_options), nullptr));
  gtk_drop_down_set_selected(state.graphics_card, 0);
  GraphicsSelectorRow(display_card, "Graphics Card",
                      "Select a discrete or specific card for rendering",
                      state.graphics_card);

  GtkWidget* behavior_card = SettingsSection(
      page, "Behavior", "Optional controls for Studio while it is running.");
  SettingsSwitchRow(behavior_card, "Share Activity on Discord",
                   "Show Roblox Studio activity on your Discord profile",
                   &state.discord_activity);
  SettingsEntryRow(behavior_card, "Memory safety limit (MiB)",
                   "Set to 0 to disable the limit", &state.memory_limit, "0");

  GtkWidget* advanced_card = SettingsSection(
      page, "Advanced", "Custom launch paths and an optional place file.");
  SettingsEntryRow(advanced_card, "WINEPREFIX / Proton compatdata",
                   "Wine prefix or Proton compatibility data directory",
                   &state.prefix, "~/.local/share/aurora/studio-prefix");
  SettingsEntryRow(advanced_card, "Place file",
                   "Optional; used when launching Studio from this page",
                   &state.place, "~/Documents/MyPlace.rbxl");

  state.status = GTK_LABEL(gtk_label_new(""));
  gtk_widget_set_halign(GTK_WIDGET(state.status), GTK_ALIGN_START);
  gtk_widget_add_css_class(GTK_WIDGET(state.status), "dim-label");
  gtk_widget_add_css_class(GTK_WIDGET(state.status), "studio-settings-status");
  gtk_box_append(GTK_BOX(outer), GTK_WIDGET(state.status));
  GtkWidget* actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_add_css_class(actions, "studio-settings-footer");
  GtkWidget* close = gtk_button_new_with_label("ปิด");
  GtkWidget* save = gtk_button_new_with_label("บันทึก");
  GtkWidget* launch = gtk_button_new_with_label("บันทึกและเปิด Studio");
  gtk_widget_add_css_class(launch, "suggested-action");
  GtkWidget* spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(spacer, TRUE);
  g_signal_connect(close, "clicked", G_CALLBACK(Close), &state);
  g_signal_connect(save, "clicked", G_CALLBACK(Save), &state);
  g_signal_connect(launch, "clicked", G_CALLBACK(Launch), &state);
  gtk_box_append(GTK_BOX(actions), close);
  gtk_box_append(GTK_BOX(actions), spacer);
  gtk_box_append(GTK_BOX(actions), save);
  gtk_box_append(GTK_BOX(actions), launch);
  gtk_box_append(GTK_BOX(outer), actions);
  gtk_window_set_child(state.window, outer);
  LoadConfig(&state);
  state.memory_limit_bytes = MemoryLimitBytes(&state);
  if (state.settings_mode) {
    gtk_window_present(state.window);
  } else {
    gtk_widget_set_visible(GTK_WIDGET(state.window), FALSE);
  }
  if (!state.settings_mode || !state.launch_target.empty()) {
    g_idle_add(LaunchOnIdle, &state);
  }
  state.theme_poll_source = g_timeout_add(1000, PollHostTheme, &state);
  g_main_loop_run(state.loop);
  if (state.poll_source != 0) g_source_remove(state.poll_source);
  if (state.theme_poll_source != 0) g_source_remove(state.theme_poll_source);
  if (state.process.has_value()) {
    std::string ignored;
    (void)state.process->Terminate(std::chrono::milliseconds(1500), &ignored);
    state.process.reset();
  }
  StopStudioDiscordPresence(&state);
  g_main_loop_unref(state.loop);
  return 0;
}

}  // namespace

int main(int argc, char* argv[]) { return Run(argc, argv); }
