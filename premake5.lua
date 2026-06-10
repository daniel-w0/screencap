require('premake.embed')

workspace "screencap"
  configurations { "Debug", "Release" }
  platforms { "x64" }
  location "build"

  language "C++"
  cppdialect "C++20"

  targetdir "bin/%{cfg.buildcfg}-%{cfg.platform}"
  objdir "obj/%{cfg.buildcfg}-%{cfg.platform}/%{prj.name}"

  filter "configurations:Debug"
    defines { "SC_DEBUG" }
    runtime "Debug"
    symbols "On"

  filter "configurations:Release"
    defines { "SC_RELEASE" }
    runtime "Release"
    optimize "On"

  filter "platforms:x64"
    architecture "x86_64"

  filter "system:windows"
    defines { "SC_PLATFORM_WINDOWS", "_CRT_SECURE_NO_WARNINGS" }
    links { "dwmapi", "winmm", "uxtheme", "comctl32", "windowsapp", "gdiplus" }

  filter {}

  project "screencap"
    kind "WindowedApp"
    files { "src/**.cc", "src/**.h", "ext/**.h", "src/resources.rc" }
    includedirs { "src", "ext" }

    pchheader "pch.h"
    pchsource "src/pch.cc"

    embed_file("res/locales.ini", "src/embed/locales_data.h", "locales_ini")
    embed_file("res/Shutter_07.wav", "src/embed/screenshot_sound.h", "screenshot_sound")
    embed_file("res/Shutter_09.wav", "src/embed/screenshot_sound_quick.h", "screenshot_sound_quick")

    filter "files:**.rc"
      buildaction "ResourceCompile"