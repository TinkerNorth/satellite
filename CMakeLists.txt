cmake_minimum_required(VERSION 3.20)

# /VERSION is the authoritative version string. src/core/version.h mirrors
# the same value as preprocessor macros so satellite.rc and C++ both see it
# without a generated header. The version-consistency CI gate fails if these
# drift.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" SATELLITE_VERSION_FILE LIMIT_COUNT 1)
string(STRIP "${SATELLITE_VERSION_FILE}" SATELLITE_VERSION_FILE)

if(DEFINED SATELLITE_RELEASE_VERSION AND NOT "${SATELLITE_RELEASE_VERSION}" STREQUAL "")
    set(SATELLITE_VERSION_DISPLAY "${SATELLITE_RELEASE_VERSION}")
    string(REGEX REPLACE "^[vV]" "" SATELLITE_VERSION "${SATELLITE_RELEASE_VERSION}")
    string(REGEX REPLACE "[-+].*$" "" SATELLITE_VERSION "${SATELLITE_VERSION}")
else()
    set(SATELLITE_VERSION "${SATELLITE_VERSION_FILE}")
    set(SATELLITE_VERSION_DISPLAY "${SATELLITE_VERSION_FILE}-dev")
endif()

if(NOT SATELLITE_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
    message(FATAL_ERROR "Resolved version '${SATELLITE_VERSION}' is not in MAJOR.MINOR.PATCH form.")
endif()
set(SATELLITE_VERSION_MAJOR "${CMAKE_MATCH_1}")
set(SATELLITE_VERSION_MINOR "${CMAKE_MATCH_2}")
set(SATELLITE_VERSION_PATCH "${CMAKE_MATCH_3}")
message(STATUS "satellite: building ${SATELLITE_VERSION} (display: ${SATELLITE_VERSION_DISPLAY})")

project(satellite VERSION ${SATELLITE_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(WIN32)
    add_compile_definitions(NOMINMAX)
endif()

# Vendored single-header libraries (nlohmann/json, cpp-httplib) under lib/.
# SYSTEM keeps their internal warnings out of -Wall/-Wextra//W4, and every
# target (including pure-core tests) can include them.
include_directories(SYSTEM ${CMAKE_CURRENT_SOURCE_DIR}/lib)

# Core domain (portable, no OS deps).
set(SATELLITE_CORE_SOURCES
    src/core/session_service.cpp
    src/core/catalog.cpp
    src/core/update_service.cpp
    src/core/github_release.cpp
    src/core/network_info.cpp
)

# Currently-shared infrastructure (to be ported to POSIX in Stage 3).
set(SATELLITE_NET_SOURCES
    src/net/receiver.cpp
    src/net/inner_dispatch.cpp
    src/net/webserver.cpp
    src/net/routes_common.cpp
    src/net/routes_admin.cpp
    src/net/routes_client.cpp
    src/net/tls.cpp
    src/net/machine_id.cpp
    src/net/pairing.cpp
    src/net/pairing_keys.cpp
    src/net/pairing_service.cpp
    src/net/session_crypto.cpp
    src/net/pin_rotation.cpp
    src/net/discovery.cpp
    src/net/mdns_protocol.cpp
    src/net/mdns_responder.cpp
    src/net/local_iface.cpp
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
    src/core/config_io.cpp
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
    src/platform/posix/config_posix.cpp
    src/core/config_io.cpp
    src/platform/posix/crypto_posix.cpp
    src/platform/macos/tray.mm
    src/platform/macos/gamepad_backend.cpp
    src/platform/macos/mac_hid_gamepad_adapter.cpp
    src/platform/macos/updater_adapter.mm
)

# Linux-only infrastructure.
set(SATELLITE_LINUX_SOURCES
    src/platform/linux/main.cpp
    src/platform/linux/globals.cpp
    src/platform/linux/config.cpp
    src/platform/posix/config_posix.cpp
    src/core/config_io.cpp
    src/platform/posix/crypto_posix.cpp
    src/platform/linux/gamepad_backend.cpp
    src/platform/linux/gamepad_adapter.cpp
    src/platform/linux/tray.cpp
    src/platform/linux/updater_adapter.cpp
)

if(WIN32)
    set(SATELLITE_RC ${CMAKE_CURRENT_SOURCE_DIR}/satellite.rc)

    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/version_rc.h.in
        ${CMAKE_CURRENT_BINARY_DIR}/generated/version_rc.h
        @ONLY)

    add_executable(satellite WIN32
        ${SATELLITE_CORE_SOURCES}
        ${SATELLITE_NET_SOURCES}
        ${SATELLITE_ADAPTER_SOURCES}
        ${SATELLITE_WIN_SOURCES}
        ${SATELLITE_RC}
    )

    # Platform headers (crypto.h, config.h, vigem.h, tray.h, globals.h) are
    # resolved via bare includes; add the windows platform dir to the search
    # path so net/*.cpp and adapters/*.cpp stay OS-agnostic.
    target_include_directories(satellite PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/windows
        ${CMAKE_CURRENT_BINARY_DIR}/generated
    )
    # SYSTEM suppresses warnings from inside vendored headers (cpp-httplib uses
    # its own deprecated APIs, which we don't control).
    target_include_directories(satellite SYSTEM PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/lib
        ${CMAKE_CURRENT_SOURCE_DIR}/vigem/include
    )

    target_compile_definitions(satellite PRIVATE
        _WIN32_WINNT=0x0A00
        CPPHTTPLIB_NO_EXCEPTIONS
        CPPHTTPLIB_OPENSSL_SUPPORT
    )

    # libsodium is resolved per-toolchain after the system-library list below:
    # the vendored MinGW archive for GCC, or vcpkg for MSVC (see the if(MINGW)
    # block further down).

    target_link_libraries(satellite PRIVATE
        setupapi
        ws2_32
        iphlpapi
        shell32
        ole32
        oleaut32
        # WinRT: RoActivateInstance / RoGetActivationFactory + HSTRING for the
        # actionable pairing toast (src/platform/windows/toast.cpp).
        runtimeobject
        advapi32
        bcrypt
        crypt32
        winmm
        # MMCSS (AvSetMmThreadCharacteristics) for the low-latency receiver thread.
        avrt
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

    # OpenSSL: the client API server speaks HTTPS (cpp-httplib SSLServer).
    # Provide via vcpkg, or set -DOPENSSL_ROOT_DIR=<prefix>.
    find_package(OpenSSL REQUIRED)
    target_link_libraries(satellite PRIVATE OpenSSL::SSL OpenSSL::Crypto)

    # libsodium: MinGW links the vendored prebuilt static archive; MSVC pulls an
    # ABI-matching static build from vcpkg (x64-windows-static → /MT, matching
    # MSVC_RUNTIME_LIBRARY below). The vcpkg path is what makes the hardened MSVC
    # build (CFG/CET) achievable without re-vendoring libsodium for MSVC by hand.
    if(MINGW)
        target_include_directories(satellite SYSTEM PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/lib/libsodium/libsodium-win64/include)
        target_link_directories(satellite PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/lib/libsodium/libsodium-win64/lib)
        target_link_libraries(satellite PRIVATE sodium)
    else()
        find_package(unofficial-sodium CONFIG REQUIRED)
        target_link_libraries(satellite PRIVATE unofficial-sodium::sodium)
    endif()

    # Exploit-mitigation flags. CMake is the single source of truth for the
    # build (build-satellite.bat just drives CMake), so these apply to every
    # Windows build path.
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
            # satellite.rc already embeds satellite.manifest as RT_MANIFEST id 1
            # (windres consumes the same source). MSVC's linker embeds its own
            # manifest by default, which collides (CVT1100 "duplicate resource").
            # Suppress the linker copy so the .rc stays the single manifest source.
            /MANIFEST:NO
        )
        set_target_properties(satellite PROPERTIES
            LINK_FLAGS "/SUBSYSTEM:WINDOWS"
            MSVC_RUNTIME_LIBRARY "MultiThreaded"
        )
    endif()

    # Drop satellite.exe at the source root for BOTH single-config (MinGW
    # Makefiles / Ninja) and multi-config (Visual Studio) generators. The
    # per-config properties stop VS from nesting the binary under Release/, so
    # installer.iss and release.yml find it at the same path under either
    # toolchain.
    set_target_properties(satellite PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}
        RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_CURRENT_SOURCE_DIR}
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
        ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/posix
    )
    # SYSTEM suppresses upstream deprecation warnings from cpp-httplib.
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

    # OpenSSL: the client API server speaks HTTPS (cpp-httplib SSLServer).
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

    target_link_libraries(satellite PRIVATE
        "-framework AppKit"
        "-framework Foundation"
        # Virtual-DS4 backend: IOHIDUserDevice + CF property dictionaries.
        "-framework IOKit"
        "-framework CoreFoundation"
    )

    target_compile_options(satellite PRIVATE -O2 -Wall -Wextra -Wno-deprecated-declarations)

    # Keep the MACOSX_BUNDLE wrapper but drop the .app next to the source tree,
    # matching the Windows build's output convention.
    set_target_properties(satellite PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        MACOSX_BUNDLE_BUNDLE_NAME "Satellite"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.tinkernorth.satellite"
        MACOSX_BUNDLE_BUNDLE_VERSION "${SATELLITE_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${SATELLITE_VERSION}"
    )

    # Copy the web UI into the .app so the server can serve it from
    # <bundle>/Contents/Resources/web.
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
        ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/posix
    )
    # SYSTEM suppresses upstream deprecation warnings from cpp-httplib.
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

    # OpenSSL: the client API server speaks HTTPS (cpp-httplib SSLServer).
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

    # Install rules (drives `make install` and CPack DEB staging).
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

    # CPack: build .deb (DEB generator) and/or .rpm (RPM generator).
    # Debian/Ubuntu host: `cpack -G DEB` (needs dpkg-dev for shlibdeps).
    # Any Linux host: `cpack -G RPM` (needs rpmbuild). Ubuntu's rpm package
    # ships rpmbuild too, so the release workflow builds both from one job.
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

    set(CPACK_DEBIAN_FILE_NAME              DEB-DEFAULT)

    # CPack/RPM autoreq picks up shared-library dependencies from the binary, so
    # we don't enumerate them. Group mirrors Fedora's "Games" ordering.
    # Architecture is auto-derived from CPACK_RPM_PACKAGE_ARCHITECTURE / uname -m.
    set(CPACK_RPM_PACKAGE_NAME              "satellite")
    set(CPACK_RPM_PACKAGE_LICENSE           "LGPLv3+")
    set(CPACK_RPM_PACKAGE_GROUP             "Amusements/Games")
    set(CPACK_RPM_PACKAGE_URL               "${CPACK_PACKAGE_HOMEPAGE_URL}")
    set(CPACK_RPM_PACKAGE_VENDOR            "TinkerNorth")
    set(CPACK_RPM_PACKAGE_SUMMARY           "${CPACK_PACKAGE_DESCRIPTION_SUMMARY}")
    set(CPACK_RPM_PACKAGE_DESCRIPTION       "${CPACK_PACKAGE_DESCRIPTION}")
    set(CPACK_RPM_PACKAGE_AUTOREQ           ON)
    # Scriptlets match the Debian postinst / postrm (udev reload, modprobe
    # uinput, best-effort input-group membership).
    set(CPACK_RPM_POST_INSTALL_SCRIPT_FILE
        "${SATELLITE_RPM_DIR}/postinst.sh")
    set(CPACK_RPM_POST_UNINSTALL_SCRIPT_FILE
        "${SATELLITE_RPM_DIR}/postuninst.sh")
    # Carve out files that overlap system-managed dirs, otherwise rpmbuild
    # refuses to claim ownership.
    set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
        "/lib/udev"
        "/lib/udev/rules.d"
        "/lib/modules-load.d"
        "/usr/share/applications"
        "/usr/share/pixmaps"
        "/usr/share/satellite")
    set(CPACK_RPM_FILE_NAME                 RPM-DEFAULT)

    include(CPack)

else()
    message(STATUS "satellite: unsupported platform; configuring tests only.")
endif()

# Link-Time Optimization (IPO/LTO) for the shipped binary.
# Cross-TU inlining across the hot path (receiver → session_service →
# vigem_adapter → vigem). check_ipo_supported gates it so a toolchain that can't
# do LTO degrades gracefully instead of failing configure. Release-only: the
# test executables stay non-LTO for fast, debuggable builds. MSVC maps this to
# /GL + /LTCG (which also strengthens /guard:cf); GCC/Clang to -flto.
if(TARGET satellite)
    target_compile_definitions(satellite PRIVATE
        SATELLITE_VERSION_RESOLVED="${SATELLITE_VERSION_DISPLAY}")

    include(CheckIPOSupported)
    check_ipo_supported(RESULT SATELLITE_IPO_OK OUTPUT SATELLITE_IPO_MSG LANGUAGES CXX)
    if(SATELLITE_IPO_OK)
        set_target_properties(satellite PROPERTIES INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
        message(STATUS "satellite: LTO/IPO enabled for Release builds.")
    else()
        message(STATUS "satellite: LTO/IPO unavailable: ${SATELLITE_IPO_MSG}")
    endif()
endif()

# Tests (portable, core-only).
enable_testing()

add_executable(test_session_service
    tests/test_session_service.cpp
    ${SATELLITE_CORE_SOURCES}
)
target_include_directories(test_session_service PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
# Enables the backdateForTest seam for reap/grace coverage.
target_compile_definitions(test_session_service PRIVATE SATELLITE_BUILD_TESTS)

if(WIN32 AND MINGW)
    target_compile_options(test_session_service PRIVATE -O2 -Wall -Wextra)
elseif(MSVC)
    target_compile_options(test_session_service PRIVATE /O2 /W4)
endif()

set_target_properties(test_session_service PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)

add_test(NAME session_service COMMAND test_session_service)

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

# Exercises net/pairing.cpp: the Path-B (dish shows PIN, operator accepts)
# request lifecycle: submit, poll, PIN-matched accept, single-use key handback,
# deny, and refresh-in-place. Crypto-free, so it builds on every platform
# without libsodium.
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

# Each pairs a pure unit (codec / parser / predicate) with its own test binary,
# mirroring the "pure core, separate from I/O" split the app itself uses.
function(satellite_add_pure_test name)
    add_executable(${name} ${ARGN})
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    if(WIN32 AND MINGW)
        target_compile_options(${name} PRIVATE -O2 -Wall -Wextra)
        target_link_options(${name} PRIVATE -static)
    elseif(MSVC)
        target_compile_options(${name} PRIVATE /O2 /W4)
    endif()
    set_target_properties(${name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    add_test(NAME ${name} COMMAND ${name})
endfunction()

# Links libsodium into a test target, replicating whichever resolution the
# platform uses for the satellite binary: the vendored prebuilt archive on
# MinGW, vcpkg's config package on MSVC, pkg-config (with the find_path /
# find_library fallback) on Linux/macOS. The POSIX branches reuse the
# SODIUM_* variables the satellite block above already resolved.
function(satellite_link_sodium name)
    if(MINGW)
        target_include_directories(${name} SYSTEM PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/lib/libsodium/libsodium-win64/include)
        target_link_directories(${name} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/lib/libsodium/libsodium-win64/lib)
        target_link_libraries(${name} PRIVATE sodium)
    elseif(MSVC)
        find_package(unofficial-sodium CONFIG REQUIRED)
        target_link_libraries(${name} PRIVATE unofficial-sodium::sodium)
    elseif(SODIUM_FOUND)
        target_include_directories(${name} SYSTEM PRIVATE ${SODIUM_INCLUDE_DIRS})
        target_link_directories(${name} PRIVATE ${SODIUM_LIBRARY_DIRS})
        target_link_libraries(${name} PRIVATE ${SODIUM_LIBRARIES})
    elseif(SODIUM_INCLUDE_DIR AND SODIUM_LIBRARY)
        target_include_directories(${name} SYSTEM PRIVATE ${SODIUM_INCLUDE_DIR})
        target_link_libraries(${name} PRIVATE ${SODIUM_LIBRARY})
    else()
        message(FATAL_ERROR
            "libsodium not resolved for ${name}; install libsodium (dev headers) "
            "or set -DSODIUM_ROOT=<prefix>.")
    endif()
endfunction()

satellite_add_pure_test(test_ipv4_util tests/test_ipv4_util.cpp)
satellite_add_pure_test(test_semver tests/test_semver.cpp)
satellite_add_pure_test(test_mdns_rejoin tests/test_mdns_rejoin.cpp)
satellite_add_pure_test(test_firewall_status tests/test_firewall_status.cpp)
satellite_add_pure_test(test_codecs tests/test_codecs.cpp)
satellite_add_pure_test(test_machine_id tests/test_machine_id.cpp)
satellite_add_pure_test(test_discovery tests/test_discovery.cpp)
satellite_add_pure_test(test_github_release tests/test_github_release.cpp src/core/github_release.cpp)
satellite_add_pure_test(test_update_service tests/test_update_service.cpp src/core/update_service.cpp)
satellite_add_pure_test(test_json tests/test_json.cpp)
satellite_add_pure_test(test_config_json tests/test_config_json.cpp)
satellite_add_pure_test(test_network_info tests/test_network_info.cpp src/core/network_info.cpp)
satellite_add_pure_test(test_origin_guard tests/test_origin_guard.cpp)
satellite_add_pure_test(test_status_json tests/test_status_json.cpp)

# Portable session-crypto interop suite (net/session_crypto.cpp): the pinned
# RFC 5869 HKDF vector, the HMAC-proof vector, and the AEAD direction/counter/
# AAD bindings run on EVERY lane (Linux, MinGW, MSVC, macOS), so a cross-end
# crypto drift fails all CIs, not just Windows. Gated on the three platforms
# that have a libsodium recipe; an unsupported host still configures the
# sodium-free tests above.
if(WIN32 OR APPLE OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
    satellite_add_pure_test(test_session_crypto
        tests/test_session_crypto.cpp
        src/net/session_crypto.cpp
    )
    satellite_link_sodium(test_session_crypto)

    # Portable PIN-rotation + shared-crypto-helper suite (net/pin_rotation.cpp,
    # the D11 hoist): runs on every lane so no platform can drift its own copy
    # again. SATELLITE_BUILD_TESTS gates the backdatePinClockForTest seam.
    satellite_add_pure_test(test_pin_rotation
        tests/test_pin_rotation.cpp
        src/net/pin_rotation.cpp
    )
    target_compile_definitions(test_pin_rotation PRIVATE SATELLITE_BUILD_TESTS)
    satellite_link_sodium(test_pin_rotation)
endif()

# macOS DS4 v2 report codec (platform/macos/ds4_report.h): descriptor pins,
# input packing, output parsing, feature blobs, and the probe-status seam.
# Pure/IOKit-free, but APPLE-gated: it exercises macOS platform headers.
if(APPLE)
    satellite_add_pure_test(test_macos_ds4_report tests/test_macos_ds4_report.cpp)
endif()

# In-process route tests (D10 payoff): registerAdminRoutes/registerClientRoutes
# put the PRODUCTION route tables on a loopback httplib::Server and the suites
# drive them with httplib::Client — auth machine codes, the 409 version gate,
# both pairing paths, session CRUD, the admin guard, and SSE presence. They
# link the real net/pairing/session layers against the per-OS platform
# adapters (config/crypto/globals/backend); only the gamepad bus is a stub
# inside the test. Gated to the lanes whose platform sources are desktop-free
# (the Windows list would drag tray/toast/COM surface into a test binary).
if(APPLE OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(SATELLITE_ROUTE_TEST_COMMON_SOURCES
        src/net/routes_common.cpp
        src/net/pairing.cpp
        src/net/pairing_keys.cpp
        src/net/pairing_service.cpp
        src/net/session_crypto.cpp
        src/net/pin_rotation.cpp
        src/net/local_iface.cpp
        src/core/config_io.cpp
        ${SATELLITE_CORE_SOURCES}
    )
    if(APPLE)
        set(SATELLITE_ROUTE_TEST_PLATFORM_SOURCES
            src/platform/macos/config.cpp
            src/platform/posix/config_posix.cpp
            src/platform/macos/globals.cpp
            src/platform/posix/crypto_posix.cpp
            src/platform/macos/gamepad_backend.cpp
            src/platform/macos/mac_hid_gamepad_adapter.cpp
        )
        set(SATELLITE_ROUTE_TEST_PLATFORM_DIR
            ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/macos)
    else()
        set(SATELLITE_ROUTE_TEST_PLATFORM_SOURCES
            src/platform/linux/config.cpp
            src/platform/posix/config_posix.cpp
            src/platform/linux/globals.cpp
            src/platform/posix/crypto_posix.cpp
            src/platform/linux/gamepad_backend.cpp
            src/platform/linux/gamepad_adapter.cpp
        )
        set(SATELLITE_ROUTE_TEST_PLATFORM_DIR
            ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/linux)
    endif()

    function(satellite_add_route_test name routes_src test_src)
        add_executable(${name}
            ${test_src}
            ${routes_src}
            ${SATELLITE_ROUTE_TEST_COMMON_SOURCES}
            ${SATELLITE_ROUTE_TEST_PLATFORM_SOURCES}
        )
        target_include_directories(${name} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${SATELLITE_ROUTE_TEST_PLATFORM_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/posix
        )
        target_include_directories(${name} SYSTEM PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/lib
        )
        # No CPPHTTPLIB_OPENSSL_SUPPORT: the tests exercise the route table on
        # a plain loopback server (TLS wrapping stays webserver.cpp's job).
        # SATELLITE_BUILD_TESTS keeps setAutoStart hermetic (launchctl /
        # desktop registration elided, same seam as the platform suites).
        target_compile_definitions(${name} PRIVATE
            CPPHTTPLIB_NO_EXCEPTIONS
            SATELLITE_BUILD_TESTS
        )
        target_compile_options(${name} PRIVATE -O2 -Wall -Wextra)
        satellite_link_sodium(${name})
        find_package(Threads REQUIRED)
        target_link_libraries(${name} PRIVATE Threads::Threads)
        if(APPLE)
            # probeBackend -> MacHidGamepadAdapter (IOHIDUserDevice probe).
            target_link_libraries(${name} PRIVATE
                "-framework IOKit"
                "-framework CoreFoundation"
            )
        endif()
        set_target_properties(${name} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        )
        add_test(NAME ${name} COMMAND ${name})
    endfunction()

    satellite_add_route_test(test_routes_client
        src/net/routes_client.cpp tests/test_routes_client.cpp)
    satellite_add_route_test(test_routes_admin
        src/net/routes_admin.cpp tests/test_routes_admin.cpp)
endif()

# Catalog builder + the locale completeness gate: WEB_LANG_DIR points the test
# at the real web/lang catalogs so every controller-type string is asserted
# present in every supported locale (mirror of Android's MissingTranslation).
satellite_add_pure_test(test_catalog tests/test_catalog.cpp src/core/catalog.cpp)
target_compile_definitions(test_catalog PRIVATE
    WEB_LANG_DIR="${CMAKE_CURRENT_SOURCE_DIR}/web/lang"
)

# UpdateService spawns worker/timer threads, so this suite needs pthreads on
# Linux/macOS (MinGW links winpthread implicitly).
find_package(Threads REQUIRED)
target_link_libraries(test_update_service PRIVATE Threads::Threads)

# Exercises the Linux-only adapters that don't need root/uinput/D-Bus:
# JSON helpers, config persistence, XDG autostart .desktop file handling,
# and path utilities.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(test_linux_platform
        tests/test_linux_platform.cpp
        src/platform/linux/config.cpp
        src/platform/posix/config_posix.cpp
        src/core/config_io.cpp
        src/platform/linux/globals.cpp
        src/platform/linux/gamepad_adapter.cpp
        ${SATELLITE_CORE_SOURCES}
    )
    target_include_directories(test_linux_platform PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/linux
        ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/posix
    )
    target_include_directories(test_linux_platform SYSTEM PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/lib
    )
    # SATELLITE_BUILD_TESTS gates test-only seams (e.g. the synthetic
    # invokeLightbarForTest entrypoint); the symbol is elided from
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

    # Real-kernel uinput smoke test: plugs virtual pads through the production
    # adapter and reads them back from /dev/input as a game would, including
    # the FF_RUMBLE upload → rumble-callback round trip. Needs a writable
    # /dev/uinput plus permission to open the created event nodes; exits 77
    # when either is missing so ctest reports "skipped" (CI runs it via sudo
    # after `modprobe uinput`, dev machines without the module skip cleanly).
    add_executable(test_uinput_smoke
        tests/test_uinput_smoke.cpp
        src/platform/linux/config.cpp
        src/platform/posix/config_posix.cpp
        src/core/config_io.cpp
        src/platform/linux/globals.cpp
        src/platform/linux/gamepad_adapter.cpp
        src/platform/linux/gamepad_backend.cpp
        ${SATELLITE_CORE_SOURCES}
    )
    target_include_directories(test_uinput_smoke PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/linux
        ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/posix
    )
    target_include_directories(test_uinput_smoke SYSTEM PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/lib
    )
    target_compile_definitions(test_uinput_smoke PRIVATE
        CPPHTTPLIB_NO_EXCEPTIONS
        SATELLITE_BUILD_TESTS
    )
    target_compile_options(test_uinput_smoke PRIVATE -O2 -Wall -Wextra)
    target_link_libraries(test_uinput_smoke PRIVATE Threads::Threads)
    set_target_properties(test_uinput_smoke PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    add_test(NAME uinput_smoke COMMAND test_uinput_smoke)
    set_tests_properties(uinput_smoke PROPERTIES SKIP_RETURN_CODE 77)
endif()

# Exercises the macOS-only adapters that don't need an entitled process or a
# desktop session: config persistence under ~/Library/Application Support,
# LaunchAgent autostart plist handling, the keyfile-backed dpapi* keystore,
# path utilities, and PIN rotation semantics. HOME is redirected into a
# per-test tmpdir by the harness, and launchctl registration is compiled out
# under SATELLITE_BUILD_TESTS, so the suite never touches real user state.
if(APPLE)
    add_executable(test_macos_platform
        tests/test_macos_platform.cpp
        src/platform/macos/config.cpp
        src/platform/posix/config_posix.cpp
        src/core/config_io.cpp
        src/platform/macos/globals.cpp
        src/platform/posix/crypto_posix.cpp
        src/net/pin_rotation.cpp
    )
    target_include_directories(test_macos_platform PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/macos
        ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/posix
    )
    target_include_directories(test_macos_platform SYSTEM PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/lib
    )
    # SATELLITE_BUILD_TESTS gates the backdatePinClockForTest seam and elides
    # launchctl registration (see the linux platform suite for the pattern).
    target_compile_definitions(test_macos_platform PRIVATE
        CPPHTTPLIB_NO_EXCEPTIONS
        SATELLITE_BUILD_TESTS
    )
    target_compile_options(test_macos_platform PRIVATE -O2 -Wall -Wextra)
    satellite_link_sodium(test_macos_platform)
    find_package(Threads REQUIRED)
    target_link_libraries(test_macos_platform PRIVATE Threads::Threads)
    set_target_properties(test_macos_platform PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    add_test(NAME macos_platform COMMAND test_macos_platform)
endif()

# Real-kernel IOHIDUserDevice smoke test: drives the production macOS adapter.
# The probe-fallback contract and the rumble/lightbar callback fan-out run on
# ANY Mac; actual kernel-device creation additionally needs the
# com.apple.developer.hid.virtual.device entitlement, so an unentitled process
# (CI runners, dev machines) exits 77 after the fallback assertions — wired to
# SKIP_RETURN_CODE so ctest reports "skipped", never a silent pass (mirrors
# the uinput smoke above).
if(APPLE)
    add_executable(test_mac_hid_smoke
        tests/test_mac_hid_smoke.cpp
        src/platform/macos/mac_hid_gamepad_adapter.cpp
        src/platform/macos/gamepad_backend.cpp
    )
    target_include_directories(test_mac_hid_smoke PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/macos
    )
    # Gates the injectOutputReportForTest seam, like the linux platform suite.
    target_compile_definitions(test_mac_hid_smoke PRIVATE SATELLITE_BUILD_TESTS)
    target_compile_options(test_mac_hid_smoke PRIVATE -O2 -Wall -Wextra)
    target_link_libraries(test_mac_hid_smoke PRIVATE
        "-framework IOKit"
        "-framework CoreFoundation"
    )
    set_target_properties(test_mac_hid_smoke PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    add_test(NAME mac_hid_smoke COMMAND test_mac_hid_smoke)
    set_tests_properties(mac_hid_smoke PROPERTIES SKIP_RETURN_CODE 77)
endif()

# Exercises the Windows-only adapters that don't need a desktop session,
# ViGEm bus driver, or paired client: JSON helpers, config persistence,
# HKCU\...\Run autostart, and path utilities. Both APPDATA config and
# the Run-key value are snapshotted/restored by the test harness so the
# suite is safe to run on a developer machine as well as in CI.
if(WIN32)
    add_executable(test_windows_platform
        tests/test_windows_platform.cpp
        src/platform/windows/config.cpp
        src/core/config_io.cpp
        src/platform/windows/globals.cpp
        src/platform/windows/crypto.cpp
        src/net/pin_rotation.cpp
        src/net/pairing_keys.cpp
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
        target_include_directories(test_windows_platform SYSTEM PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/lib/libsodium/libsodium-win64/include)
        target_link_directories(test_windows_platform PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/lib/libsodium/libsodium-win64/lib)
        target_link_libraries(test_windows_platform PRIVATE sodium)
    elseif(MSVC)
        target_compile_options(test_windows_platform PRIVATE /O2 /W4)
        find_package(unofficial-sodium CONFIG REQUIRED)
        target_link_libraries(test_windows_platform PRIVATE unofficial-sodium::sodium)
    endif()
    set_target_properties(test_windows_platform PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    add_test(NAME windows_platform COMMAND test_windows_platform)
endif()

# Links the real vigem_adapter.cpp against an in-test fake ViGEmBus driver
# layer (the free functions from vigem.cpp) so the submit IO path can be
# exercised without the driver. Pins that submits use the SYNCHRONOUS helpers
# (never fire-and-forget) and that a DS4 EX rejection falls back to the basic
# DS4 report (the regression that silently killed PlayStation input).
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

# Robustness fuzzing (opt-in, D-series fuzz harness): drives the receiver's
# decrypt -> inner_dispatch pipeline (net/session_crypto + net/inner_dispatch
# + core SessionService) with attacker-controlled datagram bytes, asserting
# "malformed datagrams never crash or read out of bounds". Clang-only. When
# the toolchain ships the libFuzzer runtime the target is a real fuzzer
# (ASan+UBSan+fuzzer); where it doesn't (e.g. Apple Command Line Tools) it
# degrades to a corpus-replay + deterministic-sweep driver under the same
# sanitizers, so the harness still executes everywhere. The linux lane runs
# the libFuzzer build bounded (see .github/workflows/linux-ci.yml).
option(SATELLITE_FUZZ
    "Build the decrypt->dispatch fuzz harness (clang; libFuzzer when available)" OFF)
if(SATELLITE_FUZZ)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR
            "SATELLITE_FUZZ requires clang (libFuzzer/sanitizer flags); "
            "configure with CC=clang CXX=clang++.")
    endif()

    include(CheckCXXSourceCompiles)
    set(CMAKE_REQUIRED_FLAGS "-fsanitize=fuzzer")
    check_cxx_source_compiles(
        "extern \"C\" int LLVMFuzzerTestOneInput(const unsigned char*, unsigned long) { return 0; }"
        SATELLITE_HAVE_LIBFUZZER)
    unset(CMAKE_REQUIRED_FLAGS)

    add_executable(fuzz_receiver_dispatch
        tests/fuzz/fuzz_receiver_dispatch.cpp
        src/net/inner_dispatch.cpp
        src/net/session_crypto.cpp
        ${SATELLITE_CORE_SOURCES}
    )
    target_include_directories(fuzz_receiver_dispatch PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    satellite_link_sodium(fuzz_receiver_dispatch)
    if(SATELLITE_HAVE_LIBFUZZER)
        target_compile_options(fuzz_receiver_dispatch PRIVATE
            -g -O1 -fno-omit-frame-pointer -fsanitize=fuzzer,address,undefined)
        target_link_options(fuzz_receiver_dispatch PRIVATE
            -fsanitize=fuzzer,address,undefined)
    else()
        message(STATUS "satellite: libFuzzer runtime unavailable; building the "
                       "standalone corpus-replay fuzz driver (ASan/UBSan).")
        target_compile_definitions(fuzz_receiver_dispatch PRIVATE
            SATELLITE_FUZZ_STANDALONE)
        target_compile_options(fuzz_receiver_dispatch PRIVATE
            -g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined)
        target_link_options(fuzz_receiver_dispatch PRIVATE
            -fsanitize=address,undefined)
    endif()
    set_target_properties(fuzz_receiver_dispatch PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )

    # Corpus regression rides ctest whenever the harness is configured:
    # libFuzzer's -runs=0 executes each seed exactly once; the standalone
    # driver replays the corpus and then its deterministic sweep.
    if(SATELLITE_HAVE_LIBFUZZER)
        add_test(NAME fuzz_receiver_dispatch_corpus COMMAND fuzz_receiver_dispatch
            -runs=0 ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus)
    else()
        add_test(NAME fuzz_receiver_dispatch_corpus COMMAND fuzz_receiver_dispatch
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus)
    endif()
endif()
