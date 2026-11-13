/* glib_shim.cpp — implementation of the minimal GLib compatibility layer.
 * See glib.h for the contract. Built as part of spk_lensfun only.
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifdef _MSC_VER
#pragma warning(disable: 4190)  /* C-linkage helpers returning std::string */
#endif
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "glib.h"

#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <cstdio>
#include <cstdarg>
#include <cctype>

/* ------------------------------------------------------------ utf8 <-> utf16 */
static std::wstring Utf8ToWide(const char *s)
{
    if (!s || !*s) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    return w;
}

static std::string WideToUtf8(const wchar_t *w)
{
    if (!w || !*w) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s((size_t)n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

/* ------------------------------------------------------------------ memory */
extern "C" {

void *g_malloc(gsize n) { return malloc(n ? n : 1); }
void *g_realloc(void *p, gsize n) { return realloc(p, n ? n : 1); }
void  g_free(void *p) { free(p); }

gchar *g_strdup(const gchar *s)
{
    if (!s) return nullptr;
    size_t n = strlen(s) + 1;
    gchar *r = (gchar *)g_malloc(n);
    memcpy(r, s, n);
    return r;
}

int g_str_has_prefix(const gchar *str, const gchar *prefix)
{
    if (!str || !prefix) return 0;
    return strncmp(str, prefix, strlen(prefix)) == 0;
}

void g_warning(const gchar *format, ...)
{
    va_list ap;
    va_start(ap, format);
    fputs("[Lensfun/glib-shim] ", stderr);
    vfprintf(stderr, format, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* --------------------------------------------------------------- GPtrArray */
struct PtrArrayImpl : GPtrArray { guint cap; };

GPtrArray *g_ptr_array_new(void)
{
    PtrArrayImpl *a = (PtrArrayImpl *)calloc(1, sizeof(PtrArrayImpl));
    a->cap = 16;
    a->len = 0;
    a->pdata = (void **)calloc(a->cap, sizeof(void *));
    return a;
}

static void ptr_array_grow(PtrArrayImpl *a, guint need)
{
    if (need <= a->cap) return;
    guint cap = a->cap;
    while (cap < need) cap *= 2;
    void **p = (void **)realloc(a->pdata, (size_t)cap * sizeof(void *));
    if (!p) abort();
    memset(p + a->cap, 0, (size_t)(cap - a->cap) * sizeof(void *));
    a->pdata = p;
    a->cap = cap;
}

void g_ptr_array_add(GPtrArray *array, gpointer data)
{
    PtrArrayImpl *a = (PtrArrayImpl *)array;
    ptr_array_grow(a, a->len + 1);
    a->pdata[a->len++] = data;
}

void g_ptr_array_set_size(GPtrArray *array, gint length)
{
    PtrArrayImpl *a = (PtrArrayImpl *)array;
    if (length < 0) return;
    guint n = (guint)length;
    if (n > a->len) {
        ptr_array_grow(a, n);
        memset(a->pdata + a->len, 0, (size_t)(n - a->len) * sizeof(void *));
    }
    a->len = n;
}

gpointer g_ptr_array_remove_index_fast(GPtrArray *array, guint index)
{
    PtrArrayImpl *a = (PtrArrayImpl *)array;
    if (index >= a->len) return nullptr;
    gpointer old = a->pdata[index];
    a->pdata[index] = a->pdata[--a->len];
    a->pdata[a->len] = nullptr;
    return old;
}

void g_ptr_array_remove_range(GPtrArray *array, guint index, guint length)
{
    PtrArrayImpl *a = (PtrArrayImpl *)array;
    if (index >= a->len || length == 0) return;
    if (index + length > a->len) length = a->len - index;
    memmove(a->pdata + index, a->pdata + index + length,
            (size_t)(a->len - index - length) * sizeof(void *));
    a->len -= length;
    memset(a->pdata + a->len, 0, (size_t)length * sizeof(void *));
}

void **g_ptr_array_free(GPtrArray *array, gboolean free_segment)
{
    PtrArrayImpl *a = (PtrArrayImpl *)array;
    if (!a) return nullptr;
    void **p = a->pdata;
    if (free_segment) { free(p); p = nullptr; }
    free(a);
    return p;
}

/* ---------------------------------------------------------------- GString */
GString *g_string_sized_new(gsize default_size)
{
    GString *s = (GString *)calloc(1, sizeof(GString));
    s->allocated_len = default_size + 1;
    s->str = (gchar *)calloc(s->allocated_len, 1);
    s->len = 0;
    return s;
}

GString *g_string_append(GString *s, const gchar *val)
{
    if (!s || !val) return s;
    gsize add = strlen(val);
    gsize need = s->len + add + 1;
    if (need > s->allocated_len) {
        gsize cap = s->allocated_len;
        while (cap < need) cap *= 2;
        s->str = (gchar *)realloc(s->str, cap);
        s->allocated_len = cap;
    }
    memcpy(s->str + s->len, val, add + 1);
    s->len += add;
    return s;
}

gchar *g_string_free(GString *s, gboolean free_segment)
{
    if (!s) return nullptr;
    gchar *str = s->str;
    if (free_segment) { free(str); str = nullptr; }
    free(s);
    return str;
}

/* ----------------------------------------------------------------- GError */
void g_set_error(GError **err, GQuark domain, gint code,
                 const gchar *format, ...)
{
    if (!err || *err) return;
    GError *e = (GError *)calloc(1, sizeof(GError));
    e->domain = domain;
    e->code = code;
    char buf[1024];
    va_list ap;
    va_start(ap, format);
    vsnprintf(buf, sizeof(buf), format ? format : "", ap);
    va_end(ap);
    e->message = g_strdup(buf);
    *err = e;
}

static void clear_error(GError **err)
{
    if (err && *err) {
        g_free((*err)->message);
        g_free(*err);
        *err = nullptr;
    }
}

/* ------------------------------------------------------------- escaping */
static std::string xml_escape(const std::string &in)
{
    std::string out;
    out.reserve(in.size() + 16);
    for (unsigned char c : in) {
        switch (c) {
        case '&':  out += "&amp;";  break;
        case '<':  out += "&lt;";   break;
        case '>':  out += "&gt;";   break;
        case '"':  out += "&quot;"; break;
        case '\'': out += "&apos;"; break;
        default:   out += (char)c;  break;
        }
    }
    return out;
}

static std::string xml_unescape(const std::string &in)
{
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ) {
        if (in[i] != '&') { out += in[i++]; continue; }
        size_t semi = in.find(';', i + 1);
        if (semi == std::string::npos || semi - i > 12) { out += in[i++]; continue; }
        std::string ent = in.substr(i + 1, semi - i - 1);
        if      (ent == "amp")  out += '&';
        else if (ent == "lt")   out += '<';
        else if (ent == "gt")   out += '>';
        else if (ent == "quot") out += '"';
        else if (ent == "apos") out += '\'';
        else if (ent.size() > 1 && ent[0] == '#') {
            int cp = 0;
            if (ent[1] == 'x' || ent[1] == 'X')
                cp = (int)strtol(ent.c_str() + 2, nullptr, 16);
            else
                cp = atoi(ent.c_str() + 1);
            /* Encode as UTF-8 (BMP subset is enough for lensfun DB). */
            if (cp > 0 && cp < 0x80) out += (char)cp;
            else if (cp < 0x800) {
                out += (char)(0xC0 | (cp >> 6));
                out += (char)(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                out += (char)(0xE0 | (cp >> 12));
                out += (char)(0x80 | ((cp >> 6) & 0x3F));
                out += (char)(0x80 | (cp & 0x3F));
            }
        } else { out += in[i++]; continue; }
        i = semi + 1;
    }
    return out;
}

gchar *g_markup_vprintf_escaped(const gchar *format, va_list args)
{
    va_list copy;
    va_copy(copy, args);
    int n = vsnprintf(nullptr, 0, format, copy);
    va_end(copy);
    if (n < 0) n = 0;
    std::string raw((size_t)n, '\0');
    if (n > 0) vsnprintf(&raw[0], (size_t)n + 1, format, args);
    std::string esc = xml_escape(raw);
    gchar *r = (gchar *)g_malloc(esc.size() + 1);
    memcpy(r, esc.c_str(), esc.size() + 1);
    return r;
}

gchar *g_markup_printf_escaped(const gchar *format, ...)
{
    va_list ap;
    va_start(ap, format);
    gchar *r = g_markup_vprintf_escaped(format, ap);
    va_end(ap);
    return r;
}

/* -------------------------------------------------------------- GMarkup */
struct GMarkupParseContext {
    const GMarkupParser *parser = nullptr;
    gpointer user_data = nullptr;
    /* GMarkup keeps element names alive for the lifetime of the context:
     * Lensfun stores the raw element_name pointers in its own parse stack and
     * reads them in later callbacks. A deque gives stable addresses, and the
     * context owns them until g_markup_parse_context_free(). */
    std::deque<std::string> intern;
    std::vector<const gchar *> stack;
    gint line = 1, col = 1;
};

GMarkupParseContext *g_markup_parse_context_new(const GMarkupParser *parser,
                                                GMarkupParseFlags,
                                                gpointer user_data,
                                                GDestroyNotify)
{
    GMarkupParseContext *c = new GMarkupParseContext;
    c->parser = parser;
    c->user_data = user_data;
    return c;
}

void g_markup_parse_context_free(GMarkupParseContext *ctx) { delete ctx; }

const gchar *g_markup_parse_context_get_element(GMarkupParseContext *ctx)
{
    if (!ctx || ctx->stack.empty()) return "";
    return ctx->stack.back();
}

void g_markup_parse_context_get_position(GMarkupParseContext *ctx,
                                         gint *line, gint *col)
{
    if (line) *line = ctx ? ctx->line : 0;
    if (col)  *col  = ctx ? ctx->col  : 0;
}

/* Dispatch a text chunk, honouring an error raised by a previous callback. */
static bool markup_text(GMarkupParseContext *ctx, const std::string &raw,
                        GError **error)
{
    if (!ctx->parser->text) return true;
    std::string s = xml_unescape(raw);
    ctx->parser->text(ctx, s.c_str(), (gsize)s.size(), ctx->user_data, error);
    return !(error && *error);
}

static bool markup_start(GMarkupParseContext *ctx, const std::string &name,
                         const std::vector<std::string> &an,
                         const std::vector<std::string> &av,
                         GError **error)
{
    std::vector<const gchar *> names, values;
    for (const auto &s : an) names.push_back(s.c_str());
    for (const auto &s : av) values.push_back(s.c_str());
    names.push_back(nullptr);
    values.push_back(nullptr);
    ctx->intern.push_back(name);
    const gchar *stable = ctx->intern.back().c_str();
    ctx->stack.push_back(stable);
    if (ctx->parser->start_element)
        ctx->parser->start_element(ctx, stable, names.data(), values.data(),
                                   ctx->user_data, error);
    return !(error && *error);
}

static bool markup_end(GMarkupParseContext *ctx, const std::string &name,
                       GError **error)
{
    const gchar *nm = ctx->stack.empty() ? name.c_str() : ctx->stack.back();
    if (ctx->parser->end_element)
        ctx->parser->end_element(ctx, nm, ctx->user_data, error);
    if (!ctx->stack.empty()) ctx->stack.pop_back();
    return !(error && *error);
}

/* Parse one tag's attribute list (the text between the tag name and '>' or "/>"). */
static void parse_attributes(const std::string &s,
                             std::vector<std::string> &names,
                             std::vector<std::string> &values)
{
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && isspace((unsigned char)s[i])) ++i;
        if (i >= s.size()) break;
        size_t n0 = i;
        while (i < s.size() && !isspace((unsigned char)s[i]) &&
               s[i] != '=' && s[i] != '/' && s[i] != '>') ++i;
        std::string key = s.substr(n0, i - n0);
        while (i < s.size() && isspace((unsigned char)s[i])) ++i;
        std::string val;
        if (i < s.size() && s[i] == '=') {
            ++i;
            while (i < s.size() && isspace((unsigned char)s[i])) ++i;
            if (i < s.size() && (s[i] == '"' || s[i] == '\'')) {
                char q = s[i++];
                size_t v0 = i;
                while (i < s.size() && s[i] != q) ++i;
                val = s.substr(v0, i - v0);
                if (i < s.size()) ++i;
            } else {
                size_t v0 = i;
                while (i < s.size() && !isspace((unsigned char)s[i])) ++i;
                val = s.substr(v0, i - v0);
            }
        }
        if (!key.empty()) {
            names.push_back(key);
            values.push_back(xml_unescape(val));
        }
    }
}

gboolean g_markup_parse_context_parse(GMarkupParseContext *ctx,
                                      const gchar *text, gssize text_len,
                                      GError **error)
{
    if (!ctx) return FALSE;
    size_t n = (text_len < 0) ? strlen(text) : (size_t)text_len;
    std::string src(text, n);
    clear_error(error);

    size_t i = 0;
    std::string chunk;
    auto flush = [&]() -> bool {
        if (chunk.empty()) return true;
        bool ok = markup_text(ctx, chunk, error);
        chunk.clear();
        return ok;
    };

    /* UTF-8 BOM */
    if (src.size() >= 3 && (unsigned char)src[0] == 0xEF &&
        (unsigned char)src[1] == 0xBB && (unsigned char)src[2] == 0xBF) i = 3;

    while (i < src.size()) {
        if (src[i] != '<') {
            size_t j = src.find('<', i);
            if (j == std::string::npos) j = src.size();
            chunk.append(src, i, j - i);
            for (size_t k = i; k < j; ++k) {
                if (src[k] == '\n') { ++ctx->line; ctx->col = 1; } else ++ctx->col;
            }
            i = j;
            continue;
        }
        if (!flush()) return FALSE;

        /* XML declaration / comments / DOCTYPE / processing instructions */
        if (i + 3 < src.size() && src.compare(i, 4, "<!--") == 0) {
            size_t e = src.find("-->", i + 4);
            if (e == std::string::npos) {
                g_set_error(error, G_MARKUP_ERROR, G_MARKUP_ERROR_INVALID_CONTENT,
                            "unterminated comment"); return FALSE;
            }
            i = e + 3;
            continue;
        }
        if (i + 8 < src.size() && src.compare(i, 9, "<!DOCTYPE") == 0) {
            size_t e = i + 9;
            int brackets = 0;
            for (; e < src.size(); ++e) {
                if (src[e] == '[') ++brackets;
                else if (src[e] == ']') --brackets;
                else if (src[e] == '>' && brackets == 0) break;
            }
            i = (e < src.size()) ? e + 1 : src.size();
            continue;
        }
        if (i + 1 < src.size() && (src[i + 1] == '?' || src[i + 1] == '!')) {
            size_t e = src.find('>', i + 2);
            i = (e == std::string::npos) ? src.size() : e + 1;
            continue;
        }

        /* Find the end of the tag, respecting quotes. */
        size_t e = i + 1;
        char quote = 0;
        for (; e < src.size(); ++e) {
            char c = src[e];
            if (quote) { if (c == quote) quote = 0; }
            else if (c == '"' || c == '\'') quote = c;
            else if (c == '>') break;
        }
        if (e >= src.size()) {
            g_set_error(error, G_MARKUP_ERROR, G_MARKUP_ERROR_INVALID_CONTENT,
                        "unterminated tag"); return FALSE;
        }
        std::string body = src.substr(i + 1, e - i - 1);
        i = e + 1;
        for (char c : body) { if (c == '\n') { ++ctx->line; ctx->col = 1; } else ++ctx->col; }

        bool closing = !body.empty() && body[0] == '/';
        if (closing) body.erase(0, 1);
        while (!body.empty() && isspace((unsigned char)body[0])) body.erase(0, 1);
        bool self_closing = !body.empty() && body.back() == '/';
        if (self_closing) body.pop_back();
        while (!body.empty() && isspace((unsigned char)body.back())) body.pop_back();

        size_t ns = 0;
        while (ns < body.size() && !isspace((unsigned char)body[ns])) ++ns;
        std::string name = body.substr(0, ns);
        if (name.empty()) continue;

        if (closing) {
            if (!markup_end(ctx, name, error)) return FALSE;
        } else {
            std::vector<std::string> an, av;
            parse_attributes(body.substr(ns), an, av);
            if (!markup_start(ctx, name, an, av, error)) return FALSE;
            if (self_closing && !markup_end(ctx, name, error)) return FALSE;
        }
    }
    if (!flush()) return FALSE;
    if (!ctx->stack.empty()) {
        g_set_error(error, G_MARKUP_ERROR, G_MARKUP_ERROR_INVALID_CONTENT,
                    "unclosed element <%s>", ctx->stack.back());
        return FALSE;
    }
    return TRUE;
}

/* ----------------------------------------------------------- GDir / files */
struct GDir {
    HANDLE handle = INVALID_HANDLE_VALUE;
    WIN32_FIND_DATAW fd{};
    bool first = true;
    std::string current;
};

GDir *g_dir_open(const gchar *path, guint, GError **error)
{
    std::wstring w = Utf8ToWide(path);
    if (w.empty()) return nullptr;
    if (!w.empty() && (w.back() == L'\\' || w.back() == L'/')) w.pop_back();
    w += L"\\*";
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(w.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        if (error) g_set_error(error, G_MARKUP_ERROR, G_MARKUP_ERROR_INVALID_CONTENT,
                               "cannot open directory %s", path);
        return nullptr;
    }
    GDir *d = new GDir;
    d->handle = h;
    d->fd = fd;
    d->first = true;
    return d;
}

const gchar *g_dir_read_name(GDir *dir)
{
    if (!dir || dir->handle == INVALID_HANDLE_VALUE) return nullptr;
    while (true) {
        if (!dir->first) {
            if (!FindNextFileW(dir->handle, &dir->fd)) return nullptr;
        }
        dir->first = false;
        std::string name = WideToUtf8(dir->fd.cFileName);
        if (name == "." || name == "..") continue;
        dir->current = name;
        return dir->current.c_str();
    }
}

void g_dir_close(GDir *dir)
{
    if (!dir) return;
    if (dir->handle != INVALID_HANDLE_VALUE) FindClose(dir->handle);
    delete dir;
}

/* -------------------------------------------------------- GPatternSpec */
struct GPatternSpec { std::string pattern; };

GPatternSpec *g_pattern_spec_new(const gchar *pattern)
{
    GPatternSpec *p = new GPatternSpec;
    p->pattern = pattern ? pattern : "";
    return p;
}
void g_pattern_spec_free(GPatternSpec *ps) { delete ps; }

/* Simple '*' / '?' glob (lensfun only ever uses "*.xml"). */
gboolean g_pattern_match(GPatternSpec *ps, guint string_length,
                         const gchar *string, const gchar *)
{
    if (!ps || !string) return FALSE;
    if (string_length == 0) string_length = (guint)strlen(string);
    std::string s(string, string_length);
    const std::string &p = ps->pattern;
    size_t si = 0, pi = 0, star = std::string::npos, star_si = 0;
    while (si < s.size()) {
        if (pi < p.size() && (p[pi] == '?' || p[pi] == s[si])) { ++si; ++pi; }
        else if (pi < p.size() && p[pi] == '*') { star = pi++; star_si = si; }
        else if (star != std::string::npos) { pi = star + 1; si = ++star_si; }
        else return FALSE;
    }
    while (pi < p.size() && p[pi] == '*') ++pi;
    return pi == p.size();
}

/* ------------------------------------------------------------ paths / IO */
gchar *g_build_filename(const gchar *first, ...)
{
    std::string out = first ? first : "";
    va_list ap;
    va_start(ap, first);
    while (const gchar *part = va_arg(ap, const gchar *)) {
        if (!*part) continue;
        if (!out.empty() && out.back() != '\\' && out.back() != '/') out += '\\';
        out += part;
    }
    va_end(ap);
    gchar *r = (gchar *)g_malloc(out.size() + 1);
    memcpy(r, out.c_str(), out.size() + 1);
    return r;
}

const gchar *g_get_user_data_dir(void)
{
    static std::string dir = []() -> std::string {
        wchar_t buf[1024];
        DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, 1024);
        if (n > 0 && n < 1024) return WideToUtf8(buf);
        return std::string(".");
    }();
    return dir.c_str();
}

gboolean g_file_get_contents(const gchar *filename, gchar **contents,
                             gsize *length, GError **error)
{
    if (contents) *contents = nullptr;
    if (length) *length = 0;
    std::wstring w = Utf8ToWide(filename);
    HANDLE h = CreateFileW(w.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        g_set_error(error, G_MARKUP_ERROR, G_MARKUP_ERROR_INVALID_CONTENT,
                    "cannot open %s", filename ? filename : "(null)");
        return FALSE;
    }
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    if (sz.QuadPart < 0 || sz.QuadPart > (1LL << 31)) { CloseHandle(h); return FALSE; }
    gsize n = (gsize)sz.QuadPart;
    gchar *buf = (gchar *)g_malloc(n + 1);
    DWORD got = 0;
    BOOL ok = ReadFile(h, buf, (DWORD)n, &got, nullptr);
    CloseHandle(h);
    if (!ok) { g_free(buf); return FALSE; }
    buf[got] = 0;
    if (contents) *contents = buf; else g_free(buf);
    if (length) *length = got;
    return TRUE;
}

gboolean g_file_test(const gchar *filename, gint test)
{
    std::wstring w = Utf8ToWide(filename);
    DWORD a = GetFileAttributesW(w.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return FALSE;
    if (test & G_FILE_TEST_IS_DIR) return (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
    return TRUE;
}

int g_mkdir(const gchar *filename, int)
{
    std::wstring w = Utf8ToWide(filename);
    return CreateDirectoryW(w.c_str(), nullptr) ? 0 : -1;
}

int g_open(const gchar *filename, int flags, int mode)
{
    std::wstring w = Utf8ToWide(filename);
    int f = 0;
    if (flags & O_CREAT)  f |= _O_CREAT;
    if (flags & O_WRONLY) f |= _O_WRONLY;
    if (flags & O_TRUNC)  f |= _O_TRUNC;
    f |= _O_BINARY;
    return _wopen(w.c_str(), f, mode);
}

/* --------------------------------------------------------------- UTF-8 */
static size_t utf8_decode(const gchar *p, gunichar *out)
{
    const unsigned char *s = (const unsigned char *)p;
    if (s[0] < 0x80) { *out = s[0]; return 1; }
    if ((s[0] & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
        *out = ((gunichar)(s[0] & 0x1F) << 6) | (s[1] & 0x3F); return 2;
    }
    if ((s[0] & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        *out = ((gunichar)(s[0] & 0x0F) << 12) | ((gunichar)(s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        return 3;
    }
    if ((s[0] & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 &&
        (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
        *out = ((gunichar)(s[0] & 0x07) << 18) | ((gunichar)(s[1] & 0x3F) << 12) |
               ((gunichar)(s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        return 4;
    }
    *out = s[0];
    return 1;
}

static void utf8_encode(gunichar c, std::string &out)
{
    if (c < 0x80) out += (char)c;
    else if (c < 0x800) {
        out += (char)(0xC0 | (c >> 6));
        out += (char)(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += (char)(0xE0 | (c >> 12));
        out += (char)(0x80 | ((c >> 6) & 0x3F));
        out += (char)(0x80 | (c & 0x3F));
    } else {
        out += (char)(0xF0 | (c >> 18));
        out += (char)(0x80 | ((c >> 12) & 0x3F));
        out += (char)(0x80 | ((c >> 6) & 0x3F));
        out += (char)(0x80 | (c & 0x3F));
    }
}

gunichar g_utf8_get_char(const gchar *p)
{
    gunichar c = 0;
    if (!p || !*p) return 0;
    utf8_decode(p, &c);
    return c;
}

gchar *g_utf8_next_char(const gchar *p)
{
    if (!p || !*p) return (gchar *)p;
    gunichar c;
    return (gchar *)(p + utf8_decode(p, &c));
}

gboolean g_unichar_isspace(gunichar c)
{
    if (c == ' ' || (c >= 0x09 && c <= 0x0D)) return TRUE;
    if (c == 0x85 || c == 0xA0 || c == 0x1680 || c == 0x2028 || c == 0x2029 ||
        c == 0x202F || c == 0x205F || c == 0x3000 ||
        (c >= 0x2000 && c <= 0x200A)) return TRUE;
    return FALSE;
}

gunichar g_unichar_tolower(gunichar c)
{
    if (c < 128) return (gunichar)tolower((int)c);
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 0x20;
    if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) return c + 0x20;
    if (c >= 0x410 && c <= 0x42F) return c + 0x20;
    return c;
}

gchar *g_utf8_casefold(const gchar *str, gssize len)
{
    if (!str) return g_strdup("");
    size_t n = (len < 0) ? strlen(str) : (size_t)len;
    std::string out;
    out.reserve(n);
    for (size_t i = 0; i < n; ) {
        gunichar c = 0;
        size_t step = utf8_decode(str + i, &c);
        i += step;
        utf8_encode(g_unichar_tolower(c), out);
    }
    gchar *r = (gchar *)g_malloc(out.size() + 1);
    memcpy(r, out.c_str(), out.size() + 1);
    return r;
}

/* --------------------------------------------------------------- mutex */
static std::mutex &shim_mutex()
{
    static std::mutex m;
    return m;
}
void g_mutex_lock(GMutex *)             { shim_mutex().lock(); }
void g_mutex_unlock(GMutex *)           { shim_mutex().unlock(); }
void g_static_mutex_lock(GStaticMutex *){ shim_mutex().lock(); }
void g_static_mutex_unlock(GStaticMutex *) { shim_mutex().unlock(); }

} /* extern "C" */

/* Replacement for libs/lensfun/windows/auxfun.cpp (excluded from this build).
 * That file asks for the directory of "liblensfun.dll" via GetModuleHandle,
 * which cannot work when Lensfun is linked statically. The host always calls
 * lfDatabase::LoadDirectory() with an explicit path, so an empty fallback is
 * correct; LENSFUN_DB_DIR is honoured for convenience/debugging.
 * Declared as a C++-linkage function to match database.cpp's declaration. */
gchar *_lf_get_database_dir()
{
    char buf[1024];
    DWORD n = GetEnvironmentVariableA("LENSFUN_DB_DIR", buf, sizeof(buf));
    if (n > 0 && n < sizeof(buf)) return g_strdup(buf);
    return g_strdup("");
}
