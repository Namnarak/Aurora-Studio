// SPDX-License-Identifier: GPL-3.0-only

#ifndef AURORA_STUDIO_STUDIO_LAUNCH_SETTINGS_H_
#define AURORA_STUDIO_STUDIO_LAUNCH_SETTINGS_H_

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace aurora::studio {

inline constexpr std::string_view kDxvkDeviceFilterSetting =
    "dxvk_filter_device_name";
inline constexpr std::string_view kDxvkDeviceFilterEnvironment =
    "DXVK_FILTER_DEVICE_NAME";
inline constexpr std::string_view kGraphicsModeSetting = "graphics_mode";
inline constexpr std::string_view kGraphicsCardSetting = "graphics_card";

enum class StudioGraphicsMode {
  kWineD3d,
  kWineD3dLegacy,
  kDxvk,
  kDxvkLegacy,
  kVulkan,
};

inline constexpr std::string_view GraphicsModeLabel(StudioGraphicsMode mode) {
  switch (mode) {
    case StudioGraphicsMode::kWineD3d: return "WineD3D";
    case StudioGraphicsMode::kWineD3dLegacy: return "WineD3D (Legacy)";
    case StudioGraphicsMode::kDxvk: return "DXVK";
    case StudioGraphicsMode::kDxvkLegacy: return "DXVK (Legacy)";
    case StudioGraphicsMode::kVulkan: return "Vulkan";
  }
  return "DXVK";
}

inline constexpr std::string_view GraphicsModeConfigValue(
    StudioGraphicsMode mode) {
  switch (mode) {
    case StudioGraphicsMode::kWineD3d: return "wined3d";
    case StudioGraphicsMode::kWineD3dLegacy: return "wined3d_legacy";
    case StudioGraphicsMode::kDxvk: return "dxvk";
    case StudioGraphicsMode::kDxvkLegacy: return "dxvk_legacy";
    case StudioGraphicsMode::kVulkan: return "vulkan";
  }
  return "dxvk";
}

inline constexpr std::optional<StudioGraphicsMode> ParseGraphicsMode(
    std::string_view value) {
  if (value == "wined3d") return StudioGraphicsMode::kWineD3d;
  if (value == "wined3d_legacy") return StudioGraphicsMode::kWineD3dLegacy;
  if (value == "dxvk") return StudioGraphicsMode::kDxvk;
  if (value == "dxvk_legacy") return StudioGraphicsMode::kDxvkLegacy;
  if (value == "vulkan") return StudioGraphicsMode::kVulkan;
  return std::nullopt;
}

inline constexpr bool UsesDxvk(StudioGraphicsMode mode) {
  return mode == StudioGraphicsMode::kDxvk ||
         mode == StudioGraphicsMode::kDxvkLegacy;
}

struct GraphicsAdapterChoice {
  std::string name;
  std::uint32_t vendor_id = 0;
  std::uint32_t device_id = 0;
};

inline std::vector<std::string> GraphicsModeEnvironment(
    StudioGraphicsMode mode, const GraphicsAdapterChoice* adapter,
    std::string_view state_cache_path = {}) {
  const bool use_wined3d_gl =
      mode == StudioGraphicsMode::kWineD3d ||
      mode == StudioGraphicsMode::kWineD3dLegacy;
  const bool use_dxvk = UsesDxvk(mode);
  const std::string renderer = use_wined3d_gl ? "gl" : "vulkan";
  const std::string d3d_overrides = use_dxvk ? "n,b" : "b";
  std::vector<std::string> environment = {
      "WINE_D3D_CONFIG=renderer=" + renderer,
      "WINEDLLOVERRIDES=;dxdiagn,winemenubuilder.exe,mscoree,mshtml="
      ";d3d9,d3d10core,d3d11,dxgi=" + d3d_overrides,
      std::string(
          "WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS=--in-process-gpu --use-angle=") +
          (use_wined3d_gl ? "gl" : "vulkan"),
  };
  if (adapter != nullptr) {
    if (!adapter->name.empty() && use_dxvk) {
      environment.push_back(std::string(kDxvkDeviceFilterEnvironment) + "=" +
                           adapter->name);
    }
    if (adapter->vendor_id != 0 && adapter->device_id != 0) {
      std::ostringstream prime;
      prime << "DRI_PRIME=" << std::hex << std::setfill('0') << std::setw(4)
            << adapter->vendor_id << ':' << std::setw(4) << adapter->device_id
            << '!';
      environment.push_back(prime.str());
    }
  }
  if (use_dxvk) {
    environment.push_back("DXVK_LOG_LEVEL=warn");
    environment.push_back("DXVK_LOG_PATH=none");
    if (!state_cache_path.empty()) {
      environment.push_back("DXVK_STATE_CACHE_PATH=" +
                            std::string(state_cache_path));
    }
  }
  return environment;
}

inline std::string ReadConfigSetting(const std::filesystem::path& path,
                                     std::string_view key) {
  std::ifstream input(path);
  const std::string prefix = std::string(key) + "=";
  std::string line;
  while (std::getline(input, line)) {
    if (line.compare(0, prefix.size(), prefix) == 0) {
      return line.substr(prefix.size());
    }
  }
  return {};
}

inline std::optional<std::string> FormatConfigSetting(
    std::string_view key, std::string_view value) {
  if (key.find_first_of("\r\n") != std::string_view::npos ||
      value.find_first_of("\r\n") != std::string_view::npos) {
    return std::nullopt;
  }
  return std::string(key) + "=" + std::string(value);
}

}  // namespace aurora::studio

#endif  // AURORA_STUDIO_STUDIO_LAUNCH_SETTINGS_H_
