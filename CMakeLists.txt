cmake_minimum_required(VERSION 3.20)
project(tinydvd C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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
find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)

pkg_check_modules(DVDREAD REQUIRED dvdread)
pkg_check_modules(MPV REQUIRED mpv)
pkg_check_modules(SDL2 REQUIRED sdl2)

add_definitions(-DGL_SILENCE_DEPRECATION)

include_directories(
    src
    ${DVDREAD_INCLUDE_DIRS}
    ${MPV_INCLUDE_DIRS}
    ${SDL2_INCLUDE_DIRS}
    ${OPENGL_INCLUDE_DIRS}
)

set(TINYDVD_SOURCES
    src/main.c
    src/player_adapter.c
    src/mvob_parser.c
    src/tdvd_archive.c
    src/dvd_vm_bridge.c
    src/mpv_controller.c
    src/spu_compositor.c
    src/spu_png.c
    src/sha1.c
    src/web_server.c
)

add_executable(tinydvd ${TINYDVD_SOURCES})

target_link_libraries(tinydvd
    PRIVATE
    ${DVDREAD_LIBRARIES}
    ${MPV_LIBRARIES}
    ${SDL2_LIBRARIES}
    ${OPENGL_LIBRARIES}
    Threads::Threads
    ZLIB::ZLIB
)

target_link_directories(tinydvd
    PRIVATE
    ${DVDREAD_LIBRARY_DIRS}
    ${MPV_LIBRARY_DIRS}
    ${SDL2_LIBRARY_DIRS}
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

add_executable(test_web tests/test_web_server.c src/player_adapter.c src/web_server.c src/dvd_vm_bridge.c src/tdvd_archive.c src/mvob_parser.c src/spu_compositor.c src/spu_png.c src/sha1.c src/mpv_controller.c)
target_include_directories(test_web PRIVATE src ${DVDREAD_INCLUDE_DIRS} ${MPV_INCLUDE_DIRS} ${OPENGL_INCLUDE_DIRS})
target_link_libraries(test_web PRIVATE ${DVDREAD_LIBRARIES} ${MPV_LIBRARIES} ${OPENGL_LIBRARIES} Threads::Threads ZLIB::ZLIB)
target_link_directories(test_web PRIVATE ${DVDREAD_LIBRARY_DIRS} ${MPV_LIBRARY_DIRS})

# Installation rules for Linux / GNOME desktop integration
include(GNUInstallDirs)

install(TARGETS tinydvd
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
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
