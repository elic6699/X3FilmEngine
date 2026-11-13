/* glib.h — minimal GLib compatibility layer for building the *unmodified*
 * official Lensfun 0.3.4 sources with MSVC, without shipping glib-2.0 DLLs.
 *
 * Only the subset used by libs/lensfun is implemented:
 *   memory / strings, GPtrArray, GString, GError, GDir, GPatternSpec,
 *   GMarkup (a self-contained SAX parser), UTF-8 helpers and mutexes.
 *
 * This is NOT a general-purpose GLib replacement. Do not use it elsewhere.
 */
#ifndef X3L_GLIB_SHIM_H
#define X3L_GLIB_SHIM_H

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef char               gchar;
typedef unsigned char      guchar;
typedef int                gint;
typedef unsigned int       guint;
typedef uint32_t           guint32;
typedef uint64_t           guint64;
typedef size_t             gsize;
typedef ptrdiff_t          gssize;
typedef int                gboolean;
typedef void              *gpointer;
typedef const void        *gconstpointer;
typedef unsigned int       gunichar;
typedef unsigned int       GQuark;

typedef int  (*GCompareFunc)   (gconstpointer a, gconstpointer b);
typedef void (*GDestroyNotify) (gpointer data);

#define TRUE  1
#define FALSE 0

/* Force cpuid.cpp onto the GStaticMutex branch (simpler to emulate). */
#define GLIB_CHECK_VERSION(major, minor, micro) 0
#define GLIB_VERSION_2_26 0
#define GLIB_VERSION_2_40 0

/* ---------------------------------------------------------------- memory */
void  *g_malloc  (gsize n);
void  *g_realloc (void *p, gsize n);
void   g_free    (void *p);
gchar *g_strdup  (const gchar *s);
#define g_new(type, n) ((type *) g_malloc (sizeof (type) * (gsize) (n)))
#define g_assert(expr) do { if (!(expr)) abort (); } while (0)
void   g_warning (const gchar *format, ...);
int    g_str_has_prefix (const gchar *str, const gchar *prefix);

/* ------------------------------------------------------------- GPtrArray */
typedef struct {
    void **pdata;
    guint  len;
} GPtrArray;

GPtrArray *g_ptr_array_new            (void);
void       g_ptr_array_add            (GPtrArray *a, gpointer data);
void       g_ptr_array_set_size       (GPtrArray *a, gint length);
gpointer   g_ptr_array_remove_index_fast (GPtrArray *a, guint index);
void       g_ptr_array_remove_range   (GPtrArray *a, guint index, guint length);
void     **g_ptr_array_free           (GPtrArray *a, gboolean free_segment);
#define g_ptr_array_index(a, i) ((a)->pdata [(i)])

/* --------------------------------------------------------------- GString */
typedef struct {
    gchar *str;
    gsize  len;
    gsize  allocated_len;
} GString;

GString *g_string_sized_new (gsize default_size);
GString *g_string_append    (GString *s, const gchar *val);
gchar   *g_string_free      (GString *s, gboolean free_segment);

/* ---------------------------------------------------------------- GError */
typedef struct {
    GQuark domain;
    gint   code;
    gchar *message;
} GError;

#define G_MARKUP_ERROR              ((GQuark) 1)
#define G_MARKUP_ERROR_UNKNOWN_ATTRIBUTE  1
#define G_MARKUP_ERROR_INVALID_CONTENT    2
#define G_FILE_ERROR_ACCES          13
#define G_FILE_TEST_IS_DIR          1

void g_set_error (GError **err, GQuark domain, gint code,
                  const gchar *format, ...);

/* -------------------------------------------------------------- GMarkup */
typedef struct GMarkupParseContext GMarkupParseContext;
typedef int GMarkupParseFlags;

typedef void (*GMarkupStartElementFunc) (GMarkupParseContext *ctx,
                                         const gchar *element_name,
                                         const gchar **attribute_names,
                                         const gchar **attribute_values,
                                         gpointer user_data, GError **error);
typedef void (*GMarkupEndElementFunc)   (GMarkupParseContext *ctx,
                                         const gchar *element_name,
                                         gpointer user_data, GError **error);
typedef void (*GMarkupTextFunc)         (GMarkupParseContext *ctx,
                                         const gchar *text, gsize text_len,
                                         gpointer user_data, GError **error);
typedef void (*GMarkupPassThroughFunc)  (GMarkupParseContext *ctx,
                                         const gchar *pass_through,
                                         gsize text_len, gpointer user_data,
                                         GError **error);
typedef void (*GMarkupErrorFunc)        (GMarkupParseContext *ctx,
                                         GError *error, gpointer user_data);

typedef struct {
    GMarkupStartElementFunc  start_element;
    GMarkupEndElementFunc    end_element;
    GMarkupTextFunc          text;
    GMarkupPassThroughFunc   passthrough;
    GMarkupErrorFunc         error;
} GMarkupParser;

GMarkupParseContext *g_markup_parse_context_new   (const GMarkupParser *parser,
                                                   GMarkupParseFlags flags,
                                                   gpointer user_data,
                                                   GDestroyNotify notify);
gboolean  g_markup_parse_context_parse           (GMarkupParseContext *ctx,
                                                   const gchar *text,
                                                   gssize text_len,
                                                   GError **error);
void      g_markup_parse_context_free            (GMarkupParseContext *ctx);
const gchar *g_markup_parse_context_get_element (GMarkupParseContext *ctx);
void      g_markup_parse_context_get_position   (GMarkupParseContext *ctx,
                                                  gint *line, gint *col);
gchar    *g_markup_vprintf_escaped (const gchar *format, va_list args);
gchar    *g_markup_printf_escaped  (const gchar *format, ...);

/* -------------------------------------------------------------- GDir etc */
typedef struct GDir          GDir;
typedef struct GPatternSpec  GPatternSpec;

GDir         *g_dir_open        (const gchar *path, guint flags, GError **error);
const gchar  *g_dir_read_name   (GDir *dir);
void          g_dir_close       (GDir *dir);
GPatternSpec *g_pattern_spec_new  (const gchar *pattern);
void          g_pattern_spec_free (GPatternSpec *ps);
gboolean      g_pattern_match    (GPatternSpec *ps, guint string_length,
                                  const gchar *string,
                                  const gchar *string_reversed);

gchar *g_build_filename   (const gchar *first, ...);
const gchar *g_get_user_data_dir (void);
gboolean g_file_get_contents (const gchar *filename, gchar **contents,
                              gsize *length, GError **error);
gboolean g_file_test (const gchar *filename, gint test);

/* glib/gstdio.h */
int g_mkdir (const gchar *filename, int mode);
int g_open  (const gchar *filename, int flags, int mode);

/* --------------------------------------------------------------- UTF-8 */
gunichar    g_utf8_get_char  (const gchar *p);
gchar      *g_utf8_next_char (const gchar *p);
gboolean    g_unichar_isspace (gunichar c);
gunichar    g_unichar_tolower (gunichar c);
gchar      *g_utf8_casefold   (const gchar *str, gssize len);

/* --------------------------------------------------------------- mutex */
typedef struct { int unused; } GMutex;
typedef struct { int unused; } GStaticMutex;
#define G_STATIC_MUTEX_INIT { 0 }
void g_mutex_lock              (GMutex *m);
void g_mutex_unlock            (GMutex *m);
void g_static_mutex_lock       (GStaticMutex *m);
void g_static_mutex_unlock     (GStaticMutex *m);

#ifdef __cplusplus
}
#endif
#endif /* X3L_GLIB_SHIM_H */
