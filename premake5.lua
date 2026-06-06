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
    links { "dwmapi", "windowsapp" }

  filter {}

  project "screencap"
    kind "WindowedApp"
    files { "src/**.cc", "src/**.h", "ext/**.h", "src/resources.rc" }
    includedirs { "src", "ext" }

    pchheader "pch.h"
    pchsource "src/pch.cc"

    filter "files:**.rc"
      buildaction "ResourceCompile"