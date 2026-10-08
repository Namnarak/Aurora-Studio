// SPDX-License-Identifier: GPL-3.0-only

#include "studio/studio_provisioner.h"

#include <gtest/gtest.h>

namespace aurora::studio {
namespace {

TEST(StudioProvisionerTest, ParsesOfficialVersionIdentity) {
  const auto result = ParseStudioVersionResponse(
      R"({"version":"0.741.19.7411056","clientVersionUpload":"version-76e1a02649ad4f35","bootstrapperVersion":""})");
  ASSERT_TRUE(result) << result.error;
  EXPECT_EQ(result.version, "0.741.19.7411056");
  EXPECT_EQ(result.upload, "version-76e1a02649ad4f35");
}

TEST(StudioProvisionerTest, RejectsUnsafeVersionIdentity) {
  const auto result = ParseStudioVersionResponse(
      R"({"version":"1","clientVersionUpload":"../bad"})");
  EXPECT_FALSE(result);
}

TEST(StudioProvisionerTest, ParsesPackageManifestAndDestinations) {
  const auto result = ParseStudioPackageManifest(
      "v0\r\n"
      "RobloxStudio.zip\r\n"
      "0123456789abcdef0123456789abcdef\r\n"
      "100\r\n"
      "200\r\n"
      "content-textures3.zip\r\n"
      "abcdef0123456789abcdef0123456789\r\n"
      "300\r\n"
      "400\r\n");
  ASSERT_TRUE(result) << result.error;
  ASSERT_EQ(result.packages.size(), 2U);
  EXPECT_TRUE(result.packages[0].destination.empty());
  EXPECT_EQ(result.packages[1].destination,
            std::filesystem::path("PlatformContent/pc/textures"));
}

TEST(StudioProvisionerTest, MapsCurrentStudioPackageLayout) {
  EXPECT_EQ(StudioPackageDestination("BuiltInPlugins.zip"),
            std::filesystem::path("BuiltInPlugins"));
  EXPECT_EQ(StudioPackageDestination("content-textures2.zip"),
            std::filesystem::path("content/textures"));
  EXPECT_EQ(StudioPackageDestination("studiocontent-models.zip"),
            std::filesystem::path("StudioContent/models"));
  EXPECT_FALSE(StudioPackageDestination("UnknownFuturePackage.zip").has_value());
}

TEST(StudioProvisionerTest, RejectsUnmappedPackage) {
  const auto result = ParseStudioPackageManifest(
      "v0\n"
      "UnknownFuturePackage.zip\n"
      "0123456789abcdef0123456789abcdef\n"
      "1\n"
      "1\n");
  EXPECT_FALSE(result);
  EXPECT_NE(result.error.find("unmapped package"), std::string::npos);
}

}  // namespace
}  // namespace aurora::studio
