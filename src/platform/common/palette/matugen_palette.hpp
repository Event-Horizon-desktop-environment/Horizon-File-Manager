#pragma once

#include <string>
#include <string_view>

namespace eh::config {
struct ShellAppearance;
}

namespace eh::matugen {

[[nodiscard]] std::string normalize_matugen_scheme(std::string_view in);
[[nodiscard]] std::string normalize_matugen_mode(std::string_view in);

void refresh_wallpaper_derived_palette(eh::config::ShellAppearance& appearance,
                                         const std::string& normalized_wallpaper_image_path);

/// Read colors from the Event Horizon Shell color engine config file
/// (~/.config/event-horizon/horizon-files-matugen.conf).
/// Returns true if the file existed and colors were populated.
[[nodiscard]] bool read_shell_color_config(eh::config::ShellAppearance& appearance);

/// Returns the path to the shell color engine config file.
[[nodiscard]] std::string shell_color_config_path();

}
