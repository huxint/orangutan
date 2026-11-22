-- Compiler and binutils discovery only; project flags belong to oran.build.
-- Use compiler drivers for assembly so dependency .S files are preprocessed.

toolchain("oran-gcc")
    set_homepage("https://gcc.gnu.org/")
    set_description("GCC 16.1+ for Orangutan")
    set_kind("standalone")
    set_runtimes("stdc++_static", "stdc++_shared")
    set_toolset("cc", "gcc-16", "gcc")
    set_toolset("cxx", "g++-16", "g++")
    set_toolset("ld", "g++-16", "g++")
    set_toolset("sh", "g++-16", "g++")
    set_toolset("as", "gcc-16", "gcc")
    -- Keep ar semantics for xmake's Autoconf adapter, with GCC's LTO plugin.
    set_toolset("ar", "ar@gcc-ar-16", "ar@gcc-ar", "ar")
    set_toolset("ranlib", "gcc-ranlib-16", "gcc-ranlib", "ranlib")
    set_toolset("strip", "strip")
    set_toolset("objcopy", "objcopy")
toolchain_end()
