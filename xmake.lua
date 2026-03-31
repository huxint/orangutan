set_project("orangutan")
set_version("0.1.0")
set_languages("c++23")
set_warnings("all")
add_rules("mode.debug", "mode.release")
add_rules("plugin.compile_commands.autoupdate", {outputdir = ".", lsp = "clangd"})
set_policy("package.requires_lock", true)

add_cxxflags("-Wno-system-headers")

includes("xmake/options.lua")
includes("xmake/packages.lua")
includes("xmake/targets.lua")
includes("xmake/tests.lua")
