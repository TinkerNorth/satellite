cmake_minimum_required(VERSION 3.20)

# ── Single-source-of-truth version ───────────────────────────────────────────
# /VERSION is the authoritative version string. src/core/version.h mirrors
# the same value as preprocessor macros (so satellite.rc and C++ both see
# it without a generated header). The version-consistency CI gate fails
# if these drift.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" SATELLITE_VERSION LIMIT_COUNT 1)
string(STRIP "${SATELLITE_VERSION}" SATELLITE_VERSION)
if(NOT SATELLITE_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
    message(FATAL_ERROR "VERSION file content '${SATELLITE_VERSION}' is not in MAJOR.MINOR.PATCH form.")
endif()
set(SATELLITE_VERSION_MAJOR "${CMAKE_MATCH_1}")
set(SATELLITE_VERSION_MINOR "${CMAKE_MATCH_2}")
set(SATELLITE_VERSION_PATCH "${CMAKE_MATCH_3}")
message(STATUS "satellite: building version ${SATELLITE_VERSION}")

project(satellite VERSION ${SATELLITE_VERSION} LANGUAGES CXX)

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
    src/core/update_service.cpp
    src/core/github_release.cpp
)

# Currently-shared infrastructure (will be ported to POSIX in Stage 3).
set(SATELLITE_NET_SOURCES
    src/net/receiver.cpp
    src/net/inner_dispatch.cpp
    src/net/webserver.cpp
    src/net/tls.cpp
    src/net/machine_id.cpp
    src/net/pairing.cpp
    src/net/pairing_service.cpp
    src/net/discovery.cpp
    src/net/mdns_protocol.cpp
    src/net/mdns_responder.cpp
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
    src/platform/windows/toast.cpp
    src/platform/windows/vigem.cpp
    src/platform/windows/vigem_adapter.cpp
    src/platform/windows/gamepad_backend.cpp
    src/platform/windows/updater_adapter.cpp
    src/platform/windows/app_lifecycle.cpp
    src/platform/windows/shell_integration.cpp
)

# macOS-only infrastructure.
set(SATELLITE_MAC_SOURCES
    src/platform/macos/main.mm
    src/platform/macos/globals.cpp
    src/platform/macos/config.cpp
    src/platform/macos/crypto.cpp
    src/platform/macos/tray.mm
    src/platform/macos/gamepad_backend.cpp
    src/platform/macos/updater_adapter.mm
)

# Linux-only infrastructure.
set(SATELLITE_LINUX_SOURCES
    src/platform/linux/main.cpp
    src/platform/linux/globals.cpp
    src/platform/linux/config.cpp
    src/platform/linux/crypto.cpp
    src/platform/linux/gamepad_backend.cpp
    src/platform/linux/gamepad_adapter.cpp
    src/platform/linux/tray.cpp
    src/platform/linux/updater_adapter.cpp
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
    )
    # Vendored third-party headers go in as SYSTEM so the compiler suppresses
    # warnings emitted from inside them — cpp-httplib uses its own deprecated
    # APIs internally, and we don't control that upstream code.
    target_include_directories(satellite SYSTEM PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/lib
        ${CMAKE_CURRENT_SOURCE_DIR}/vigem/include
    )

    target_compile_definitions(satellite PRIVATE
        _WIN32_WINNT=0x0A00
        CPPHTTPLIB_NO_EXCEPTIONS
        CPPHTTPLIB_OPENSSL_SUPPORT
    )

    # libsodium: pre-built static lib vendored under lib/libsodium/libsodium-win64/
    target_include_directories(satellite SYSTEM PRIVATE
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
        # WinRT: RoActivateInstance / RoGetActivationFactory + HSTRING for the
        # actionable pairing toast (src/platform/windows/toast.cpp).
        runtimeobject
        advapi32
        bcrypt
        crypt32
        winmm
        # OTA updater: WinHTTP (HTTPS to api.github.com).
        winhttp
        # Authenticode signature check on OTA-downloaded installer.
        wintrust
        # IPropertyStore + PKEY_AppUserModel_ID for jump-list user tasks.
        propsys
        # TaskDialogIndirect / modern common controls.
        comctl32
        comdlg32
        # MiniDumpWriteDump for the crash filter.
        dbghelp
        # In-library GUID + PROPERTYKEY definitions (FOLDERID_*,
        # CLSID_ShellLink, PKEY_Title, etc.). Without -luuid the
        # KnownFolders/ShObjIdl headers' extern consts are unresolved.
        uuid
        # InitPropVariantFromString is an inline wrapper around SHStrDupW
        # (shlwapi). Missing this lib causes ld to fail silently.
        shlwapi
    )

    # OpenSSL — the client API server speaks HTTPS (cpp-httplib SSLServer).
    # Provide via vcpkg, or set -DOPENSSL_ROOT_DIR=<prefix>.
    find_package(OpenSSL REQUIRED)
    target_link_libraries(satellite PRIVATE OpenSSL::SSL OpenSSL::Crypto)

    # Exploit-mitigation flags. Kept in sync with build-satellite.bat --
    # the two paths share a single binary outcome, so a flag landing in
    # only one place creates a "works in CI, breaks in dev" gap.
    if(MINGW)
        target_compile_options(satellite PRIVATE
            -O2 -Wall -Wextra
            -fstack-protector-strong
            -D_FORTIFY_SOURCE=2
            -Wformat -Wformat-security
        )
        target_link_options(satellite PRIVATE
            -static -mwindows
            -Wl,--dynamicbase
            -Wl,--high-entropy-va
            -Wl,--nxcompat
            -Wl,--disable-auto-image-base
        )
    elseif(MSVC)
        target_compile_options(satellite PRIVATE
            /O2 /W4 /permissive-
            /GS                # stack-buffer security check
            /guard:cf          # Control Flow Guard
            /Qspectre          # Spectre v1 mitigations
            /sdl               # Security Development Lifecycle checks
        )
        target_link_options(satellite PRIVATE
            /DYNAMICBASE
            /HIGHENTROPYVA
            /NXCOMPAT
            /GUARD:CF
            /CETCOMPAT         # Win11 hardware shadow stack
        )
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
    )
    # Vendored third-party headers (cpp-httplib) — SYSTEM suppresses upstream
    # deprecation warnings we can't fix.
    target_include_directories(satellite SYSTEM PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/lib
    )

    target_compile_definitions(satellite PRIVATE
        CPPHTTPLIB_NO_EXCEPTIONS
        CPPHTTPLIB_OPENSSL_SUPPORT
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

    # OpenSSL — the client API server speaks HTTPS (cpp-httplib SSLServer).
    # Apple ships no OpenSSL development headers; use the Homebrew keg.
    if(NOT OPENSSL_ROOT_DIR)
        execute_process(COMMAND brew --prefix openssl@3
            OUTPUT_VARIABLE BREW_OPENSSL OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(BREW_OPENSSL)
            set(OPENSSL_ROOT_DIR "${BREW_OPENSSL}")
        endif()
    endif()
    find_package(OpenSSL REQUIRED)
    target_link_libraries(satellite PRIVATE OpenSSL::SSL OpenSSL::Crypto)

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
        MACOSX_BUNDLE_BUNDLE_VERSION "${SATELLITE_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${SATELLITE_VERSION}"
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

elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(satellite
        ${SATELLITE_CORE_SOURCES}
        ${SATELLITE_NET_SOURCES}
        ${SATELLITE_ADAPTER_SOURCES}
        ${SATELLITE_LINUX_SOURCES}
    )

    target_include_directories(satellite PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/linux
    )
    # Vendored third-party headers (cpp-httplib) — SYSTEM suppresses upstream
    # deprecation warnings we can't fix.
    target_include_directories(satellite SYSTEM PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/lib
    )

    target_compile_definitions(satellite PRIVATE
        CPPHTTPLIB_NO_EXCEPTIONS
        CPPHTTPLIB_OPENSSL_SUPPORT
    )

    # libsodium: resolved via pkg-config (every distro ships libsodium-dev /
    # libsodium-devel with a .pc file). Fall back to a find_library search if
    # pkg-config is absent (e.g. bespoke build with -DSODIUM_ROOT=<prefix>).
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
            PATHS /usr/local /usr
        )
        find_library(SODIUM_LIBRARY NAMES sodium
            HINTS ${SODIUM_ROOT} ENV SODIUM_ROOT
            PATH_SUFFIXES lib lib64
            PATHS /usr/local /usr
        )
        if(NOT SODIUM_INCLUDE_DIR OR NOT SODIUM_LIBRARY)
            message(FATAL_ERROR
                "libsodium not found. Install libsodium-dev (Debian/Ubuntu), "
                "libsodium-devel (Fedora/RHEL), libsodium (Arch), or build "
                "from source and set -DSODIUM_ROOT=<prefix>.")
        endif()
        target_include_directories(satellite PRIVATE ${SODIUM_INCLUDE_DIR})
        target_link_libraries(satellite PRIVATE ${SODIUM_LIBRARY})
    endif()

    # OpenSSL — the client API server speaks HTTPS (cpp-httplib SSLServer).
    find_package(OpenSSL REQUIRED)
    target_link_libraries(satellite PRIVATE OpenSSL::SSL OpenSSL::Crypto)

    find_package(Threads REQUIRED)
    target_link_libraries(satellite PRIVATE Threads::Threads)

    # libcurl for the OTA updater (HTTPS to api.github.com + asset CDN).
    # Universally present on desktop distros; we link it directly rather
    # than dlopen'ing because the updater is now a first-class feature.
    #   Debian/Ubuntu: libcurl4-openssl-dev
    #   Fedora/RHEL:   libcurl-devel
    #   Arch:          curl
    if(PkgConfig_FOUND)
        pkg_check_modules(CURL REQUIRED libcurl)
        target_include_directories(satellite PRIVATE ${CURL_INCLUDE_DIRS})
        target_link_directories(satellite PRIVATE ${CURL_LIBRARY_DIRS})
        target_link_libraries(satellite PRIVATE ${CURL_LIBRARIES})
    else()
        find_package(CURL REQUIRED)
        target_link_libraries(satellite PRIVATE CURL::libcurl)
    endif()

    # Status-tray icon via libayatana-appindicator (StatusNotifierItem over
    # D-Bus, plus GTK3 for the menu widgets). Optional: when the dev headers
    # are absent the binary is built headless and just runs a sigwait loop.
    # Debian/Ubuntu: libayatana-appindicator3-dev
    # Fedora/RHEL:   libayatana-appindicator-gtk3-devel
    # Arch:          libayatana-appindicator
    if(PkgConfig_FOUND)
        pkg_check_modules(APPINDICATOR QUIET ayatana-appindicator3-0.1)
        if(NOT APPINDICATOR_FOUND)
            # Fall back to the older Canonical name still shipped on some boxes.
            pkg_check_modules(APPINDICATOR QUIET appindicator3-0.1)
        endif()
        if(APPINDICATOR_FOUND)
            pkg_check_modules(GTK3 REQUIRED gtk+-3.0)
            target_compile_definitions(satellite PRIVATE SATELLITE_HAS_TRAY=1)
            target_include_directories(satellite PRIVATE
                ${APPINDICATOR_INCLUDE_DIRS} ${GTK3_INCLUDE_DIRS})
            target_link_directories(satellite PRIVATE
                ${APPINDICATOR_LIBRARY_DIRS} ${GTK3_LIBRARY_DIRS})
            target_link_libraries(satellite PRIVATE
                ${APPINDICATOR_LIBRARIES} ${GTK3_LIBRARIES})
            message(STATUS "satellite: tray icon enabled (libayatana-appindicator + GTK3).")
            # libnotify (optional) drives the native reverse-pairing prompt with
            # Accept/Reject actions. Absent → the prompt falls back to the web
            # dashboard. Debian/Ubuntu: libnotify-dev; Fedora: libnotify-devel;
            # Arch: libnotify.
            pkg_check_modules(LIBNOTIFY QUIET libnotify)
            if(LIBNOTIFY_FOUND)
                target_compile_definitions(satellite PRIVATE SATELLITE_HAS_LIBNOTIFY=1)
                target_include_directories(satellite PRIVATE ${LIBNOTIFY_INCLUDE_DIRS})
                target_link_directories(satellite PRIVATE ${LIBNOTIFY_LIBRARY_DIRS})
                target_link_libraries(satellite PRIVATE ${LIBNOTIFY_LIBRARIES})
                message(STATUS "satellite: native pairing notifications enabled (libnotify).")
            else()
                message(STATUS "satellite: libnotify not found; reverse-pairing prompts use "
                               "the web dashboard only.")
            endif()
        else()
            message(STATUS "satellite: libayatana-appindicator not found; "
                           "building headless (no tray icon).")
        endif()
    else()
        message(STATUS "satellite: pkg-config missing; building headless (no tray icon).")
    endif()

    target_compile_options(satellite PRIVATE -O2 -Wall -Wextra)

    set_target_properties(satellite PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )

    # ── Install rules (drives `make install` and CPack DEB staging) ─────────
    include(GNUInstallDirs)
    set(SATELLITE_PKG_DIR  "${CMAKE_CURRENT_SOURCE_DIR}/packaging/debian")

    install(TARGETS satellite RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(DIRECTORY web/  DESTINATION ${CMAKE_INSTALL_DATADIR}/satellite/web)
    install(FILES   ${SATELLITE_PKG_DIR}/satellite.desktop
            DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES   web/icon.png
            DESTINATION ${CMAKE_INSTALL_DATADIR}/pixmaps RENAME satellite.png)
    install(FILES   ${SATELLITE_PKG_DIR}/70-satellite-uinput.rules
            DESTINATION lib/udev/rules.d)
    install(FILES   ${SATELLITE_PKG_DIR}/satellite-modules.conf
            DESTINATION lib/modules-load.d RENAME satellite.conf)
    install(FILES   ${SATELLITE_PKG_DIR}/copyright
            DESTINATION ${CMAKE_INSTALL_DOCDIR})

    # ── CPack: build .deb (DEB generator) and/or .rpm (RPM generator).
    # On a Debian/Ubuntu host: `cpack -G DEB` (needs dpkg-dev for shlibdeps).
    # On any Linux host:       `cpack -G RPM` (needs rpmbuild). Ubuntu's
    #                          rpm package ships rpmbuild too, so the release
    #                          workflow builds both from one job.
    set(CPACK_GENERATOR "DEB;RPM")
    set(SATELLITE_RPM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/packaging/rpm")
    set(CPACK_PACKAGE_NAME                  "satellite")
    set(CPACK_PACKAGE_VERSION               "${SATELLITE_VERSION}")
    set(CPACK_PACKAGE_VENDOR                "TinkerNorth")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY   "Low-latency Xbox controller forwarding over the network")
    set(CPACK_PACKAGE_HOMEPAGE_URL          "https://github.com/TinkerNorth/satellite")
    set(CPACK_PACKAGE_CONTACT               "TinkerNorth <https://github.com/TinkerNorth/satellite>")
    set(CPACK_RESOURCE_FILE_LICENSE         "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
    set(CPACK_PACKAGE_DESCRIPTION
        "Captures physical Xbox controller input on one machine and injects\n it as a virtual Xbox 360 controller on another via /dev/uinput.\n Includes a system-tray icon and a web UI at http://localhost:9877\n for pairing, configuration, and status.")

    set(CPACK_DEBIAN_PACKAGE_MAINTAINER     "TinkerNorth <https://github.com/TinkerNorth/satellite>")
    set(CPACK_DEBIAN_PACKAGE_SECTION        "games")
    set(CPACK_DEBIAN_PACKAGE_PRIORITY       "optional")
    set(CPACK_DEBIAN_PACKAGE_HOMEPAGE       "${CPACK_PACKAGE_HOMEPAGE_URL}")
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS      ON)

    set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
        "${SATELLITE_PKG_DIR}/postinst;${SATELLITE_PKG_DIR}/postrm")

    # File-name in the form `satellite_1.0.0_amd64.deb`.
    set(CPACK_DEBIAN_FILE_NAME              DEB-DEFAULT)

    # ── RPM-specific settings ───────────────────────────────────────────────
    # CPack/RPM autoreq picks up shared-library dependencies from the binary
    # automatically, so we don't enumerate them. Sectioning mirrors Fedora's
    # standard "Games" group ordering. Architecture is auto-derived from
    # CPACK_RPM_PACKAGE_ARCHITECTURE / uname -m.
    set(CPACK_RPM_PACKAGE_NAME              "satellite")
    set(CPACK_RPM_PACKAGE_LICENSE           "LGPLv3+")
    set(CPACK_RPM_PACKAGE_GROUP             "Amusements/Games")
    set(CPACK_RPM_PACKAGE_URL               "${CPACK_PACKAGE_HOMEPAGE_URL}")
    set(CPACK_RPM_PACKAGE_VENDOR            "TinkerNorth")
    set(CPACK_RPM_PACKAGE_SUMMARY           "${CPACK_PACKAGE_DESCRIPTION_SUMMARY}")
    set(CPACK_RPM_PACKAGE_DESCRIPTION       "${CPACK_PACKAGE_DESCRIPTION}")
    set(CPACK_RPM_PACKAGE_AUTOREQ           ON)
    # Wire the post-install / post-uninstall scriptlets that match the
    # behavior of the Debian postinst / postrm (udev reload, modprobe
    # uinput, best-effort input-group membership).
    set(CPACK_RPM_POST_INSTALL_SCRIPT_FILE
        "${SATELLITE_RPM_DIR}/postinst.sh")
    set(CPACK_RPM_POST_UNINSTALL_SCRIPT_FILE
        "${SATELLITE_RPM_DIR}/postuninst.sh")
    # Files our package owns that overlap with system-managed dirs need to
    # be carved out, otherwise rpmbuild refuses to claim ownership.
    set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
        "/lib/udev"
        "/lib/udev/rules.d"
        "/lib/modules-load.d"
        "/usr/share/applications"
        "/usr/share/pixmaps"
        "/usr/share/satellite")
    # File-name in the form `satellite-1.0.0-1.x86_64.rpm`.
    set(CPACK_RPM_FILE_NAME                 RPM-DEFAULT)

    include(CPack)

else()
    message(STATUS "satellite: unsupported platform; configuring tests only.")
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

# ── Receiver inner-message dispatch tests (portable — no networking) ─────────
# Exercises net/inner_dispatch.cpp: the per-message length guards that reject
# truncated / oversized wire payloads before a decoder reads out of bounds.
# inner_dispatch.cpp is socket-free, so this builds cleanly on every platform.
add_executable(test_receiver
    tests/test_receiver.cpp
    src/net/inner_dispatch.cpp
    ${SATELLITE_CORE_SOURCES}
)
target_include_directories(test_receiver PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(WIN32 AND MINGW)
    target_compile_options(test_receiver PRIVATE -O2 -Wall -Wextra)
elseif(MSVC)
    target_compile_options(test_receiver PRIVATE /O2 /W4)
endif()
set_target_properties(test_receiver PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
add_test(NAME receiver COMMAND test_receiver)

# ── mDNS protocol tests (portable — no networking) ───────────────────────────
add_executable(test_mdns_protocol
    tests/test_mdns_protocol.cpp
    src/net/mdns_protocol.cpp
)
target_include_directories(test_mdns_protocol PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(WIN32 AND MINGW)
    target_compile_options(test_mdns_protocol PRIVATE -O2 -Wall -Wextra)
elseif(MSVC)
    target_compile_options(test_mdns_protocol PRIVATE /O2 /W4)
endif()
set_target_properties(test_mdns_protocol PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
add_test(NAME mdns_protocol COMMAND test_mdns_protocol)

# ── Pairing-request registry tests (portable — no networking, no sodium) ─────
# Exercises net/pairing.cpp: the Path-B (dish shows PIN, operator accepts)
# request lifecycle — submit, poll, PIN-matched accept, single-use key
# handback, deny, and refresh-in-place. Deliberately crypto-free, so it builds
# on every platform without libsodium.
add_executable(test_pairing
    tests/test_pairing.cpp
    src/net/pairing.cpp
)
target_include_directories(test_pairing PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(WIN32 AND MINGW)
    target_compile_options(test_pairing PRIVATE -O2 -Wall -Wextra)
elseif(MSVC)
    target_compile_options(test_pairing PRIVATE /O2 /W4)
endif()
set_target_properties(test_pairing PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
add_test(NAME pairing COMMAND test_pairing)

# ── Linux platform tests ────────────────────────────────────────────────────
# Exercises the Linux-only adapters that don't need root/uinput/D-Bus:
# JSON helpers, config persistence, XDG autostart .desktop file handling,
# and path utilities.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(test_linux_platform
        tests/test_linux_platform.cpp
        src/platform/linux/config.cpp
        src/platform/linux/globals.cpp
        src/platform/linux/gamepad_adapter.cpp
        ${SATELLITE_CORE_SOURCES}
    )
    target_include_directories(test_linux_platform PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/linux
    )
    target_include_directories(test_linux_platform SYSTEM PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/lib
    )
    # SATELLITE_BUILD_TESTS gates test-only seams (e.g. the synthetic
    # invokeLightbarForTest entrypoint) — the symbol is elided from
    # production builds, which don't define this macro.
    target_compile_definitions(test_linux_platform PRIVATE
        CPPHTTPLIB_NO_EXCEPTIONS
        SATELLITE_BUILD_TESTS
    )
    target_compile_options(test_linux_platform PRIVATE -O2 -Wall -Wextra)
    find_package(Threads REQUIRED)
    target_link_libraries(test_linux_platform PRIVATE Threads::Threads)
    set_target_properties(test_linux_platform PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    add_test(NAME linux_platform COMMAND test_linux_platform)
endif()

# ── Windows platform tests ──────────────────────────────────────────────────
# Exercises the Windows-only adapters that don't need a desktop session,
# ViGEm bus driver, or paired client: JSON helpers, config persistence,
# HKCU\...\Run autostart, and path utilities. Both APPDATA config and
# the Run-key value are snapshotted/restored by the test harness so the
# suite is safe to run on a developer machine as well as in CI.
if(WIN32)
    add_executable(test_windows_platform
        tests/test_windows_platform.cpp
        src/platform/windows/config.cpp
        src/platform/windows/globals.cpp
        ${SATELLITE_CORE_SOURCES}
    )
    target_include_directories(test_windows_platform PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/windows
    )
    target_include_directories(test_windows_platform SYSTEM PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/lib
        ${CMAKE_CURRENT_SOURCE_DIR}/vigem/include
    )
    target_compile_definitions(test_windows_platform PRIVATE
        _WIN32_WINNT=0x0A00
        CPPHTTPLIB_NO_EXCEPTIONS
    )
    target_link_libraries(test_windows_platform PRIVATE
        ws2_32
        shell32
        ole32
        advapi32
        bcrypt
        crypt32
        winmm
    )
    if(MINGW)
        target_compile_options(test_windows_platform PRIVATE -O2 -Wall -Wextra)
        target_link_options(test_windows_platform PRIVATE -static)
    elseif(MSVC)
        target_compile_options(test_windows_platform PRIVATE /O2 /W4)
    endif()
    set_target_properties(test_windows_platform PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    add_test(NAME windows_platform COMMAND test_windows_platform)
endif()

# ── ViGEm adapter submit-path tests ─────────────────────────────────────────
# Links the real vigem_adapter.cpp against an in-test fake ViGEmBus driver
# layer (the free functions from vigem.cpp) so the submit IO path can be
# exercised without the driver. Pins that submits use the SYNCHRONOUS helpers
# (never fire-and-forget) and that a DS4 EX rejection falls back to the basic
# DS4 report — the regression that silently killed PlayStation input.
if(WIN32)
    add_executable(test_vigem_adapter
        tests/test_vigem_adapter.cpp
        src/platform/windows/vigem_adapter.cpp
    )
    target_include_directories(test_vigem_adapter PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/windows
    )
    target_include_directories(test_vigem_adapter SYSTEM PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/lib
        ${CMAKE_CURRENT_SOURCE_DIR}/vigem/include
    )
    target_compile_definitions(test_vigem_adapter PRIVATE
        _WIN32_WINNT=0x0A00
    )
    target_link_libraries(test_vigem_adapter PRIVATE
        ws2_32
        user32
        setupapi
    )
    if(MINGW)
        target_compile_options(test_vigem_adapter PRIVATE -O2 -Wall -Wextra)
        target_link_options(test_vigem_adapter PRIVATE -static)
    elseif(MSVC)
        target_compile_options(test_vigem_adapter PRIVATE /O2 /W4)
    endif()
    set_target_properties(test_vigem_adapter PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    add_test(NAME vigem_adapter COMMAND test_vigem_adapter)
endif()
