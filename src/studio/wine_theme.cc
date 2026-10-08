// SPDX-License-Identifier: GPL-3.0-only

#include "wine_theme.h"

#include <gio/gio.h>

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

#include <sys/wait.h>
#include <unistd.h>

#include "runtime/managed_process.h"

namespace aurora {
namespace studio {
namespace {

std::filesystem::path HomePath() {
  const char* home = std::getenv("HOME");
  return home == nullptr || home[0] == '\0' ? std::filesystem::path(".")
                                             : std::filesystem::path(home);
}

std::filesystem::path ConfigPath(std::string_view relative) {
  const char* configured = std::getenv("XDG_CONFIG_HOME");
  const std::filesystem::path root =
      configured != nullptr && configured[0] == '/' ?
          std::filesystem::path(configured) : HomePath() / ".config";
  return root / std::filesystem::path(relative);
}

std::string Trim(std::string value) {
  std::size_t begin = 0;
  while (begin < value.size() &&
         std::isspace(static_cast<unsigned char>(value[begin]))) {
    ++begin;
  }
  std::size_t end = value.size();
  while (end > begin &&
         std::isspace(static_cast<unsigned char>(value[end - 1]))) {
    --end;
  }
  return value.substr(begin, end - begin);
}

std::string LowerAscii(std::string value) {
  for (char& character : value) {
    character = static_cast<char>(std::tolower(
        static_cast<unsigned char>(character)));
  }
  return value;
}

std::optional<bool> ThemeFromName(const std::string& value) {
  const std::string lower = LowerAscii(Trim(value));
  if (lower.find("dark") != std::string::npos) return true;
  if (lower.find("light") != std::string::npos) return false;
  return std::nullopt;
}

std::optional<bool> ThemeFromBoolean(const std::string& value) {
  const std::string normalized = LowerAscii(Trim(value));
  if (normalized == "true" || normalized == "1" || normalized == "yes") {
    return true;
  }
  if (normalized == "false" || normalized == "0" || normalized == "no") {
    return false;
  }
  return std::nullopt;
}

std::optional<std::string> ReadIniValue(const std::filesystem::path& path,
                                        std::string_view wanted_section,
                                        std::string_view wanted_key) {
  std::ifstream input(path);
  if (!input.is_open()) return std::nullopt;

  const std::string section = LowerAscii(std::string(wanted_section));
  const std::string key = LowerAscii(std::string(wanted_key));
  bool in_section = false;
  std::string line;
  std::size_t line_count = 0;
  while (line_count++ < 4096 && std::getline(input, line)) {
    if (line.size() > 8192) continue;
    const std::string trimmed = Trim(line);
    if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';') continue;
    if (trimmed.front() == '[' && trimmed.back() == ']') {
      in_section = LowerAscii(Trim(trimmed.substr(1, trimmed.size() - 2))) ==
                   section;
      continue;
    }
    if (!in_section) continue;
    const std::size_t separator = trimmed.find('=');
    if (separator == std::string::npos ||
        LowerAscii(Trim(trimmed.substr(0, separator))) != key) {
      continue;
    }
    return Trim(trimmed.substr(separator + 1));
  }
  return std::nullopt;
}

std::optional<bool> ReadKdeTheme() {
  const auto value = ReadIniValue(ConfigPath("kdeglobals"), "General",
                                  "ColorScheme");
  return value.has_value() ? ThemeFromName(*value) : std::nullopt;
}

std::optional<bool> ReadGtkTheme() {
  if (const char* gtk_theme = std::getenv("GTK_THEME");
      gtk_theme != nullptr && gtk_theme[0] != '\0') {
    if (const auto result = ThemeFromName(gtk_theme); result.has_value()) {
      return result;
    }
  }

  for (const char* relative : {"gtk-4.0/settings.ini",
                               "gtk-3.0/settings.ini"}) {
    const auto path = ConfigPath(relative);
    if (const auto value = ReadIniValue(path, "Settings", "gtk-theme");
        value.has_value()) {
      if (const auto result = ThemeFromName(*value); result.has_value()) {
        return result;
      }
    }
    if (const auto value = ReadIniValue(
            path, "Settings", "gtk-application-prefer-dark-theme");
        value.has_value()) {
      if (const auto result = ThemeFromBoolean(*value); result.has_value()) {
        return result;
      }
    }
  }
  return std::nullopt;
}

std::optional<bool> ReadGSettingsTheme() {
  GSettings* settings = g_settings_new("org.gnome.desktop.interface");
  if (settings == nullptr) return std::nullopt;

  std::optional<bool> result;
  gchar* color_scheme = g_settings_get_string(settings, "color-scheme");
  if (color_scheme != nullptr) {
    const std::string value = LowerAscii(color_scheme);
    if (value == "prefer-dark") {
      result = true;
    } else if (value == "prefer-light") {
      result = false;
    }
    g_free(color_scheme);
  }
  if (!result.has_value()) {
    gchar* gtk_theme = g_settings_get_string(settings, "gtk-theme");
    if (gtk_theme != nullptr) {
      result = ThemeFromName(gtk_theme);
      g_free(gtk_theme);
    }
  }
  g_object_unref(settings);
  return result;
}

HostThemeResolution ResolveTheme() {
  for (const char* variable : {"AURORA_WINE_THEME", "AURORA_THEME"}) {
    const char* value = std::getenv(variable);
    if (value == nullptr || value[0] == '\0' ||
        LowerAscii(value) == "system") {
      continue;
    }
    if (const auto result = ThemeFromName(value); result.has_value()) {
      return {*result, variable};
    }
  }
  if (const auto theme = ReadKdeTheme(); theme.has_value()) {
    return {*theme, "KDE kdeglobals"};
  }
  if (const auto theme = ReadGtkTheme(); theme.has_value()) {
    return {*theme, "GTK settings"};
  }
  if (const auto theme = ReadGSettingsTheme(); theme.has_value()) {
    return {*theme, "GSettings"};
  }
  return {};
}

struct SystemColor {
  const char* name;
  const char* dark;
  const char* light;
};

constexpr SystemColor kSystemColors[] = {
    {"ActiveBorder", "45 45 48", "212 208 200"},
    {"ActiveTitle", "45 45 48", "10 36 106"},
    {"AppWorkSpace", "32 32 32", "128 128 128"},
    {"Background", "32 32 32", "58 110 165"},
    {"ButtonAlternateFace", "45 45 48", "181 181 181"},
    {"ButtonDkShadow", "20 20 20", "64 64 64"},
    {"ButtonFace", "45 45 48", "212 208 200"},
    {"ButtonHilight", "80 80 80", "255 255 255"},
    {"ButtonLight", "60 60 60", "212 208 200"},
    {"ButtonShadow", "20 20 20", "128 128 128"},
    {"ButtonText", "241 241 241", "0 0 0"},
    {"GradientActiveTitle", "45 45 48", "166 202 240"},
    {"GradientInactiveTitle", "32 32 32", "192 192 192"},
    {"GrayText", "160 160 160", "128 128 128"},
    {"Hilight", "51 103 178", "10 36 106"},
    {"HilightText", "255 255 255", "255 255 255"},
    {"HotTrackingColor", "120 170 230", "0 0 200"},
    {"InactiveBorder", "45 45 48", "212 208 200"},
    {"InactiveTitle", "32 32 32", "128 128 128"},
    {"InactiveTitleText", "160 160 160", "212 208 200"},
    {"InfoText", "241 241 241", "0 0 0"},
    {"InfoWindow", "45 45 48", "255 255 225"},
    {"Menu", "45 45 48", "212 208 200"},
    {"MenuBar", "45 45 48", "212 208 200"},
    {"MenuHilight", "65 65 65", "10 36 106"},
    {"MenuText", "241 241 241", "0 0 0"},
    {"Scrollbar", "45 45 48", "212 208 200"},
    {"TitleText", "241 241 241", "255 255 255"},
    {"Window", "32 32 32", "255 255 255"},
    {"WindowFrame", "20 20 20", "0 0 0"},
    {"WindowText", "241 241 241", "0 0 0"},
};

void WriteColorKey(std::ofstream& output, const char* key, bool dark) {
  output << "[" << key << "]\n";
  for (const SystemColor& color : kSystemColors) {
    output << '"' << color.name << "\"=\""
           << (dark ? color.dark : color.light) << "\"\n";
  }
  output << '\n';
}

bool WriteRegistryFile(const std::filesystem::path& path, bool dark,
                       std::string* error) {
  std::ofstream output(path, std::ios::trunc);
  if (!output.is_open()) {
    if (error != nullptr) *error = "cannot create Wine theme registry file";
    return false;
  }

  output << "REGEDIT4\n\n"
         << "[HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize]\n"
         << "\"AppsUseLightTheme\"=dword:"
         << (dark ? "00000000" : "00000001") << "\n"
         << "\"SystemUsesLightTheme\"=dword:"
         << (dark ? "00000000" : "00000001") << "\n\n"
         << "[HKEY_CURRENT_USER\\Software\\Roblox\\RobloxStudio\\Themes]\n"
         << "\"CurrentTheme\"=\""
         << (dark ? "Dark" : "Light") << "\"\n\n";

  // Wine's classic controls read the ThemeManager copy of the system colors.
  // The top-level copy covers applications that query the normal Windows key.
  output << "[HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\ThemeManager]\n";
  if (dark) {
    output << "\"ThemeActive\"=\"0\"\n"
           << "\"ColorName\"=-\n"
           << "\"DllName\"=-\n"
           << "\"SizeName\"=-\n"
           << "\"LoadedBefore\"=-\n\n";
  } else {
    output << "\"ThemeActive\"=\"1\"\n"
           << "\"ColorName\"=\"Blue\"\n"
           << "\"DllName\"=\"C:\\\\windows\\\\resources\\\\themes\\\\aero\\\\aero.msstyles\"\n"
           << "\"SizeName\"=\"NormalSize\"\n"
           << "\"LoadedBefore\"=\"1\"\n\n";
  }

  WriteColorKey(output, "HKEY_CURRENT_USER\\Control Panel\\Colors", dark);
  WriteColorKey(
      output,
      "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\ThemeManager\\Control Panel\\Colors",
      dark);
  output.close();
  if (!output) {
    if (error != nullptr) *error = "cannot finish Wine theme registry file";
    return false;
  }
  return true;
}

}  // namespace

HostThemeResolution ResolveHostTheme() { return ResolveTheme(); }

bool SyncWineThemeWithRuntime(
    const std::filesystem::path& runner,
    const std::vector<std::string>& runner_arguments,
    const std::filesystem::path& prefix,
    const std::vector<std::string>& environment,
    std::string* description,
    std::string* error) {
  if (description != nullptr) description->clear();
  if (error != nullptr) error->clear();
  if (prefix.empty()) {
    if (error != nullptr) *error = "Wine prefix is empty";
    return false;
  }

  const HostThemeResolution theme = ResolveHostTheme();
  std::error_code filesystem_error;
  std::filesystem::create_directories(prefix, filesystem_error);
  if (filesystem_error) {
    if (error != nullptr) {
      *error = "cannot create Wine prefix for theme sync: " +
               filesystem_error.message();
    }
    return false;
  }

  const std::filesystem::path registry_file =
      prefix / (".aurora-theme-" + std::to_string(getpid()) + ".reg");
  if (!WriteRegistryFile(registry_file, theme.dark, error)) return false;

  aurora::runtime::ManagedProcessSpec spec;
  spec.executable = runner.empty() ? "wine" : runner.string();
  spec.arguments = runner_arguments;
  spec.arguments.push_back("reg");
  spec.arguments.push_back("import");
  spec.arguments.push_back(registry_file.string());
  spec.environment = environment;
  spec.inherit_stdio = false;
  std::string start_error;
  aurora::runtime::ManagedProcess process =
      aurora::runtime::ManagedProcess::Start(spec, &start_error);
  if (!process.valid()) {
    std::filesystem::remove(registry_file, filesystem_error);
    if (error != nullptr) *error = start_error;
    return false;
  }

  int wait_status = 0;
  const bool waited = process.Wait(&wait_status, &start_error);
  std::filesystem::remove(registry_file, filesystem_error);
  if (!waited) {
    if (error != nullptr) *error = start_error;
    return false;
  }
  if (!WIFEXITED(wait_status) || WEXITSTATUS(wait_status) != 0) {
    if (error != nullptr) {
      *error = "runtime registry import failed with status " +
               std::to_string(wait_status);
    }
    return false;
  }

  if (description != nullptr) {
    *description = std::string(theme.dark ? "dark" : "light") + " via " +
                   theme.source;
  }
  return true;
}

bool SyncWineTheme(const std::filesystem::path& wine,
                   const std::filesystem::path& prefix,
                   std::string* description,
                   std::string* error) {
  return SyncWineThemeWithRuntime(
      wine, {}, prefix,
      {"WINEPREFIX=" + prefix.string(), "WINEDEBUG=-all",
       "WINELOADERNOEXEC=1"},
      description, error);
}

}  // namespace studio
}  // namespace aurora
