# Pinned standalone LibRaw build; no download or Python at configure/runtime.
# The tree is the project fork pinned by engine/setup-libraw.sh (POSIX) or
# engine/setup-libraw.ps1 (Windows), which records the verified path in
# <deps>/libraw-source.path. That file is the single copy of the pin; this
# module and build-host.sh both read it.
if(NOT SPEKTRALAB_LIBRAW_SOURCE AND EXISTS "${SPEKTRALAB_DEPS_DIR}/libraw-source.path")
    file(STRINGS "${SPEKTRALAB_DEPS_DIR}/libraw-source.path" SPEKTRALAB_LIBRAW_SOURCE LIMIT_COUNT 1)
endif()
if(NOT SPEKTRALAB_LIBRAW_SOURCE)
    set(SPEKTRALAB_LIBRAW_SOURCE "${SPEKTRALAB_DEPS_DIR}/LibRaw-e419de0")
endif()
set(SPEKTRALAB_LIBRAW_SOURCE "${SPEKTRALAB_LIBRAW_SOURCE}" CACHE PATH "LibRaw source tree pinned by engine/setup-libraw.sh")
if(NOT EXISTS "${SPEKTRALAB_LIBRAW_SOURCE}/libraw/libraw.h")
    message(FATAL_ERROR "Native RAW requires the pinned LibRaw source; run engine/setup-libraw.ps1 (Windows) or engine/setup-libraw.sh (POSIX) and set SPEKTRALAB_LIBRAW_SOURCE")
endif()
# The fork is the upstream 0.22 development line: its version.h still says
# 0.22.0 Release while carrying post-0.22.2 fixes, so the gate is the 0.22
# series rather than one patch release. Any other series is refused.
file(STRINGS "${SPEKTRALAB_LIBRAW_SOURCE}/libraw/libraw_version.h" libraw_version REGEX "^#define LIBRAW_(MAJOR|MINOR|PATCH)_VERSION")
foreach(libraw_line IN LISTS libraw_version)
    if(libraw_line MATCHES "LIBRAW_MAJOR_VERSION ([0-9]+)")
        set(libraw_major "${CMAKE_MATCH_1}")
    elseif(libraw_line MATCHES "LIBRAW_MINOR_VERSION ([0-9]+)")
        set(libraw_minor "${CMAKE_MATCH_1}")
    elseif(libraw_line MATCHES "LIBRAW_PATCH_VERSION ([0-9]+)")
        set(libraw_patch "${CMAKE_MATCH_1}")
    endif()
endforeach()
if(NOT libraw_major STREQUAL "0" OR NOT libraw_minor STREQUAL "22")
    message(FATAL_ERROR "This RAW integration is validated with the LibRaw 0.22 series only (found ${libraw_major}.${libraw_minor}.${libraw_patch})")
endif()
message(STATUS "LibRaw ${libraw_major}.${libraw_minor}.${libraw_patch} from ${SPEKTRALAB_LIBRAW_SOURCE}")
file(GLOB_RECURSE libraw_sources CONFIGURE_DEPENDS "${SPEKTRALAB_LIBRAW_SOURCE}/src/*.cpp")
# *_ph.cpp are replacement placeholders for LibRaw's no-postprocessing build,
# not extra implementations. Including them beside the real processing units
# makes small metadata-only consumers fail with duplicate symbols.
list(FILTER libraw_sources EXCLUDE REGEX "/[^/]*_ph\\.cpp$")
add_library(spk_libraw STATIC ${libraw_sources})
target_include_directories(spk_libraw PUBLIC "${SPEKTRALAB_LIBRAW_SOURCE}")
target_compile_definitions(spk_libraw PUBLIC LIBRAW_NODLL PRIVATE LIBRAW_NOTHREADS)
target_compile_features(spk_libraw PUBLIC cxx_std_11)
set_target_properties(spk_libraw PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(WIN32)
    target_link_libraries(spk_libraw PUBLIC ws2_32)
else()
    target_link_libraries(spk_libraw PUBLIC m)
endif()
# LibRaw's own sources are not ours to warn about.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(spk_libraw PRIVATE -w)
endif()
# Select the LGPL-2.1 option offered by the LibRaw dual license.
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/third-party/LibRaw")
foreach(notice LICENSE.LGPL COPYRIGHT)
    configure_file("${SPEKTRALAB_LIBRAW_SOURCE}/${notice}" "third-party/LibRaw/${notice}" COPYONLY)
endforeach()
