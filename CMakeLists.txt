cmake_minimum_required(VERSION 3.13)

set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)

# Fallout 1 Realtime release: link the C/C++ runtime STATICALLY, so the exe runs on a PC without the
# Visual C++ redistributable (otherwise: "MSVCP140.dll was not found"). CMP0091 decides how the
# runtime flags are made and must be NEW before project(); the policy default carries it into SDL2
# and the other subprojects, whose own cmake_minimum_required predates it. Release tree only -- the
# development build is untouched. FALLOUT_RELEASE is read here from the -D on the command line.
if(FALLOUT_RELEASE)
    if(POLICY CMP0091)
        cmake_policy(SET CMP0091 NEW)
    endif()
    set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(EXECUTABLE_NAME fallout-ce)

if (APPLE)
    if(IOS)
        set(CMAKE_OSX_DEPLOYMENT_TARGET "12" CACHE STRING "")
        set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "")
    else()
        set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "")
        set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "")
    endif()
endif()

project(${EXECUTABLE_NAME})

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED YES)
set(CMAKE_CXX_EXTENSIONS NO)

option(ASAN "Enable address sanitizer" OFF)
option(UBSAN "Enable undefined behaviour sanitizer" OFF)
# Fallout 1 Realtime: the PUBLIC build. Same source as the development build; the diagnostics stay
# compiled in but default OFF (trace_log=1 in fallout.cfg still turns the flight recorder on for a
# bug report), and the exe is named fallout1-realtime. Configure it into its own tree:
#   cmake -S . -B build-release -DFALLOUT_RELEASE=ON
option(FALLOUT_RELEASE "Fallout 1 Realtime public build: diagnostics default off" OFF)

if (ANDROID)
    add_library(${EXECUTABLE_NAME} SHARED)
else()
    add_executable(${EXECUTABLE_NAME} WIN32 MACOSX_BUNDLE)
endif()

if(ASAN)
    if(MSVC)
        # MSVC spells it /fsanitize=address and has no -fsanitize-recover equivalent (use the
        # ASAN_OPTIONS env var at runtime instead). ASan is incompatible with incremental linking
        # and with edit-and-continue debug info, so /Zi + /INCREMENTAL:NO are pinned here; the
        # sanitizer runtime is added by the linker automatically. Only OUR target is instrumented
        # -- SDL and the other fetched deps stay plain, which is fine: the interceptors live in
        # the runtime, so allocations made anywhere are still checked.
        target_compile_options(${EXECUTABLE_NAME} PRIVATE /fsanitize=address /Zi)
        target_link_options(${EXECUTABLE_NAME} PRIVATE /INCREMENTAL:NO /DEBUG)
    else()
        target_compile_options(${EXECUTABLE_NAME} PUBLIC "-fsanitize=address;-fsanitize-recover=address")
        target_link_options(${EXECUTABLE_NAME} PUBLIC "-fsanitize=address;-fsanitize-recover=address")
    endif()
endif()
if(UBSAN)
    target_compile_options(${EXECUTABLE_NAME} PUBLIC "-fsanitize=undefined")
    target_link_options(${EXECUTABLE_NAME} PUBLIC "-fsanitize=undefined")
endif()

target_include_directories(${EXECUTABLE_NAME} PUBLIC src)

target_sources(${EXECUTABLE_NAME} PUBLIC
    "src/game/actions.cc"
    "src/game/actions.h"
    "src/game/amutex.cc"
    "src/game/amutex.h"
    "src/game/anim.cc"
    "src/game/anim.h"
    "src/game/art.cc"
    "src/game/art.h"
    "src/game/automap.cc"
    "src/game/automap.h"
    "src/game/bmpdlog.cc"
    "src/game/bmpdlog.h"
    "src/game/autotest.cc"
    "src/game/autotest.h"
    "src/game/cache.cc"
    "src/game/cache.h"
    "src/game/combat_defs.h"
    "src/game/combat.cc"
    "src/game/combat.h"
    "src/game/combat_rt.cc"
    "src/game/combat_rt.h"
    "src/game/combatai.cc"
    "src/game/combatai.h"
    "src/game/config.cc"
    "src/game/config.h"
    "src/game/credits.cc"
    "src/game/credits.h"
    "src/game/critter.cc"
    "src/game/critter.h"
    "src/game/cycle.cc"
    "src/game/cycle.h"
    "src/game/display.cc"
    "src/game/display.h"
    "src/game/editor.cc"
    "src/game/editor.h"
    "src/game/elevator.cc"
    "src/game/elevator.h"
    "src/game/endgame.cc"
    "src/game/endgame.h"
    "src/game/fontmgr.cc"
    "src/game/fontmgr.h"
    "src/game/game_vars.h"
    "src/game/game.cc"
    "src/game/game.h"
    "src/game/game_trace.cc"
    "src/game/game_trace.h"
    "src/game/gamepad_ui.cc"
    "src/game/gamepad_ui.h"
    "src/game/interact_prompt.cc"
    "src/game/interact_prompt.h"
    "src/game/radial_wheel.cc"
    "src/game/radial_wheel.h"
    "src/game/gconfig.cc"
    "src/game/gconfig.h"
    "src/game/gdebug.cc"
    "src/game/gdebug.h"
    "src/game/gdialog.cc"
    "src/game/gdialog.h"
    "src/game/gmemory.cc"
    "src/game/gmemory.h"
    "src/game/gmouse.cc"
    "src/game/gmouse.h"
    "src/game/gmovie.cc"
    "src/game/gmovie.h"
    "src/game/graphlib.cc"
    "src/game/graphlib.h"
    "src/game/gsound.cc"
    "src/game/gsound.h"
    "src/game/heap.cc"
    "src/game/heap.h"
    "src/game/intface.cc"
    "src/game/intface.h"
    "src/game/inventry.cc"
    "src/game/inventry.h"
    "src/game/item.cc"
    "src/game/item.h"
    "src/game/light.cc"
    "src/game/light.h"
    "src/game/lip_sync.cc"
    "src/game/lip_sync.h"
    "src/game/loadsave.cc"
    "src/game/loadsave.h"
    "src/game/main.cc"
    "src/game/main.h"
    "src/game/mainmenu.cc"
    "src/game/mainmenu.h"
    "src/game/map_defs.h"
    "src/game/map.cc"
    "src/game/map.h"
    "src/game/message.cc"
    "src/game/message.h"
    "src/game/mods_page.cc"
    "src/game/mods_page.h"
    "src/game/moviefx.cc"
    "src/game/moviefx.h"
    "src/game/object_types.h"
    "src/game/object.cc"
    "src/game/object.h"
    "src/game/options.cc"
    "src/game/options.h"
    "src/game/palette.cc"
    "src/game/palette.h"
    "src/game/party.cc"
    "src/game/party.h"
    "src/game/perk_defs.h"
    "src/game/perk.cc"
    "src/game/perk.h"
    "src/game/pipboy.cc"
    "src/game/pipboy.h"
    "src/game/protinst.cc"
    "src/game/protinst.h"
    "src/game/proto_types.h"
    "src/game/proto.cc"
    "src/game/proto.h"
    "src/game/queue.cc"
    "src/game/queue.h"
    "src/game/reaction.cc"
    "src/game/reaction.h"
    "src/game/roll.cc"
    "src/game/roll.h"
    "src/game/scripts.cc"
    "src/game/scripts.h"
    "src/game/select.cc"
    "src/game/select.h"
    "src/game/selfrun.cc"
    "src/game/selfrun.h"
    "src/game/sfxcache.cc"
    "src/game/sfxcache.h"
    "src/game/sfxlist.cc"
    "src/game/sfxlist.h"
    "src/game/skill_defs.h"
    "src/game/skill.cc"
    "src/game/skill.h"
    "src/game/skilldex.cc"
    "src/game/skilldex.h"
    "src/game/stat_defs.h"
    "src/game/stat.cc"
    "src/game/stat.h"
    "src/game/textobj.cc"
    "src/game/textobj.h"
    "src/game/tile.cc"
    "src/game/tile.h"
    "src/game/trait.cc"
    "src/game/trait.h"
    "src/game/version.cc"
    "src/game/version.h"
    "src/game/wordwrap.cc"
    "src/game/wordwrap.h"
    "src/game/worldmap_walkmask.cc"
    "src/game/worldmap_walkmask.h"
    "src/game/worldmap.cc"
    "src/game/worldmap.h"
    "src/int/audio.cc"
    "src/int/audio.h"
    "src/int/audiof.cc"
    "src/int/audiof.h"
    "src/int/datafile.cc"
    "src/int/datafile.h"
    "src/int/dialog.cc"
    "src/int/dialog.h"
    "src/int/export.cc"
    "src/int/export.h"
    "src/int/intlib.cc"
    "src/int/intlib.h"
    "src/int/intrpret.cc"
    "src/int/intrpret.h"
    "src/int/memdbg.cc"
    "src/int/memdbg.h"
    "src/int/mousemgr.cc"
    "src/int/mousemgr.h"
    "src/int/movie.cc"
    "src/int/movie.h"
    "src/int/nevs.cc"
    "src/int/nevs.h"
    "src/int/pcx.cc"
    "src/int/pcx.h"
    "src/int/region.cc"
    "src/int/region.h"
    "src/int/share1.cc"
    "src/int/share1.h"
    "src/int/sound.cc"
    "src/int/sound.h"
    "src/int/support/intextra.cc"
    "src/int/support/intextra.h"
    "src/int/widget.cc"
    "src/int/widget.h"
    "src/int/window.cc"
    "src/int/window.h"
    "src/plib/assoc/assoc.cc"
    "src/plib/assoc/assoc.h"
    "src/plib/color/color.cc"
    "src/plib/color/color.h"
    "src/plib/db/db.cc"
    "src/plib/db/db.h"
    "src/plib/db/lzss.cc"
    "src/plib/db/lzss.h"
    "src/plib/gnw/button.cc"
    "src/plib/gnw/button.h"
    "src/plib/gnw/debug.cc"
    "src/plib/gnw/debug.h"
    "src/plib/gnw/dxinput.cc"
    "src/plib/gnw/dxinput.h"
    "src/plib/gnw/grbuf.cc"
    "src/plib/gnw/grbuf.h"
    "src/plib/gnw/input.cc"
    "src/plib/gnw/input.h"
    "src/plib/gnw/gnw_types.h"
    "src/plib/gnw/gnw.cc"
    "src/plib/gnw/gnw.h"
    "src/plib/gnw/intrface.cc"
    "src/plib/gnw/intrface.h"
    "src/plib/gnw/kb.cc"
    "src/plib/gnw/kb.h"
    "src/plib/gnw/memory.cc"
    "src/plib/gnw/memory.h"
    "src/plib/gnw/mouse.cc"
    "src/plib/gnw/mouse.h"
    "src/plib/gnw/rect.cc"
    "src/plib/gnw/rect.h"
    "src/plib/gnw/crt_shader.cc"
    "src/plib/gnw/crt_shader.h"
    "src/plib/gnw/svga_types.h"
    "src/plib/gnw/svga.cc"
    "src/plib/gnw/svga.h"
    "src/plib/gnw/text.cc"
    "src/plib/gnw/text.h"
    "src/plib/gnw/vcr.cc"
    "src/plib/gnw/vcr.h"
    "src/plib/gnw/winmain.cc"
    "src/plib/gnw/winmain.h"
    "src/movie_lib.cc"
    "src/movie_lib.h"
)

target_sources(${EXECUTABLE_NAME} PUBLIC
    "src/audio_engine.cc"
    "src/audio_engine.h"
    "src/fps_limiter.cc"
    "src/fps_limiter.h"
    "src/platform_compat.cc"
    "src/platform_compat.h"
    "src/pointer_registry.cc"
    "src/pointer_registry.h"
    "src/plib/gnw/touch.cc"
    "src/plib/gnw/touch.h"
)

if(IOS)
    target_sources(${EXECUTABLE_NAME} PUBLIC
        "src/platform/ios/paths.h"
        "src/platform/ios/paths.mm"
    )
endif()

if(WIN32)
    target_compile_definitions(${EXECUTABLE_NAME} PUBLIC
        _CRT_SECURE_NO_WARNINGS
        _CRT_NONSTDC_NO_WARNINGS
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
endif()

if(WIN32)
    target_link_libraries(${EXECUTABLE_NAME}
        winmm
        dbghelp
    )
endif()

if(FALLOUT_RELEASE)
    target_compile_definitions(${EXECUTABLE_NAME} PRIVATE FALLOUT_RELEASE=1)
    if(NOT APPLE)
        set_target_properties(${EXECUTABLE_NAME} PROPERTIES OUTPUT_NAME "fallout1-realtime")
    endif()
endif()

# Fallout 1 Realtime's Windows installer: ONE exe carrying the game and data\aimmask.dat as
# resources, which installs them into an existing Fallout 1 (on Steam it takes FalloutLauncher.exe's
# place, so Steam's Play button starts the game -- see installer/setup.cc). Release tree only:
#   cmake --build build-release --config Release --target fallout1-realtime-setup
# The payload .rc is generated per configuration because it names the game exe of THAT build.
if(FALLOUT_RELEASE AND WIN32)
    set(FALLOUT_RT_AIMMASK "${CMAKE_SOURCE_DIR}/build/Release/data/aimmask.dat" CACHE FILEPATH
        "The painted aim masks (data/aimmask.dat) packed into the installer")
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/setup_payload_$<CONFIG>.rc" CONTENT
"1 ICON \"${CMAKE_SOURCE_DIR}/os/windows/fallout-ce.ico\"
101 RCDATA \"$<TARGET_FILE:${EXECUTABLE_NAME}>\"
102 RCDATA \"${FALLOUT_RT_AIMMASK}\"
")
    add_executable(fallout1-realtime-setup WIN32
        "installer/setup.cc"
        "installer/setup.manifest"
        "${CMAKE_CURRENT_BINARY_DIR}/setup_payload_$<CONFIG>.rc"
    )
    target_include_directories(fallout1-realtime-setup PRIVATE "src")
    target_compile_definitions(fallout1-realtime-setup PRIVATE
        UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(fallout1-realtime-setup comctl32 ole32 shell32 advapi32 uuid)
    set_target_properties(fallout1-realtime-setup PROPERTIES OUTPUT_NAME "Fallout1-Realtime-Setup")
    # The payload is the game exe: build it first, and re-pack whenever it changes.
    add_dependencies(fallout1-realtime-setup ${EXECUTABLE_NAME})
    set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/setup_payload_$<CONFIG>.rc" PROPERTIES
        OBJECT_DEPENDS "$<TARGET_FILE:${EXECUTABLE_NAME}>;${FALLOUT_RT_AIMMASK}")
endif()

# Emit a PDB even for Release so the game_trace crash handler can resolve symbolic
# stack frames (function + file:line). /Zi adds debug info, /DEBUG generates the PDB,
# and /OPT:REF,ICF restore the size optimizations that /DEBUG otherwise disables.
if(MSVC)
    target_compile_options(${EXECUTABLE_NAME} PRIVATE $<$<CONFIG:Release>:/Zi>)
    target_link_options(${EXECUTABLE_NAME} PRIVATE $<$<CONFIG:Release>:/DEBUG /OPT:REF /OPT:ICF>)
endif()

if (WIN32)
    target_sources(${EXECUTABLE_NAME} PUBLIC
        "os/windows/fallout-ce.ico"
        "os/windows/fallout-ce.rc"
    )
endif()

if(APPLE)
    if(IOS)
        set(RESOURCES
            "os/ios/AppIcon.xcassets"
            "os/ios/LaunchScreen.storyboard"
        )

        target_sources(${EXECUTABLE_NAME} PUBLIC ${RESOURCES})
        set_source_files_properties(${RESOURCES} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")

        set_target_properties(${EXECUTABLE_NAME} PROPERTIES
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/os/ios/Info.plist"
            XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME "AppIcon"
            XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.alexbatalov.fallout-ce"
            XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
        )

        set(MACOSX_BUNDLE_BUNDLE_NAME "${EXECUTABLE_NAME}")
        set(MACOSX_BUNDLE_DISPLAY_NAME "Fallout")
    else()
        set(RESOURCES
            "os/macos/fallout-ce.icns"
        )

        target_sources(${EXECUTABLE_NAME} PUBLIC ${RESOURCES})
        set_source_files_properties(${RESOURCES} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")

        set_target_properties(${EXECUTABLE_NAME} PROPERTIES
            OUTPUT_NAME "Fallout Community Edition"
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/os/macos/Info.plist"
            XCODE_ATTRIBUTE_EXECUTABLE_NAME "${EXECUTABLE_NAME}"
            XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.alexbatalov.fallout-ce"
        )

        set(MACOSX_BUNDLE_ICON_FILE "fallout-ce.icns")
        set(MACOSX_BUNDLE_BUNDLE_NAME "Fallout: Community Edition")
        set(MACOSX_BUNDLE_DISPLAY_NAME "Fallout")
    endif()

    set(MACOSX_BUNDLE_GUI_IDENTIFIER "com.alexbatalov.fallout-ce")
    set(MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0.0")
    set(MACOSX_BUNDLE_BUNDLE_VERSION "1.0.0")
endif()

if((NOT ${CMAKE_SYSTEM_NAME} MATCHES "Linux") AND (NOT ${CMAKE_SYSTEM_NAME} MATCHES "FreeBSD") AND (NOT ${CMAKE_SYSTEM_NAME} MATCHES "OpenBSD"))
    add_subdirectory("third_party/sdl2")
else()
    find_package(SDL2)
endif()

add_subdirectory("third_party/adecode")
target_link_libraries(${EXECUTABLE_NAME} adecode::adecode)

add_subdirectory("third_party/fpattern")
target_link_libraries(${EXECUTABLE_NAME} fpattern::fpattern)

target_link_libraries(${EXECUTABLE_NAME} ${SDL2_LIBRARIES})
target_include_directories(${EXECUTABLE_NAME} PRIVATE ${SDL2_INCLUDE_DIRS})

if(APPLE)
    if(IOS)
        install(TARGETS ${EXECUTABLE_NAME} DESTINATION "Payload")

        set(CPACK_GENERATOR "ZIP")
        set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
        set(CPACK_PACKAGE_FILE_NAME "fallout-ce")
        set(CPACK_ARCHIVE_FILE_EXTENSION "ipa")
    else()
        install(TARGETS ${EXECUTABLE_NAME} DESTINATION .)

        set(CPACK_GENERATOR "DragNDrop")
        set(CPACK_DMG_DISABLE_APPLICATIONS_SYMLINK ON)
        set(CPACK_PACKAGE_FILE_NAME "Fallout Community Edition")
    endif()

    include(CPack)
endif()
