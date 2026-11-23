-- Personal Agent host executable (built by default) plus opt-in channel hosts.
local root = os.projectdir()

target("orangutan")
    set_kind("binary")
    set_group("oran-apps")
    set_default(true)
    add_includedirs(path.join(root, "include"))
    add_files(path.join(root, "apps/personal/*.cpp"))
    add_deps("oran-bootstrap")
    add_packages("nlohmann_json", "libsodium")
    set_pcxxheader(path.join(root, "include/oran/_pch.hpp"))
target_end()

-- Opt-in pure C++26 + Vulkan native Personal Agent desktop application.
-- Uses system SDL3, FreeType2, Fontconfig, libpng16, and dynamically loaded libvulkan.
target("orangutan-desktop")
    set_kind("binary")
    set_group("oran-apps")
    set_default(false)
    add_includedirs(path.join(root, "include"), path.join(root, "apps/personal"))
    add_sysincludedirs("/usr/include/freetype2", "/usr/include/libpng16")
    add_files(
        path.join(root, "apps/desktop-vk/*.cpp"),
        path.join(root, "apps/personal/local_provider.cpp"),
        path.join(root, "apps/personal/reminders.cpp")
    )
    add_deps("oran-bootstrap")
    add_packages("nlohmann_json", "libsodium")
    add_syslinks("SDL3", "freetype", "fontconfig", "png16", "dl")
    set_pcxxheader(path.join(root, "include/oran/_pch.hpp"))
target_end()

target("oran-telegram")
    set_kind("binary")
    set_group("oran-apps")
    set_default(false)
    add_includedirs(path.join(root, "include"))
    add_files(path.join(root, "apps/telegram/*.cpp"))
    add_deps("oran-bootstrap")
    add_packages("nlohmann_json", "cmark", "libsodium")
    set_pcxxheader(path.join(root, "include/oran/_pch.hpp"))
target_end()

target("oran-qq-login")
    set_kind("binary")
    set_group("oran-apps")
    set_default(false)
    add_includedirs(path.join(root, "include"))
    add_files(path.join(root, "apps/qq/main.cpp"), path.join(root, "apps/qq/login.cpp"))
    add_deps("oran-bootstrap")
    add_packages("nlohmann_json", "libsodium", "libqrencode")
    set_pcxxheader(path.join(root, "include/oran/_pch.hpp"))
target_end()
