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
    if(DEFINED ENV{PKG_CONFIG_PATH})
        set(ENV{PKG_CONFIG_PATH} "/opt/homebrew/lib/pkgconfig:/usr/local/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
    else()
        set(ENV{PKG_CONFIG_PATH} "/opt/homebrew/lib/pkgconfig:/usr/local/lib/pkgconfig")
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
find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)

pkg_check_modules(DVDREAD REQUIRED dvdread)
pkg_check_modules(MPV REQUIRED mpv)
pkg_check_modules(SDL2 REQUIRED sdl2)
pkg_check_modules(CAIRO REQUIRED cairo)
pkg_check_modules(PANGOCAIRO REQUIRED pangocairo)

add_definitions(-DGL_SILENCE_DEPRECATION)

include_directories(
    src
    ${DVDREAD_INCLUDE_DIRS}
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
    ${DVDREAD_LIBRARIES}
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
    ${DVDREAD_LIBRARY_DIRS}
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

add_executable(test_dvd_vm tests/test_dvd_vm.c src/player_adapter.c src/dvd_vm_bridge.c src/tdvd_archive.c src/mvob_parser.c src/spu_compositor.c src/spu_png.c src/mpv_controller.c)
target_include_directories(test_dvd_vm PRIVATE src ${DVDREAD_INCLUDE_DIRS} ${MPV_INCLUDE_DIRS} ${OPENGL_INCLUDE_DIRS})
target_link_libraries(test_dvd_vm PRIVATE ${DVDREAD_LIBRARIES} ${MPV_LIBRARIES} ${OPENGL_LIBRARIES} Threads::Threads ZLIB::ZLIB)
target_link_directories(test_dvd_vm PRIVATE ${DVDREAD_LIBRARY_DIRS} ${MPV_LIBRARY_DIRS})

add_executable(test_web tests/test_web_server.c)
add_dependencies(test_web tinydvd_core)
target_compile_definitions(test_web PRIVATE
    PYTHON3_EXECUTABLE="${Python3_EXECUTABLE}"
    TINYDVD_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    TINYDVD_BINARY_DIR="${CMAKE_BINARY_DIR}"
    TINYDVD_CORE_LIB="$<TARGET_FILE:tinydvd_core>"
)
target_compile_options(test_web PRIVATE -Wall -Wextra)

# Installation rules for Linux / GNOME desktop integration
include(GNUInstallDirs)

install(TARGETS tinydvd
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(TARGETS tinydvd_core
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/tinydvd
    RUNTIME DESTINATION ${CMAKE_INSTALL_LIBDIR}/tinydvd
)

install(FILES
    scripts/tinydvd_app.py
    scripts/tinydvd_core.py
    scripts/tinydvd_desktop_ui.py
    scripts/tinydvd_library.py
    scripts/tinydvd_metadata.py
    scripts/tinydvd_web_server.py
    scripts/convert-to-tinydvd.py
    scripts/tinydvd-thumbnailer.py
    scripts/tinydvd-set-thumbnail.py
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/tinydvd/scripts
)

install(FILES src/web_assets.h
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/tinydvd
)

install(PROGRAMS scripts/convert-to-tinydvd.py
    DESTINATION ${CMAKE_INSTALL_BINDIR}
    RENAME convert-to-tinydvd
)

install(FILES data/tinydvd.desktop
    DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
)

install(FILES data/tinydvd.xml
    DESTINATION ${CMAKE_INSTALL_DATADIR}/mime/packages
)

install(FILES data/icons/tinydvd.svg
    DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps
)

install(FILES data/icons/tinydvd.svg
    DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/mimetypes
    RENAME video-x-tinydvd.svg
)

install(PROGRAMS scripts/tinydvd-thumbnailer.py
    DESTINATION ${CMAKE_INSTALL_BINDIR}
    RENAME tinydvd-thumbnailer
)

install(PROGRAMS scripts/tinydvd-set-thumbnail.py
    DESTINATION ${CMAKE_INSTALL_BINDIR}
    RENAME tinydvd-set-thumbnail
)

install(FILES data/tinydvd.thumbnailer
    DESTINATION ${CMAKE_INSTALL_DATADIR}/thumbnailers
)
