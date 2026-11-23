vcpkg_download_distfile(ARCHIVE
    URLS "https://github.com/getsentry/sentry-native/releases/download/${VERSION}/sentry-native.zip"
    FILENAME "sentry-native-${VERSION}.zip"
    SHA512 68af23e3075265c5c2a74e1967dc9bb87eef119a249bde26100db4f8d106c7424937aa15e64af3d5c50c239598633dae30d81230047e574177cc8ad530d3419a
)

vcpkg_extract_source_archive(
    SOURCE_PATH
    ARCHIVE "${ARCHIVE}"
    NO_REMOVE_ONE_LEVEL
    PATCHES
        fix-crashpad-wer.patch
        fix-wer-define.patch
        fix-usage-runtime.patch
        fix-cmake4.patch
)
file(REMOVE_RECURSE "${SOURCE_PATH}/external/crashpad/third_party/zlib/zlib")

vcpkg_list(SET options)

# Satellite's one delta from upstream vcpkg's port besides the header above:
# upstream allows the wer feature only for dynamic triplets, and Satellite
# links the x64-windows-static triplet and ships crashpad_wer.dll beside
# satellite.exe, so the DLL is expected inside a static package.
if("wer" IN_LIST FEATURES AND VCPKG_LIBRARY_LINKAGE STREQUAL "static")
    set(VCPKG_POLICY_DLLS_IN_STATIC_LIBRARY enabled)
endif()

if(NOT "backend" IN_LIST FEATURES)
    vcpkg_list(APPEND options "-DSENTRY_BACKEND=none")
elseif("wer" IN_LIST FEATURES)
    vcpkg_list(APPEND options "-DSENTRY_BACKEND=crashpad")
elseif(DEFINED SENTRY_BACKEND)
    # Legacy, possible override from triplet, but cannot handle dependencies
    vcpkg_list(APPEND options "-DSENTRY_BACKEND=${SENTRY_BACKEND}")
endif()

if(NOT "transport" IN_LIST FEATURES)
    vcpkg_list(APPEND options "-DSENTRY_TRANSPORT=none")
endif()

if("wer" IN_LIST FEATURES)
    vcpkg_list(APPEND options "-DSENTRY_TRANSPORT_CRASHPAD_USE_WER=ON")
endif()

if("compression" IN_LIST FEATURES)
    vcpkg_list(APPEND options "-DSENTRY_TRANSPORT_COMPRESSION=ON")
endif()

# sentry-native only looks for pkg-config on Linux, to locate the system libunwind for
# SENTRY_LIBUNWIND_SYSTEM. Acquire it directly instead of depending on the pkgconf port.
if(VCPKG_TARGET_IS_LINUX)
    vcpkg_find_acquire_program(PKGCONFIG)
    vcpkg_list(APPEND options "-DPKG_CONFIG_EXECUTABLE=${PKGCONFIG}")
endif()

if(VCPKG_TARGET_IS_WINDOWS AND NOT VCPKG_TARGET_IS_MINGW)
    set(VCPKG_CXX_FLAGS "/D_CRT_DECLARE_NONSTDC_NAMES ${VCPKG_CXX_FLAGS}")
    set(VCPKG_C_FLAGS "/D_CRT_DECLARE_NONSTDC_NAMES ${VCPKG_C_FLAGS}")
endif()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        ${options}
        -DSENTRY_BUILD_TESTS=OFF
        -DSENTRY_BUILD_EXAMPLES=OFF
        -DCRASHPAD_ZLIB_SYSTEM=ON
        -DSENTRY_LIBUNWIND_SYSTEM=ON
    MAYBE_UNUSED_VARIABLES
        CRASHPAD_ZLIB_SYSTEM
)

vcpkg_cmake_install()

vcpkg_copy_pdbs()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

# Upstream vcpkg also writes SENTRY_BUILD_STATIC into the installed sentry.h
# for a static build. Satellite consumes the port through sentry's CMake
# config, whose target already carries SENTRY_BUILD_STATIC=1 as a public
# definition, so the header's second, empty definition would be a macro
# redefinition (C4005) under /WX. The overlay leaves sentry.h as built.

vcpkg_cmake_config_fixup(PACKAGE_NAME sentry CONFIG_PATH lib/cmake/sentry)

if(EXISTS "${CURRENT_PACKAGES_DIR}/bin/crashpad_handler${VCPKG_TARGET_EXECUTABLE_SUFFIX}")
    vcpkg_copy_tools(TOOL_NAMES crashpad_handler AUTO_CLEAN)
endif()

vcpkg_install_copyright(
    FILE_LIST
        "${SOURCE_PATH}/LICENSE"
        "${SOURCE_PATH}/vendor/jsmn.h"
        "${SOURCE_PATH}/vendor/mpack.h"
        "${SOURCE_PATH}/vendor/stb_sprintf.h"
        "${SOURCE_PATH}/external/libunwindstack-ndk/LICENSE"
        "${SOURCE_PATH}/external/crashpad/LICENSE"
        "${SOURCE_PATH}/external/crashpad/third_party/getopt/LICENSE"
        "${SOURCE_PATH}/external/crashpad/third_party/lss/lss/LICENSE"
        "${SOURCE_PATH}/external/crashpad/third_party/mini_chromium/mini_chromium/LICENSE"
        "${SOURCE_PATH}/external/crashpad/third_party/mini_chromium/mini_chromium/base/third_party/icu/LICENSE"
        "${SOURCE_PATH}/external/crashpad/third_party/mpack/LICENSE"
)
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
