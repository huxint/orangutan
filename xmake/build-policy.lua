-- Shared project policy. Package builds only inherit compiler discovery.

rule("oran.build")
    on_load(function (target)
        -- GCC requires explicit reflection support for core enum wire names.
        target:add("cxxflags",
            "-freflection",
            "-fdiagnostics-color=always",
            "-fdiagnostics-show-template-tree",
            "-pipe",
            "-fno-plt",
            "-fno-common",
            "-fmacro-prefix-map=" .. os.projectdir() .. "=.",
            { force = true }
        )

        -- Xmake applies these policies to compilation and final linking.
        target:set("policy", "build.optimization.lto", has_config("lto") and is_mode("release"))
        local sanitizers = has_config("sanitizers") and is_mode("debug")
        target:set("policy", "build.sanitizer.address", sanitizers)
        target:set("policy", "build.sanitizer.undefined", sanitizers)
        if sanitizers then
            target:add("cxxflags", "-fno-omit-frame-pointer", "-fno-sanitize-recover=all", { force = true })
        end

        if has_config("hardened") then
            target:add("cxxflags",
                "-D_FORTIFY_SOURCE=3",
                "-fstack-protector-strong",
                "-fcf-protection",
                "-fstack-clash-protection",
                { force = true }
            )
        end
        if has_config("analyze") then
            -- docs/rules/static-analysis.md owns the required warning set.
            target:add("cxxflags",
                "-fanalyzer",
                "-Werror=analyzer-null-dereference",
                "-Werror=analyzer-use-after-free",
                "-Werror=analyzer-double-free",
                "-Werror=analyzer-malloc-leak",
                "-Werror=analyzer-tainted-allocation-size",
                "-Werror=analyzer-tainted-array-index",
                "-Werror=analyzer-out-of-bounds",
                "-Werror=analyzer-write-to-string-literal",
                "-Werror=analyzer-fd-leak",
                "-Werror=analyzer-fd-use-without-check",
                { force = true }
            )
        end
    end)
rule_end()
