#pragma once

#include "atlas/terminal/theme.hpp"

#include <sol/sol.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace atlas::lua {

void merge_theme_styles(const sol::table& styles, terminal::Theme& theme,
                        std::vector<std::string>& warnings, std::string_view source);
void merge_theme_definition(const sol::table& definition, terminal::Theme& theme,
                            std::vector<std::string>& warnings, std::string_view source);
void merge_art(const sol::object& definition, terminal::ArtOptions& art,
               std::vector<std::string>& warnings, const std::filesystem::path& base);

} // namespace atlas::lua
