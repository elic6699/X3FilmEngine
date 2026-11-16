# Pinned standalone LibRaw build; no download or Python at configure/runtime.
#
# 2026-11-16: 依赖源从上游 LibRaw 换成产品自有 fork（github.com/elic6699/LibRaw），
# 钉在 commit cf02d66e（= 上游 0.22.3 tag 的树，tree e1ff88e6）。fork 的意义是后续
# X3Lumina 需要的 LibRaw 侧改动直接提交在 fork 上，不再只靠 engine/patches 覆盖；
# 但覆盖机制暂时保留（见下），因为 OpenMP 那两处 pragma 还没进 fork。
set(SPEKTRALAB_LIBRAW_SOURCE "${SPEKTRALAB_DEPS_DIR}/LibRaw-0.22.3" CACHE PATH "LibRaw source from the elic6699/LibRaw fork (0.22.3 base)")
if(NOT EXISTS "${SPEKTRALAB_LIBRAW_SOURCE}/libraw/libraw.h")
    message(FATAL_ERROR "Native RAW requires the pinned LibRaw (fork elic6699/LibRaw @ 0.22.3); run engine/setup-libraw.ps1 (Windows) or engine/setup-libraw.sh (POSIX) and set SPEKTRALAB_LIBRAW_SOURCE")
endif()

# 2026-11-14: LibRaw 的两个逐像素大循环（scale_colors_loop / convert_to_rgb_loop）
# 在原版是单线程，42MP 全幅各扫 1.27 亿像素 × 4 通道。开 /openmp 只对 AHD 去马赛克
# 生效（那部分在 demosaic/*.cpp），这两个 loop 需要显式加 pragma。改动以整文件
# 覆盖的方式保存在 engine/patches/ 下：configure 时若发现依赖树里的文件没打过标记，
# 就自动覆盖一次，这样重新解压 LibRaw 之后也不会静默退化回单线程。
#
# 补丁文件不带版本号：它在 0.22.0 / 0.22.2 / 0.22.3 之间逐字节一致（只多了那两个
# pragma），所以换版本不需要重做。⚠️ 一旦 fork 自己改了 postprocessing_utils.cpp，
# 覆盖会把它盖掉 —— 到那时先把 fork 的改动摘出来，再决定留哪边。
set(_spk_libraw_omp_patch "${CMAKE_CURRENT_LIST_DIR}/../patches/LibRaw-postprocessing_utils.cpp")
set(_spk_libraw_postproc "${SPEKTRALAB_LIBRAW_SOURCE}/src/postprocessing/postprocessing_utils.cpp")
if(EXISTS "${_spk_libraw_omp_patch}" AND EXISTS "${_spk_libraw_postproc}")
    file(READ "${_spk_libraw_postproc}" _spk_libraw_postproc_text)
    string(FIND "${_spk_libraw_postproc_text}" "(X3Lumina)" _spk_libraw_patched)
    if(_spk_libraw_patched EQUAL -1)
        message(STATUS "LibRaw: applying X3Lumina OpenMP patch to postprocessing_utils.cpp")
        configure_file("${_spk_libraw_omp_patch}" "${_spk_libraw_postproc}" COPYONLY)
    endif()
    unset(_spk_libraw_postproc_text)
    unset(_spk_libraw_patched)
endif()
file(STRINGS "${SPEKTRALAB_LIBRAW_SOURCE}/libraw/libraw_version.h" libraw_version REGEX "^#define LIBRAW_(MAJOR|MINOR|PATCH)_VERSION")
list(FIND libraw_version "#define LIBRAW_MAJOR_VERSION 0" libraw_major)
list(FIND libraw_version "#define LIBRAW_MINOR_VERSION 22" libraw_minor)
list(FIND libraw_version "#define LIBRAW_PATCH_VERSION 3" libraw_patch)
if(libraw_major EQUAL -1 OR libraw_minor EQUAL -1 OR libraw_patch EQUAL -1)
    message(FATAL_ERROR "This RAW integration is validated with LibRaw 0.22.3 (fork elic6699/LibRaw) only")
endif()
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

# 2026-11-14: AHD / X-Trans 去马赛克是通用 RAW 链里最贵的一段。LibRaw 自带
# tile 级 OpenMP（src/demosaic/ahd_demosaic.cpp、xtrans_demosaic.cpp 的
# `#pragma omp parallel for schedule(dynamic)`，还有 raw2image / postprocessing_aux），
# 但只有编译器开了 /openmp 才会定义 _OPENMP，libraw_types.h 依赖它自动
# define LIBRAW_USE_OPENMP；不开就整段退化成单线程（之前一直是单线程）。
# 全幅 7968x5320 AHD 单线程约 8.6 s，这里是主要瓶颈。
find_package(OpenMP QUIET COMPONENTS CXX)
if(OpenMP_CXX_FOUND)
    target_compile_options(spk_libraw PRIVATE ${OpenMP_CXX_FLAGS})
    target_link_libraries(spk_libraw PUBLIC OpenMP::OpenMP_CXX)
    message(STATUS "LibRaw: OpenMP demosaic enabled (${OpenMP_CXX_FLAGS})")
else()
    message(STATUS "LibRaw: OpenMP not found, demosaic stays single-threaded")
endif()
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
