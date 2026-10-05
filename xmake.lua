set_xmakever("3.0.0")
set_project("PeaceSkyrim")
set_version("0.11.2")
set_license("GPL-3.0-or-later")
set_plat("windows")
set_arch("x64")
set_languages("c++23")
set_runtimes("MD")
add_rules("mode.debug", "mode.releasedbg")
set_defaultmode("releasedbg")

-- Desktop SE + AE. VR requires separate implementation and validation.
set_config("skyrim_se", true)
set_config("skyrim_ae", true)
set_config("skyrim_vr", false)
set_config("tests", false)
includes("lib/CommonLibSSE-NG")

target("PeaceSkyrim")
    add_rules("commonlibsse-ng.plugin", {
        name = "PeaceSkyrim",
        author = "bmeye",
        description = "A peaceful Skyrim experience"
    })
    add_files("src/*.cpp")
    after_build(function(target)
        os.cp("config/PeaceSkyrim.ini", path.join(os.projectdir(), "dist", "Data", "SKSE", "Plugins", "PeaceSkyrim.ini"))
    end)
    add_includedirs("include")
    set_pcxxheader("include/PCH.h")
    set_warnings("allextra")
    -- Always stage locally, even if machine-wide deployment variables exist.
    on_config(function(target)
        target:set("installdir", path.join(os.projectdir(), "dist", "Data"))
    end)

target("RetaliationTests")
    set_kind("binary")
    set_default(false)
    add_files("tests/RetaliationState.cpp")
    add_includedirs("include")

target("MountSetterABITests")
    set_kind("binary")
    set_default(false)
    add_files("tests/MountSetterABI.cpp")
    add_includedirs("include")
