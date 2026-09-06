#pragma once

#include "atlas/error.hpp"
#include "atlas/cli/registry.hpp"
#include "atlas/completion.hpp"

#include <sol/sol.hpp>

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace atlas::cli { class Registry; }
namespace atlas::terminal { class Ui; struct Theme; }

namespace atlas::lua {

class Runtime {
public:
    explicit Runtime(terminal::Ui* ui = nullptr);
    ~Runtime() = default;
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] const Error& initialization_error() const noexcept;
    Status load_file(const std::filesystem::path& file);
    Status load_user_config();
    Status load_user_extensions(bool best_effort = false);
    Status add_commands_to(cli::Registry& registry);
    Result<std::vector<completion::Item>> complete(std::string_view provider,
        std::string_view current, std::span<const std::string> completed);
    [[nodiscard]] bool has_command(std::string_view name) const;
    Result<int> invoke(std::string_view name, std::span<const std::string> arguments);
    Result<std::string> configured_tool(std::string_view name);
    Result<std::vector<std::string>> configured_tool_names();
    Result<std::string> configured_tools_directory();
    Result<std::string> configured_theme();
    void apply_theme_overrides(terminal::Theme& theme, std::vector<std::string>& warnings);
    void apply_ui_config(terminal::Theme& theme, std::vector<std::string>& warnings);

private:
    struct LuaCommand {
        sol::protected_function handler;
        std::filesystem::path source;
        std::string description;
        std::string usage;
        cli::CommandHelp help;
        std::vector<std::string> aliases;
    };

    void register_api();
    void bind_ui();
    void register_command(std::string name, sol::protected_function handler);
    void register_completion_provider(std::string name, sol::protected_function handler);
    Status load_directory(const std::filesystem::path& directory, bool best_effort = false);

    // Lua references are destroyed before their owning state (reverse member order).
    std::unique_ptr<sol::state> state_;
    Error initialization_error_{};
    std::unordered_map<std::string, LuaCommand> commands_;
    std::unordered_map<std::string, sol::protected_function> completion_providers_;
    std::filesystem::path current_script_;
    bool config_loaded_{};
    bool extensions_loaded_{};
    terminal::Ui* ui_{};
    sol::table ui_api_;
};

} // namespace atlas::lua
