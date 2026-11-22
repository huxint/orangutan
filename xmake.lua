-- Orangutan build root. docs/BUILD_SYSTEM.md owns the build contract.

set_project("orangutan-v2")
set_version("2.0.0")
set_languages("c++26")
set_warnings("all", "extra")

add_rules("mode.debug", "mode.release", "mode.releasedbg")
add_rules("plugin.compile_commands.autoupdate", { outputdir = ".", lsp = "clangd" })

set_policy("package.requires_lock", true)
set_policy("build.warning", true)

includes("xmake/options.lua")
includes("xmake/toolchain.lua")
includes("xmake/build-policy.lua")

-- Root selection and policy apply to every included project target.
set_toolchains("oran-gcc")
add_rules("oran.build")

includes("xmake/packages.lua")
includes("xmake/targets.lua")
includes("xmake/tests.lua")
includes("xmake/bench.lua")

includes("xmake/evals.lua")
includes("xmake/apps.lua")
