/* config.h - Lensfun 0.3.4 build configuration for the MSVC/static/GLib-shim
 * build inside spektralab-host. Mirrors include/lensfun/config.h.in.cmake with
 * the Windows, no-SSE, no-system-GLib choices fixed.
 *
 * SSE/SSE2 vectorisation is deliberately off: the scalar C++ paths are exact
 * and avoid MSVC intrinsics differences on older CPUs. Lens correction runs
 * once per decoded frame, so the difference is negligible.
 */
#ifndef __CONFIG_H__
#define __CONFIG_H__

#define CONF_PACKAGE "lensfun"
#define CONF_VERSION "0.3.4.0"

#define PLATFORM_WINDOWS 1

/* The database is relocatable: the host always passes an explicit directory to
 * lfDatabase::LoadDirectory(), so CONF_DATADIR is only a fallback. */
#define CONF_DATADIR "lensfun"
#define SYSTEM_DB_UPDATE_PATH "lensfun-updates"
#define DATABASE_SUBDIR "version_1"

#define CONF_LENSFUN_INTERNAL 1

#define GLIB_VERSION_MIN_REQUIRED (GLIB_VERSION_2_26)
#define GLIB_VERSION_MAX_ALLOWED (GLIB_VERSION_MIN_REQUIRED)

#ifdef _MSC_VER
#define _USE_MATH_DEFINES
#endif

#endif /* __CONFIG_H__ */
