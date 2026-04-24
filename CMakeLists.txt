cmake_minimum_required(VERSION 3.20)
project(satellite CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ── Source lists ─────────────────────────────────────────────────────────────
# Core domain (portable, no OS deps).
set(SATELLITE_CORE_SOURCES
    src/core/session_service.cpp
)

# Currently-shared infrastructure (will be ported to POSIX in Stage 3).
set(SATELLITE_NET_SOURCES
    src/net/receiver.cpp
    src/net/webserver.cpp
    src/net/pairing.cpp
    src/net/discovery.cpp
)

# Portable adapters (thin shims over platform-specific free functions).
set(SATELLITE_ADAPTER_SOURCES
    src/adapters/log_adapter.cpp
    src/adapters/client_adapter.cpp
)

# Windows-only infrastructure.
set(SATELLITE_WIN_SOURCES
    src/platform/windows/main.cpp
    src/platform/windows/globals.cpp
    src/platform/windows/config.cpp
    src/platform/windows/crypto.cpp
    src/platform/windows/tray.cpp
    src/platform/windows/vigem.cpp
    src/platform/windows/vigem_adapter.cpp
)

# macOS-only infrastructure.
set(SATELLITE_MAC_SOURCES
    src/platform/macos/main.mm
    src/platform/macos/globals.cpp
    src/platform/macos/config.cpp
    src/platform/macos/crypto.cpp
    src/platform/macos/tray.mm
    src/platform/macos/vigem_stub.cpp
)

# ── Platform-specific target ─────────────────────────────────────────────────
if(WIN32)
    # Resource file (icon + manifest + version info).
    set(SATELLITE_RC ${CMAKE_CURRENT_SOURCE_DIR}/satellite.rc)

    add_executable(satellite WIN32
        ${SATELLITE_CORE_SOURCES}
        ${SATELLITE_NET_SOURCES}
        ${SATELLITE_ADAPTER_SOURCES}
        ${SATELLITE_WIN_SOURCES}
        ${SATELLITE_RC}
    )

    # Platform-specific headers (crypto.h, config.h, vigem.h, tray.h, globals.h)
    # are resolved via bare includes — add the windows platform dir to the
    # search path so net/*.cpp and adapters/*.cpp stay OS-agnostic.
    target_include_directories(satellite PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/windows
        ${CMAKE_CURRENT_SOURCE_DIR}/lib
        ${CMAKE_CURRENT_SOURCE_DIR}/vigem/include
    )

    target_compile_definitions(satellite PRIVATE
        _WIN32_WINNT=0x0A00
        CPPHTTPLIB_NO_EXCEPTIONS
    )

    # libsodium: pre-built static lib vendored under lib/libsodium/libsodium-win64/
    target_include_directories(satellite PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/libsodium/libsodium-win64/include
    )
    target_link_directories(satellite PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/libsodium/libsodium-win64/lib
    )

    target_link_libraries(satellite PRIVATE
        sodium
        setupapi
        ws2_32
        shell32
        ole32
        advapi32
        bcrypt
        crypt32
        winmm
    )

    if(MINGW)
        target_compile_options(satellite PRIVATE -O2 -Wall -Wextra)
        target_link_options(satellite PRIVATE -static -mwindows)
    elseif(MSVC)
        target_compile_options(satellite PRIVATE /O2 /W4 /permissive-)
        set_target_properties(satellite PROPERTIES
            LINK_FLAGS "/SUBSYSTEM:WINDOWS"
            MSVC_RUNTIME_LIBRARY "MultiThreaded"
        )
    endif()

    set_target_properties(satellite PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )

elseif(APPLE)
    enable_language(OBJCXX)

    add_executable(satellite MACOSX_BUNDLE
        ${SATELLITE_CORE_SOURCES}
        ${SATELLITE_NET_SOURCES}
        ${SATELLITE_ADAPTER_SOURCES}
        ${SATELLITE_MAC_SOURCES}
    )

    target_include_directories(satellite PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/macos
        ${CMAKE_CURRENT_SOURCE_DIR}/lib
    )

    target_compile_definitions(satellite PRIVATE
        CPPHTTPLIB_NO_EXCEPTIONS
    )

    # libsodium: prefer pkg-config (Homebrew / MacPorts), fall back to a plain
    # find_path / find_library search so builds work without pkg-config as long
    # as sodium.h and libsodium are reachable (e.g. /usr/local from a manual
    # `make install`, or a SODIUM_ROOT override).
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(SODIUM QUIET libsodium)
    endif()
    if(SODIUM_FOUND)
        target_include_directories(satellite PRIVATE ${SODIUM_INCLUDE_DIRS})
        target_link_directories(satellite PRIVATE ${SODIUM_LIBRARY_DIRS})
        target_link_libraries(satellite PRIVATE ${SODIUM_LIBRARIES})
    else()
        find_path(SODIUM_INCLUDE_DIR sodium.h
            HINTS ${SODIUM_ROOT} ENV SODIUM_ROOT
            PATH_SUFFIXES include
            PATHS /usr/local /opt/homebrew /opt/local /usr
        )
        find_library(SODIUM_LIBRARY NAMES sodium
            HINTS ${SODIUM_ROOT} ENV SODIUM_ROOT
            PATH_SUFFIXES lib
            PATHS /usr/local /opt/homebrew /opt/local /usr
        )
        if(NOT SODIUM_INCLUDE_DIR OR NOT SODIUM_LIBRARY)
            message(FATAL_ERROR
                "libsodium not found. Install via `brew install libsodium`, "
                "`port install libsodium`, or build from source and set "
                "-DSODIUM_ROOT=<prefix>.")
        endif()
        target_include_directories(satellite PRIVATE ${SODIUM_INCLUDE_DIR})
        target_link_libraries(satellite PRIVATE ${SODIUM_LIBRARY})
    endif()

    # Cocoa frameworks for menu-bar tray + app lifecycle.
    target_link_libraries(satellite PRIVATE
        "-framework AppKit"
        "-framework Foundation"
    )

    target_compile_options(satellite PRIVATE -O2 -Wall -Wextra -Wno-deprecated-declarations)

    # Keep the MACOSX_BUNDLE wrapper but drop the .app binary next to the
    # source tree, matching the Windows build's output convention.
    set_target_properties(satellite PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        MACOSX_BUNDLE_BUNDLE_NAME "Satellite"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.tinkernorth.satellite"
        MACOSX_BUNDLE_BUNDLE_VERSION "1.0.0"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0.0"
    )

    # Copy the web UI into the .app bundle's Resources directory after build,
    # so the server can serve it from <bundle>/Contents/Resources/web.
    add_custom_command(TARGET satellite POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -rf
            "$<TARGET_BUNDLE_CONTENT_DIR:satellite>/Resources/web"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/web"
            "$<TARGET_BUNDLE_CONTENT_DIR:satellite>/Resources/web"
        COMMENT "Staging web/ into the .app bundle"
    )

else()
    message(STATUS "satellite: only Windows and macOS are supported; "
                   "configuring tests only on this platform.")
endif()

# ── Tests (portable — core-only) ─────────────────────────────────────────────
enable_testing()

add_executable(test_session_service
    tests/test_session_service.cpp
    ${SATELLITE_CORE_SOURCES}
)
target_include_directories(test_session_service PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(WIN32 AND MINGW)
    target_compile_options(test_session_service PRIVATE -O2 -Wall -Wextra)
elseif(MSVC)
    target_compile_options(test_session_service PRIVATE /O2 /W4)
endif()

set_target_properties(test_session_service PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)

add_test(NAME session_service COMMAND test_session_service)
