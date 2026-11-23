cmake_minimum_required(VERSION 3.24)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" ZR_VERSION_TEXT)
string(STRIP "${ZR_VERSION_TEXT}" ZR_VERSION_TEXT)
project(ZetRo VERSION ${ZR_VERSION_TEXT} LANGUAGES C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
include(cmake/Warnings.cmake)
if(MSVC)
    string(REPLACE "/Ob1" "/Ob2" CMAKE_C_FLAGS_RELWITHDEBINFO "${CMAKE_C_FLAGS_RELWITHDEBINFO}")
endif()
configure_file(zr_version.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/zr_version.h @ONLY)
option(ZR_LIBRETRO_ONLY "Configure only the ZetRo Libretro core" OFF)

file(GLOB ZR_YAML_SOURCES third_party/libyaml/src/*.c)
add_library(zr_yaml STATIC ${ZR_YAML_SOURCES})
target_include_directories(zr_yaml PUBLIC third_party/libyaml/include)
target_compile_definitions(zr_yaml PUBLIC YAML_DECLARE_STATIC PRIVATE
    YAML_VERSION_MAJOR=0 YAML_VERSION_MINOR=2 YAML_VERSION_PATCH=5 YAML_VERSION_STRING="0.2.5"
    _CRT_SECURE_NO_WARNINGS)

# Everything OS-specific: one implementation per OS.
if(WIN32)
    add_library(zr_host STATIC host/host_win32.c)
    target_link_libraries(zr_host PUBLIC ws2_32)
else()
    add_library(zr_host STATIC host/host_posix.c)
endif()
zr_set_warnings(zr_host)

# The emulator core: no SDL, no OS header, no wall clock. The chips (hw/),
# the media formats (core/media/) and the boards (core/machines/) join it
# as they are written.
add_library(zr_core STATIC
    core/config.c core/yamledit.c core/romdb.c core/font8x8.c core/text.c core/state.c core/media.c
    core/machine.c core/stub_machine.c core/stick.c
    core/media/inflate.c core/media/tape.c core/media/tape_gen.c core/media/tape_deck.c core/media/tape_rec.c
    core/media/tape_tap.c core/media/tape_tzx.c core/media/tape_pzx.c core/media/tape_csw.c core/media/tape_wav.c
    core/media/tape_write.c
    core/media/disk.c core/media/disk_dsk.c core/media/disk_raw.c
    core/media/snap.c core/media/snap_sna.c core/media/snap_z80.c core/media/snap_szx.c core/media/pok.c
    hw/sched.c hw/bus.c hw/z80.c hw/traps.c hw/loadaccel.c
    core/machines/spectrum.c core/machines/spectrum_snap.c core/machines/jet.c core/machines/hc.c
    core/machines/hc128.c core/machines/hc2000.c
    core/machines/cip.c
    hw/i8255.c hw/i8251.c hw/i8253.c
    hw/upd765.c hw/fdd.c hw/drivesnd.c hw/if1.c hw/dandanator.c
    hw/beeper.c hw/ay8912.c hw/ula.c)
target_include_directories(zr_core PUBLIC core hw PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(zr_core PUBLIC zr_yaml zr_host)
if(NOT WIN32)
    target_link_libraries(zr_core PUBLIC m)
endif()
# The Z80 family's peripherals and the 8257.
target_sources(zr_core PRIVATE hw/z80ctc.c hw/z80pio.c hw/z80sio.c hw/i8257.c)
# The HC-88 (P6c): its video and its board.
target_sources(zr_core PRIVATE hw/hc88video.c core/machines/hc88.c)
# The TIM-S (P6f): its board.
target_sources(zr_core PRIVATE core/machines/tims.c hw/timsvideo.c core/machines/timsplus.c)
# The CoBra (P6h): its video and its board.
target_sources(zr_core PRIVATE hw/cobravideo.c core/machines/cobra.c)
# The CIP-04 (P6e), experimental.
target_sources(zr_core PRIVATE core/machines/cip04.c)
# The CUB Z (P7b): its video, its keyboard and its board.
target_sources(zr_core PRIVATE hw/cubzvideo.c hw/cubzkbd.c core/machines/cubz.c)
# The aMIC and the PRAE-M (P7a): the video they share, what their boards share, and the boards.
target_sources(zr_core PRIVATE hw/monovideo.c core/machines/native.c core/machines/amic.c core/machines/prae.c)
# The storage models (P8a, P8d): the image behind a hard disk or a card, the IDE channel, the SD card.
target_sources(zr_core PRIVATE core/media/card.c hw/ata.c hw/mmc.c)
# The DivIDE and the DivMMC (P8b): the paging they share, and each interface.
target_sources(zr_core PRIVATE hw/divmem.c hw/divide.c hw/divmmc.c)
zr_set_warnings(zr_core)

set_target_properties(zr_core zr_yaml zr_host PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
add_library(zetro_libretro SHARED libretro/zetro_libretro.c app/typist.c)
target_include_directories(zetro_libretro PRIVATE third_party/libretro "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(zetro_libretro PRIVATE zr_core)
set_target_properties(zetro_libretro PROPERTIES PREFIX "" C_VISIBILITY_PRESET hidden)
zr_set_warnings(zetro_libretro)
if(ZR_LIBRETRO_ONLY)
    return()
endif()
set_target_properties(zetro_libretro PROPERTIES EXCLUDE_FROM_ALL TRUE)

# app/zetro.yaml is the default configuration written on first start.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/app/zetro.yaml)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/app/zetro.yaml ZR_DEFAULT_YAML HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," ZR_DEFAULT_YAML "${ZR_DEFAULT_YAML}")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/generated/zetro_default_yaml.h "static const unsigned char zr_default_yaml[]={${ZR_DEFAULT_YAML}};\n")

include(FetchContent)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL2
    URL https://www.libsdl.org/release/SDL2-2.32.10.tar.gz
    URL_HASH SHA256=5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(SDL2)

# The shell's parts without SDL: the desktop loop's pacing, the sound output's clock sync, the harness, the keyboard
# joystick, the typist, the setup screen and its text-cell toolkit, the state files, the rewind ring and the on-screen keyboard.
add_library(zr_shell STATIC app/pace.c app/harness.c app/joykeys.c app/typist.c platform/audiosync.c platform/serial.c app/tui.c app/setup.c app/states.c app/cheats.c app/osk.c)
target_link_libraries(zr_shell PUBLIC zr_core)
zr_set_warnings(zr_shell)

# The platform layer.
add_library(zr_platform STATIC platform/video.c platform/audio.c platform/overlay.c platform/png.c)
target_link_libraries(zr_platform PUBLIC zr_core zr_shell SDL2::SDL2-static)
target_compile_definitions(zr_platform PUBLIC SDL_MAIN_HANDLED)
zr_set_warnings(zr_platform)

add_executable(zetro app/zetro.c app/desktop.c app/pads.c)
if(WIN32)
    target_sources(zetro PRIVATE app/zetro.rc)      # the program's icon (assets/zetro.ico, tools/make_logo.py)
    target_sources(zetro PRIVATE app/zetro.manifest) # UTF-8 file names
endif()
target_include_directories(zetro PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(zetro PRIVATE zr_platform)
if(WIN32)
    # A Windows program without a console window of its own (app/zetro.c main).
    set_target_properties(zetro PROPERTIES WIN32_EXECUTABLE TRUE)
    if(MSVC)
        target_link_options(zetro PRIVATE /ENTRY:mainCRTStartup)
    endif()
endif()
zr_set_warnings(zetro)

# The tests (ctest): one short smoke test per part that needs none of the others, plain programs on the core, no SDL.
# tests/verify.py runs the program itself for the rest. They write only under work/tests.
get_filename_component(ZR_WORK "${CMAKE_CURRENT_SOURCE_DIR}/../work" ABSOLUTE)
get_filename_component(ZR_TESTS "${CMAKE_CURRENT_SOURCE_DIR}/../tests" ABSOLUTE)
enable_testing()
file(MAKE_DIRECTORY ${ZR_WORK}/tests)
foreach(test test_core cpu_test test_ports test_ula test_tape test_z80chips test_floppy test_snap test_cip04 test_storage test_div)
    add_executable(${test} ${ZR_TESTS}/${test}.c)
    target_include_directories(${test} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
    target_link_libraries(${test} PRIVATE zr_core)
    target_compile_definitions(${test} PRIVATE ZR_WORK="${ZR_WORK}")
    zr_set_warnings(${test})
    add_test(NAME ${test} COMMAND ${test})
endforeach()

# The HC-88's video (P6c): its own unit check beside the others.
add_executable(test_hc88video ${ZR_TESTS}/test_hc88video.c)
target_include_directories(test_hc88video PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(test_hc88video PRIVATE zr_core)
target_compile_definitions(test_hc88video PRIVATE ZR_WORK="${ZR_WORK}")
zr_set_warnings(test_hc88video)
add_test(NAME test_hc88video COMMAND test_hc88video)

# The aMIC's and the PRAE-M's video (P7a): its own unit check beside the others.
add_executable(test_monovideo ${ZR_TESTS}/test_monovideo.c)
target_include_directories(test_monovideo PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(test_monovideo PRIVATE zr_core)
target_compile_definitions(test_monovideo PRIVATE ZR_WORK="${ZR_WORK}")
zr_set_warnings(test_monovideo)
add_test(NAME test_monovideo COMMAND test_monovideo)

# The TIM-S (P6f): its byte ports and states against the ROM, from the host's side (skipped, code 77, without the ROM).
add_executable(test_timsplus ${ZR_TESTS}/test_timsplus.c)
target_include_directories(test_timsplus PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(test_timsplus PRIVATE zr_shell)
target_compile_definitions(test_timsplus PRIVATE ZR_WORK="${ZR_WORK}")
zr_set_warnings(test_timsplus)
add_test(NAME test_timsplus COMMAND test_timsplus)
# The host ends of the byte ports (P11) on the TIM-S: the layer's behaviour from the host's side (skipped without the ROM).
add_executable(test_hostports ${ZR_TESTS}/test_hostports.c)
target_include_directories(test_hostports PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(test_hostports PRIVATE zr_shell)
target_compile_definitions(test_hostports PRIVATE ZR_WORK="${ZR_WORK}")
zr_set_warnings(test_hostports)
add_test(NAME test_hostports COMMAND test_hostports)
set_tests_properties(test_hostports PROPERTIES SKIP_RETURN_CODE 77)
add_executable(test_tims ${ZR_TESTS}/test_tims.c)
target_include_directories(test_tims PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(test_tims PRIVATE zr_shell)
target_compile_definitions(test_tims PRIVATE ZR_WORK="${ZR_WORK}")
zr_set_warnings(test_tims)
add_test(NAME test_tims COMMAND test_tims)
set_tests_properties(test_tims PROPERTIES SKIP_RETURN_CODE 77)

# The CUB Z (P7b): its video and keyboard, and its byte ports against the monitor EPROM (that part skipped without it).
add_executable(test_cubz ${ZR_TESTS}/test_cubz.c)
target_include_directories(test_cubz PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(test_cubz PRIVATE zr_core)
target_compile_definitions(test_cubz PRIVATE ZR_WORK="${ZR_WORK}")
zr_set_warnings(test_cubz)
add_test(NAME test_cubz COMMAND test_cubz)

# The input: the keyboard joystick, the configuration's controllers and the typist; the state files and the rewind ring
# (on the test machine, no SDL).
foreach(test test_input test_states test_osk)
    add_executable(${test} ${ZR_TESTS}/${test}.c)
    target_include_directories(${test} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
    target_link_libraries(${test} PRIVATE zr_shell)
    target_compile_definitions(${test} PRIVATE ZR_WORK="${ZR_WORK}")
    zr_set_warnings(${test})
    add_test(NAME ${test} COMMAND ${test})
endforeach()

# The CPU conformance runner (tests/cpu_suite.py run): not a ctest test, the suite is downloaded on request.
add_executable(cpu_suite ${ZR_TESTS}/cpu_suite.c)
target_link_libraries(cpu_suite PRIVATE zr_core)
zr_set_warnings(cpu_suite)
