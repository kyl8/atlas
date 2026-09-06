# Lua

Atlas embeds Lua 5.4 through sol2. Lua handles configuration, user commands, plugins, themes, and completion providers. C++ still owns process execution, filesystem access, Windows integration, and command dispatch.

## Loading

Atlas reads `%USERPROFILE%/.atlas/config.lua` when configuration is needed. It then loads `.lua` files from `.atlas/commands` and `.atlas/plugins` when a command, help page, or home screen needs them. Files are executed with protected calls; a bad script becomes an Atlas error instead of a crash. `atlas --version` stays on the fast C++ path.

## Commands

Register metadata so help and completion know how to describe the command:

```lua
atlas.command({
    name = "hello",
    description = "Say hello",
    category = "Personal",
    usage = "atlas hello [name]",
    aliases = { "hi" },
    arguments = { { name = "name" } },
    run = function(args)
        atlas.ui.success("Hello " .. (args[1] or "there"))
    end
})
```

The short form also works:

```lua
atlas.command("hello", function(args)
    atlas.ui.info("Hello " .. (args[1] or "there"))
end)
```

Handlers receive a one-based positional argument table. Metadata can include `description`, `category`, `usage`, `aliases`, `arguments`, `options`, `examples`, `details`, and `subcommands`.

## Core API

Lua calls the same implementations used by C++:

```lua
local result = atlas.exec("git", { "status", "--short" })
atlas.spawn("code", { "D:/Dev/Waluna" })
atlas.open("D:/Dev/Waluna")

local user = atlas.getenv("USERNAME")
atlas.setenv("ATLAS_MODE", "dev")
```

`atlas.exec` returns `exit_code`, `stdout`, and `stderr`. `atlas.spawn` accepts `hidden`, `new_console`, and `cwd` settings. `atlas.fs` provides `cwd`, `cd`, `exists`, `files`, `glob`, `read`, `write`, `write_atomic`, `resolve`, `is_dir`, and `mkdir`.

Use `atlas.ui` for output instead of ANSI escape sequences:

```lua
atlas.ui.heading("Release", "building Atlas")
atlas.ui.info("Preparing files")
atlas.ui.success("Done")
atlas.ui.warning("Destination already exists")
atlas.ui.error("Build failed")
atlas.ui.path("D:/Dev/Atlas")

local progress = atlas.ui.progress("Downloading")
progress:set_total(100)
progress:set_current(42)
progress:finish("Downloaded")
```

Tables, spinners, sections, muted text, JSON output, and plain output are available through the same `atlas.ui` table. The active theme decides the appearance.

## Completion providers

Providers should be cheap and side-effect free:

```lua
atlas.completion.register("profiles", function()
    return { "debug", "release" }
end)
```

Reference one from argument metadata with `completion = "profiles"`. A provider may return strings or records with `value`, `description`, `kind`, and `score`.

## Home screen

Customize the home screen in `config.lua`:

```lua
atlas.ui.tagline = "A small tool for daily work"
atlas.ui.identity = {
    name = "atlas",
    separator = "  ·  ",
    version = false,
    palette = true
}
atlas.ui.home = {
    actions_title = "Start here",
    show_default_actions = true,
    facts = {
        { label = "User", value = atlas.getenv("USERNAME") or "unknown" }
    },
    actions = {
        { command = "atlas search <query>", description = "Find a file" }
    }
}
```

Set `atlas.ui.tagline = false` to hide the subtitle. Values are validated and adapted to the terminal width.
