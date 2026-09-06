cmake_minimum_required(VERSION 3.25)

project(atlas VERSION 1.0.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "Atlas currently supports Windows only")
endif()

find_package(Lua 5.4 REQUIRED)
find_package(sol2 CONFIG REQUIRED)

add_library(atlas_core STATIC
    src/app/app.cpp
    src/cli/parser.cpp
    src/cli/options.cpp
    src/cli/registry.cpp
    src/completion.cpp
    src/fuzzy.cpp
    src/commands/builtins.cpp
    src/commands/completion.cpp
    src/commands/files.cpp
    src/lua/bindings.cpp
    src/lua/errors.cpp
    src/lua/runtime.cpp
    src/lua/theme.cpp
    src/platform/windows/paths.cpp
    src/platform/windows/process.cpp
    src/platform/windows/service.cpp
    src/platform/windows/shell.cpp
    src/platform/windows/utf.cpp
    src/terminal/formatter.cpp
    src/terminal/image.cpp
    src/terminal/layout.cpp
    src/terminal/screens.cpp
    src/terminal/terminal.cpp
    src/terminal/theme.cpp
    src/terminal/ui.cpp
    src/tools.cpp
)

target_include_directories(atlas_core PUBLIC include)
target_include_directories(atlas_core SYSTEM PUBLIC ${LUA_INCLUDE_DIR})
target_link_libraries(atlas_core PUBLIC ${LUA_LIBRARIES} sol2::sol2 PRIVATE Shell32 Windowscodecs Ole32 Advapi32)
target_compile_features(atlas_core PUBLIC cxx_std_23)
target_compile_definitions(atlas_core PUBLIC SOL_ALL_SAFETIES_ON=1 PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

add_executable(atlas src/main.cpp resources/atlas.manifest)
target_link_libraries(atlas PRIVATE atlas_core)

if(MSVC)
    foreach(target atlas_core atlas)
        target_compile_options(${target} PRIVATE /W4 /WX /permissive- /utf-8 /EHsc /Zc:__cplusplus)
    endforeach()
endif()

install(TARGETS atlas RUNTIME DESTINATION bin)
install(DIRECTORY themes/ DESTINATION share/atlas/themes)
install(DIRECTORY scripts/commands/ DESTINATION share/atlas/commands)
install(DIRECTORY scripts/shell/ DESTINATION share/atlas/shell)
install(FILES THIRD_PARTY_NOTICES.md DESTINATION share/atlas)
