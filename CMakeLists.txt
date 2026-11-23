cmake_minimum_required(VERSION 3.20)
project(tinydvd C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(APPLE)
    enable_language(OBJC)
endif()

# Ensure Homebrew and standard paths are considered
if(APPLE)
    list(APPEND CMAKE_PREFIX_PATH "/opt/homebrew" "/usr/local")
    set(_brew_pc "/opt/homebrew/lib/pkgconfig:/usr/local/lib/pkgconfig")
    # mpv's pkg-config file requires libarchive, which Homebrew installs
    # "keg-only" (outside the usual lib/pkgconfig). Without its directory,
    # pkg-config reports mpv as found but returns no link flags
    foreach(_keg "/opt/homebrew/opt/libarchive" "/usr/local/opt/libarchive")
        if(EXISTS "${_keg}/lib/pkgconfig")
            string(APPEND _brew_pc ":${_keg}/lib/pkgconfig")
        endif()
    endforeach()
    if(DEFINED ENV{PKG_CONFIG_PATH})
        set(ENV{PKG_CONFIG_PATH} "${_brew_pc}:$ENV{PKG_CONFIG_PATH}")
    else()
        set(ENV{PKG_CONFIG_PATH} "${_brew_pc}")
    endif()
endif()

find_package(PkgConfig REQUIRED)
if(APPLE AND NOT DEFINED Python3_EXECUTABLE)
    foreach(_py_cand
        "/opt/homebrew/opt/python@3.12/bin/python3.12"
        "/usr/local/opt/python@3.12/bin/python3.12"
        "/opt/homebrew/bin/python3.12"
        "/usr/local/bin/python3.12"
    )
        if(EXISTS "${_py_cand}")
            set(Python3_EXECUTABLE "${_py_cand}")
            break()
        endif()
    endforeach()
endif()
if(APPLE)
    # Development.Embed is for TinyDVD.app's launcher, which runs Python in
    # its own process. Homebrew's Python includes it; some Pythons (pyenv
    # without --enable-shared, some conda setups) don't, and then only the
    # app is skipped, not the player
    find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter OPTIONAL_COMPONENTS Development.Embed)
else()
    find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
endif()
find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)

pkg_check_modules(MPV REQUIRED mpv)
pkg_check_modules(SDL2 REQUIRED sdl2)
pkg_check_modules(CAIRO REQUIRED cairo)
pkg_check_modules(PANGOCAIRO REQUIRED pangocairo)

add_definitions(-DGL_SILENCE_DEPRECATION)

include_directories(
    src
    ${MPV_INCLUDE_DIRS}
    ${SDL2_INCLUDE_DIRS}
    ${OPENGL_INCLUDE_DIRS}
    ${CAIRO_INCLUDE_DIRS}
    ${PANGOCAIRO_INCLUDE_DIRS}
)

set(TINYDVD_CORE_SOURCES
    src/core_bridge.c
    src/player_adapter.c
    src/mvob_parser.c
    src/tdvd_archive.c
    src/dvd_ifo.c
    src/dvd_vm_bridge.c
    src/mpv_controller.c
    src/spu_compositor.c
    src/spu_png.c
)
if(APPLE)
    list(APPEND TINYDVD_CORE_SOURCES src/ui/native_dialog_mac.m)
endif()

add_library(tinydvd_core SHARED ${TINYDVD_CORE_SOURCES})

set(TINYDVD_CORE_LINK_LIBS
    ${MPV_LIBRARIES}
    ${SDL2_LIBRARIES}
    ${OPENGL_LIBRARIES}
    ${CAIRO_LIBRARIES}
    ${PANGOCAIRO_LIBRARIES}
    Threads::Threads
    ZLIB::ZLIB
    m
)
if(APPLE)
    list(APPEND TINYDVD_CORE_LINK_LIBS "-framework Cocoa")
endif()
list(REMOVE_DUPLICATES TINYDVD_CORE_LINK_LIBS)

set(TINYDVD_CORE_LINK_DIRS
    ${MPV_LIBRARY_DIRS}
    ${SDL2_LIBRARY_DIRS}
    ${CAIRO_LIBRARY_DIRS}
    ${PANGOCAIRO_LIBRARY_DIRS}
)
list(REMOVE_DUPLICATES TINYDVD_CORE_LINK_DIRS)

target_link_libraries(tinydvd_core PRIVATE ${TINYDVD_CORE_LINK_LIBS})
target_link_directories(tinydvd_core PRIVATE ${TINYDVD_CORE_LINK_DIRS})
target_compile_options(tinydvd_core PRIVATE -Wall -Wextra)

set(TINYDVD_INSTALL_LIBDIR "/usr/local/lib/tinydvd" CACHE PATH "Installed TinyDVD runtime directory")

add_executable(tinydvd src/main.c)
add_dependencies(tinydvd tinydvd_core)
target_compile_definitions(tinydvd PRIVATE
    PYTHON3_EXECUTABLE="${Python3_EXECUTABLE}"
    TINYDVD_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    TINYDVD_BINARY_DIR="${CMAKE_BINARY_DIR}"
    TINYDVD_CORE_LIB="$<TARGET_FILE:tinydvd_core>"
    TINYDVD_INSTALL_LIBDIR="${TINYDVD_INSTALL_LIBDIR}"
)
target_compile_options(tinydvd PRIVATE -Wall -Wextra)

# macOS: the executable inside TinyDVD.app (./install builds the bundle)
if(APPLE AND Python3_Development.Embed_FOUND)
    set(TINYDVD_MACOS_APP ON CACHE INTERNAL "Building TinyDVD.app's launcher")
else()
    set(TINYDVD_MACOS_APP OFF CACHE INTERNAL "Building TinyDVD.app's launcher")
    if(APPLE)
        message(STATUS "TinyDVD.app skipped: ${Python3_EXECUTABLE} can't be embedded (no Development.Embed)")
    endif()
endif()
if(TINYDVD_MACOS_APP)
    add_executable(tinydvd_macos_app src/macos_app_main.c)
    set_target_properties(tinydvd_macos_app PROPERTIES OUTPUT_NAME tinydvd-macos-app)
    target_link_libraries(tinydvd_macos_app PRIVATE Python3::Python)
    target_compile_definitions(tinydvd_macos_app PRIVATE
        TINYDVD_INSTALL_LIBDIR="${TINYDVD_INSTALL_LIBDIR}"
    )
    target_compile_options(tinydvd_macos_app PRIVATE -Wall -Wextra)
endif()

# Convenience target / symlinks for tinydvd-player and convert-to-tinydvd
add_custom_command(TARGET tinydvd POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E create_symlink
    $<TARGET_FILE_NAME:tinydvd>
    ${CMAKE_BINARY_DIR}/tinydvd-player
    COMMAND ${CMAKE_COMMAND} -E create_symlink
    ${CMAKE_SOURCE_DIR}/scripts/convert-to-tinydvd.py
    ${CMAKE_BINARY_DIR}/convert-to-tinydvd
)

# Test executables
add_executable(test_mvob tests/test_mvob_parser.c src/mvob_parser.c)
target_include_directories(test_mvob PRIVATE src)

add_executable(test_spu tests/test_spu_rle.c src/spu_compositor.c)
target_include_directories(test_spu PRIVATE src ${OPENGL_INCLUDE_DIRS})
target_link_libraries(test_spu PRIVATE ${OPENGL_LIBRARIES})

add_executable(test_dvd_vm tests/test_dvd_vm.c src/player_adapter.c src/dvd_ifo.c src/dvd_vm_bridge.c src/tdvd_archive.c src/mvob_parser.c src/spu_compositor.c src/spu_png.c src/mpv_controller.c)
target_include_directories(test_dvd_vm PRIVATE src ${MPV_INCLUDE_DIRS} ${OPENGL_INCLUDE_DIRS})
target_link_libraries(test_dvd_vm PRIVATE ${MPV_LIBRARIES} ${OPENGL_LIBRARIES} Threads::Threads ZLIB::ZLIB)
target_link_directories(test_dvd_vm PRIVATE ${MPV_LIBRARY_DIRS})

add_executable(test_mpv tests/test_mpv_playback.c src/player_adapter.c src/dvd_ifo.c src/dvd_vm_bridge.c src/tdvd_archive.c src/mvob_parser.c src/spu_compositor.c src/spu_png.c src/mpv_controller.c)
target_include_directories(test_mpv PRIVATE src ${MPV_INCLUDE_DIRS} ${OPENGL_INCLUDE_DIRS})
target_link_libraries(test_mpv PRIVATE ${MPV_LIBRARIES} ${OPENGL_LIBRARIES} Threads::Threads ZLIB::ZLIB)
target_link_directories(test_mpv PRIVATE ${MPV_LIBRARY_DIRS})

add_executable(test_web tests/test_web_server.c)
add_dependencies(test_web tinydvd_core)
target_compile_definitions(test_web PRIVATE
    PYTHON3_EXECUTABLE="${Python3_EXECUTABLE}"
    TINYDVD_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    TINYDVD_BINARY_DIR="${CMAKE_BINARY_DIR}"
    TINYDVD_CORE_LIB="$<TARGET_FILE:tinydvd_core>"
)
target_compile_options(test_web PRIVATE -Wall -Wextra)

# The C tests check with assert(), which a Release build would compile out
foreach(_test test_mvob test_spu test_dvd_vm test_mpv test_web)
    target_compile_options(${_test} PRIVATE -UNDEBUG)
endforeach()

# Tests: run them all with `ctest --test-dir build --output-on-failure`.
# The DVD VM and web tests play tests/fixtures/TestDisc.tdvd, a small
# synthetic disc built by tests/fixtures/make-test-disc.sh.
enable_testing()
add_test(NAME mvob_parser COMMAND test_mvob)
add_test(NAME spu_compositor COMMAND test_spu)
add_test(NAME dvd_vm COMMAND test_dvd_vm WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_test(NAME mpv_playback COMMAND test_mpv WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_test(NAME web_server COMMAND test_web WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_test(NAME archive_helpers COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_archive.py)
add_test(NAME metadata_lookup COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_metadata.py)
add_test(NAME transcoder COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_transcode.py)
add_test(NAME av_sync COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_av_sync.py)
set_tests_properties(mvob_parser spu_compositor dvd_vm mpv_playback web_server archive_helpers transcoder av_sync
                     PROPERTIES TIMEOUT 300)

# Installing is done by ./install, which also sets up the Python runtime and
# desktop integration. There are deliberately no install() rules here, so
# there is only one definition of the installed layout.
