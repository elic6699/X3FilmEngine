# Lensfun 0.3.4 (official sources) built statically into spektralab-host.
#
# The upstream CMake project requires GLib 2.x (and, on Windows, a matching
# glib DLL). We instead compile the *unmodified* Lensfun sources against
# third_party/lensfun_shim, a small GLib-compatible layer implemented with
# Win32 + the C++ standard library. No extra DLLs are shipped.
#
# Dependencies are pinned and fetched out-of-tree by setup-lensfun.ps1 into
# build/deps/lensfun-0.3.4, exactly like LibRaw-0.22.2.
set(SPEKTRALAB_LENSFUN_SOURCE "${SPEKTRALAB_DEPS_DIR}/lensfun-0.3.4" CACHE PATH
    "Extracted official Lensfun 0.3.4 source tree")
if(NOT EXISTS "${SPEKTRALAB_LENSFUN_SOURCE}/libs/lensfun/lens.cpp")
    message(FATAL_ERROR "Lensfun 0.3.4 sources missing; run engine/setup-lensfun.ps1 and set SPEKTRALAB_LENSFUN_SOURCE")
endif()

set(LF_ROOT "${SPEKTRALAB_LENSFUN_SOURCE}")
set(LF_SHIM "${CMAKE_CURRENT_SOURCE_DIR}/third_party/lensfun_shim")
set(LF_GEN  "${CMAKE_CURRENT_BINARY_DIR}/lensfun_gen")
file(MAKE_DIRECTORY "${LF_GEN}")

# lensfun.h has only four @VERSION_*@ substitutions.
set(VERSION_MAJOR 0)
set(VERSION_MINOR 3)
set(VERSION_MICRO 4)
set(VERSION_BUGFIX 0)
configure_file("${LF_ROOT}/include/lensfun/lensfun.h.in" "${LF_GEN}/lensfun.h" @ONLY)

set(LF_LIB_SOURCES
    "${LF_ROOT}/libs/lensfun/camera.cpp"
    "${LF_ROOT}/libs/lensfun/database.cpp"
    "${LF_ROOT}/libs/lensfun/lens.cpp"
    "${LF_ROOT}/libs/lensfun/mount.cpp"
    "${LF_ROOT}/libs/lensfun/cpuid.cpp"
    "${LF_ROOT}/libs/lensfun/auxfun.cpp"
    "${LF_ROOT}/libs/lensfun/modifier.cpp"
    "${LF_ROOT}/libs/lensfun/mod-color.cpp"
    "${LF_ROOT}/libs/lensfun/mod-color-sse.cpp"
    "${LF_ROOT}/libs/lensfun/mod-color-sse2.cpp"
    "${LF_ROOT}/libs/lensfun/mod-coord.cpp"
    "${LF_ROOT}/libs/lensfun/mod-coord-sse.cpp"
    "${LF_ROOT}/libs/lensfun/mod-subpix.cpp"
)

# POSIX <regex.h> is absent on MSVC; Lensfun ships the TRE implementation.
file(GLOB LF_TRE_SOURCES "${LF_ROOT}/libs/regex/*.c")

add_library(spk_lensfun STATIC
    ${LF_LIB_SOURCES}
    ${LF_TRE_SOURCES}
    "${LF_SHIM}/glib_shim.cpp"
)

target_include_directories(spk_lensfun PUBLIC "${LF_GEN}" "${LF_ROOT}/include/lensfun")
target_include_directories(spk_lensfun PRIVATE
    "${LF_SHIM}"
    "${LF_ROOT}/libs/lensfun"
    "${LF_ROOT}/include/regex"
    "${LF_ROOT}/libs/regex"
)

# CONF_LENSFUN_STATIC must also be visible to consumers: lensfun.h uses it to
# decide whether LF_EXPORT is empty (static) or __declspec(dllimport).
target_compile_definitions(spk_lensfun PUBLIC CONF_LENSFUN_STATIC)
target_compile_features(spk_lensfun PUBLIC cxx_std_11)
set_target_properties(spk_lensfun PROPERTIES POSITION_INDEPENDENT_CODE ON)

set_source_files_properties(${LF_TRE_SOURCES} PROPERTIES
    LANGUAGE C
    COMPILE_DEFINITIONS "HAVE_SYS_TYPES_H;TRE_REGEX_T_FIELD=value;TRE_VERSION=\"0.8.0\";inline=_inline"
)
if(MSVC)
    set_source_files_properties(${LF_TRE_SOURCES} PROPERTIES
        COMPILE_OPTIONS "/utf-8")
endif()

if(MSVC)
    # Lensfun is third-party code; its warnings are not actionable here.
    target_compile_options(spk_lensfun PRIVATE /W1 /wd4244 /wd4996 /wd4267 /wd4305)
endif()

# Stage the lens database next to the host's engine resources at packaging time.
set(SPEKTRALAB_LENSFUN_DB "${LF_ROOT}/data/db" CACHE INTERNAL
    "Lensfun XML lens database directory (CC BY-SA 3.0)")
file(GLOB LF_DB_XML "${SPEKTRALAB_LENSFUN_DB}/*.xml")
list(LENGTH LF_DB_XML LF_DB_COUNT)
if(LF_DB_COUNT EQUAL 0)
    message(FATAL_ERROR "No Lensfun XML database found in ${SPEKTRALAB_LENSFUN_DB}")
endif()
set(LF_DB_DST "${CMAKE_BINARY_DIR}/engine/resources/lensfun/version_1")
file(MAKE_DIRECTORY "${LF_DB_DST}")
foreach(_xml IN LISTS LF_DB_XML)
    file(COPY "${_xml}" DESTINATION "${LF_DB_DST}")
endforeach()
message(STATUS "Lensfun 0.3.4: staged ${LF_DB_COUNT} lens database XML files")
