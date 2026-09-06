# Build

Atlas targets Windows and uses C++23, Lua 5.4, sol2, CMake, and MSVC. Install Visual Studio 2022 with the C++ workload, CMake 3.25 or newer, and vcpkg. Set `VCPKG_ROOT`; the manifest supplies Lua and sol2.

```powershell
cmake --preset msvc
cmake --build --preset release
cmake --install out/build/msvc --config Release --prefix D:/Tools/atlas
```

The install prefix contains `bin/atlas.exe`, themes in `share/atlas/themes`, Lua command assets in `share/atlas/commands`, and the PowerShell adapter in `share/atlas/shell`.

Personal configuration stays separate under `%USERPROFILE%/.atlas`, including `config.lua`, themes, commands, plugins, and completion state. Use `cmake --install` for distribution so the executable and its `share/atlas` resources remain together.
