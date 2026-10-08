// SPDX-License-Identifier: GPL-3.0-only

#include "studio/studio_launch_settings.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <unistd.h>

namespace aurora::studio {
namespace {

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    char path_template[] = "/tmp/aurora_studio_settings_XXXXXX";
    const char* created = mkdtemp(path_template);
    if (created != nullptr) path_ = created;
  }

  ~TemporaryDirectory() {
    if (!path_.empty()) {
      std::error_code error;
      std::filesystem::remove_all(path_, error);
    }
  }

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

TEST(StudioLaunchSettingsTest, SavesAndLoadsOptionalDeviceNameFilter) {
  TemporaryDirectory temporary;
  ASSERT_FALSE(temporary.path().empty());
  const std::filesystem::path config = temporary.path() / "studio.conf";
  const std::string filter = "Intel(R) Arc(tm) Graphics (MTL)";
  const auto serialized = FormatConfigSetting(kDxvkDeviceFilterSetting, filter);
  ASSERT_TRUE(serialized.has_value());
  {
    std::ofstream output(config);
    ASSERT_TRUE(output);
    output << *serialized << '\n';
  }

  EXPECT_EQ(ReadConfigSetting(config, kDxvkDeviceFilterSetting), filter);
  EXPECT_EQ(ReadConfigSetting(temporary.path() / "missing.conf",
                              kDxvkDeviceFilterSetting),
            "");
  const auto empty = FormatConfigSetting(kDxvkDeviceFilterSetting, "");
  ASSERT_TRUE(empty.has_value());
  EXPECT_EQ(*empty, "dxvk_filter_device_name=");
}

TEST(StudioLaunchSettingsTest, RejectsLineBreaksInConfigValues) {
  EXPECT_FALSE(FormatConfigSetting(kDxvkDeviceFilterSetting, "Intel Arc\r"));
  EXPECT_FALSE(FormatConfigSetting(kDxvkDeviceFilterSetting, "Intel Arc\n"));
  EXPECT_FALSE(FormatConfigSetting(kGraphicsCardSetting, "Intel Arc\nnext"));
}

TEST(StudioLaunchSettingsTest, GraphicsModeConfigRoundTripsAllChoices) {
  for (const auto mode : {StudioGraphicsMode::kWineD3d,
                          StudioGraphicsMode::kWineD3dLegacy,
                          StudioGraphicsMode::kDxvk,
                          StudioGraphicsMode::kDxvkLegacy,
                          StudioGraphicsMode::kVulkan}) {
    const std::string value(GraphicsModeConfigValue(mode));
    const auto parsed = ParseGraphicsMode(value);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, mode);
  }
  EXPECT_FALSE(ParseGraphicsMode("unknown").has_value());
}

TEST(StudioLaunchSettingsTest, WineD3dUsesWineOpenGlAndBuiltinDlls) {
  const auto environment =
      GraphicsModeEnvironment(StudioGraphicsMode::kWineD3d, nullptr);
  EXPECT_NE(std::find(environment.begin(), environment.end(),
                      "WINE_D3D_CONFIG=renderer=gl"),
            environment.end());
  EXPECT_NE(std::find(environment.begin(), environment.end(),
                      "WINEDLLOVERRIDES=;dxdiagn,winemenubuilder.exe,mscoree,mshtml="
                      ";d3d9,d3d10core,d3d11,dxgi=b"),
            environment.end());
  EXPECT_EQ(std::find_if(environment.begin(), environment.end(),
                         [](const std::string& value) {
                           return value.rfind("DXVK_FILTER_DEVICE_NAME=", 0) ==
                                  0;
                         }),
            environment.end());
}

TEST(StudioLaunchSettingsTest, DxvkUsesSelectedCardAndStateCache) {
  const GraphicsAdapterChoice adapter{
      "Intel(R) Arc(tm) Graphics (MTL)", 0x8086, 0x7d55};
  const auto environment = GraphicsModeEnvironment(
      StudioGraphicsMode::kDxvk, &adapter, "/tmp/aurora/dxvk-cache");
  EXPECT_NE(std::find(environment.begin(), environment.end(),
                      "WINE_D3D_CONFIG=renderer=vulkan"),
            environment.end());
  EXPECT_NE(std::find(environment.begin(), environment.end(),
                      "DXVK_FILTER_DEVICE_NAME=Intel(R) Arc(tm) Graphics (MTL)"),
            environment.end());
  EXPECT_NE(std::find(environment.begin(), environment.end(),
                      "DRI_PRIME=8086:7d55!"),
            environment.end());
  EXPECT_NE(std::find(environment.begin(), environment.end(),
                      "DXVK_STATE_CACHE_PATH=/tmp/aurora/dxvk-cache"),
            environment.end());
}

TEST(StudioLaunchSettingsTest, VulkanModeDoesNotEnableDxvk) {
  const auto environment =
      GraphicsModeEnvironment(StudioGraphicsMode::kVulkan, nullptr);
  EXPECT_NE(std::find(environment.begin(), environment.end(),
                      "WINE_D3D_CONFIG=renderer=vulkan"),
            environment.end());
  EXPECT_EQ(std::find_if(environment.begin(), environment.end(),
                         [](const std::string& value) {
                           return value.rfind("DXVK_", 0) == 0;
                         }),
            environment.end());
}

}  // namespace
}  // namespace aurora::studio
