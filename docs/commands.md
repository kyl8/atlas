# Commands

Atlas keeps command metadata in one registry. Help, aliases, options, and completion use the same source for C++ and Lua commands.

## Search

`atlas search <query>` searches indexed files and folders with Everything. Literal names receive Atlas fuzzy ranking; expressions such as `*.cpp` and `ext:lua` keep Everything's native behavior.

```powershell
atlas search waluna
atlas search "*.cpp" --files -n 20
atlas search waluna --here
atlas search waluna --here --tree
atlas search main --exact
```

`--here` restricts the search to the current directory. With no query, it browses that directory. Use `--tree`, `--files`, `--dirs`, `--limit`, `--depth`, `--all`, `--path`, and `--sort` to refine results. `--status` reports the Everything service and index. `find` is an alias for `search`.

## Files

`atlas ls [path]` lists one directory with mode, date, size, and name. `atlas tree [path]` recursively explores a bounded tree. `atlas open <path-or-url>` delegates to the Windows Shell.

## Tools, themes, and scripts

`atlas setup <directory>` creates the user folders and records the external tools directory without overwriting `config.lua`. `atlas tools` lists configured tools and resolved paths; `atlas tool <name>` inspects one tool.

`atlas theme` lists themes, `atlas theme set <name>` selects one, and `atlas theme preview [name]` previews it. `atlas completion powershell|bash|zsh|fish` prints a shell adapter. `atlas run <file.lua>` executes one Lua script in a fresh runtime.

## Output

Interactive terminals get the themed renderer. Redirected output stays predictable: data goes to `stdout`, progress and diagnostics to `stderr`. Use `--plain`, `--json`, or `--raw` for scripts and pipes. Global controls are `--color auto|always|never`, `--quiet`, `--verbose`, and `--debug`; command short options can be grouped, such as `-hf` for `--here --files`.
