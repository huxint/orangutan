-- xmake/options.lua — build-time options.
--
-- Options land with their consuming build policy; see docs/BUILD_SYSTEM.md.

option("lto")
    set_default(true)
    set_description("Enable link-time optimization in release builds.")
option_end()

option("hardened")
    set_default(false)
    set_description("Enable hardening flags (_FORTIFY_SOURCE=3, stack/cf protection).")
option_end()

option("sanitizers")
    set_default(false)
    set_description("Enable ASan/UBSan in debug builds.")
option_end()

option("analyze")
    set_default(false)
    set_description("Enable GCC 16.1 -fanalyzer (see docs/rules/static-analysis.md).")
option_end()
