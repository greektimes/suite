/*
 * suite_update.c - Help > Update. See suite_update.h for the manifest
 * schema, the URLs and the security model.
 *
 * Part of the Montreal Greek Times Unicorn Suite.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * SHAPE. Two worker threads, one after the other, both posting back to
 * the main window. Nothing here runs on the UI thread except the
 * message boxes, which is the same discipline every other network-using
 * module in the Suite follows.
 *
 *   Help > Update
 *     -> check thread: GET the manifest, parse it, compare versions
 *     -> WM_APP_UPDATE_RESULT on the UI thread: say up to date, or
 *        offer the new version with its notes
 *     -> if the user accepts:
 *          download thread: stream the MSI to
 *            %LOCALAPPDATA%\MGT Unicorn Suite\Update\, hash it with
 *            CNG, compare against the manifest
 *     -> WM_APP_UPDATE_DLDONE on the UI thread: refuse on a mismatch,
 *        otherwise hand over to the installer
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>

#include "suite_update.h"
#include "suite_shell.h"
#include "suite_version.h"
#include "http_fetch.h"
#include "suite_sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* The contract                                                        */
/* ------------------------------------------------------------------ */

/* THE ENDPOINT. A HOSTNAME, NEVER AN ADDRESS.
 *
 * apps.greektimes.ca is the update host and the only thing about the
 * update system that is compiled in. It resolves to whichever machine
 * currently serves the apps docroot, so the VPS can move, be rebuilt or
 * be replaced and every installed copy keeps updating. Putting an IP
 * here, or a path on some other host that happened to be convenient
 * during testing, would silently strand every user the day that machine
 * changed.
 *
 * One directory per application under the docroot, so the standalone
 * apps that follow get /wais/manifest.json and so on beside this one. */
#define UPDATE_MANIFEST_URL   "https://apps.greektimes.ca/unicorn-suite/manifest.json"

/* An installer may only be fetched from the update host itself, matched
 * EXACTLY and not as a suffix. The hash check below is the real defence,
 * but a manifest that has been tampered with should not even be able to
 * name another machine. */
#define UPDATE_ALLOWED_HOST   "apps.greektimes.ca"

/* A manifest is a few hundred bytes; anything remotely near this is
 * not a manifest. */
#define UPDATE_MANIFEST_MAX   (256 * 1024)

/* Ceiling on the installer download. Sanity bound, not a tight fit: the
 * 0.4.0 MSI was under 5 MB and 0.5.0 is about 16 MB, because 0.5.0
 * carries the NABU emulator package. Keep this comfortably above the
 * real size, since a manifest naming a larger installer than this would
 * be refused and the update would simply never install. */
#define UPDATE_MSI_MAX        (256ULL * 1024ULL * 1024ULL)

#define UPDATE_REG_KEY  "Software\\The Montreal Greek Times\\MGT Unicorn Suite"

/* Settings, all under UPDATE_REG_KEY in HKCU. HKCU and not HKLM because
 * the install is per-machine and a standard user cannot write there;
 * HKLM holds the installed version, written once by the MSI, and is
 * read-only to us. */
#define SET_STARTUP_ENABLED  "startup_check_enabled"   /* REG_DWORD, default 1 */
#define SET_LAST_CHECKED     "last_checked"            /* REG_SZ, ISO-8601 UTC */
#define SET_SKIPPED_VERSION  "skipped_version"         /* REG_SZ               */

/* How often the startup check is allowed to reach the network. A user
 * who launches the Suite six times in an afternoon should cost the
 * update host one request, not six. */
#define UPDATE_THROTTLE_HOURS  24

/* The two ways a check can begin. They differ only in what happens when
 * there is nothing to say: a manual check owes the user an answer, a
 * startup check owes them silence. */
#define UPD_MODE_MANUAL   0
#define UPD_MODE_STARTUP  1

/* Control ids for the update dialog. */
#define IDC_UPD_NOTES      2101
#define IDC_UPD_STARTUP    2102
#define IDC_UPD_HEADING    2103
#define IDC_UPD_VERSIONS   2104
#define IDD_UPD_SKIP       2105

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

typedef struct UpdManifest {
    char version[64];
    char msi_url[512];
    char msi_sha256[SUITE_SHA256_HEXLEN];
    unsigned long long msi_bytes;
    char notes[4096];          /* multi-line release notes for the dialog */
    char min_supported[64];
} UpdManifest;

typedef struct UpdResult {
    int  ok;                    /* the manifest was fetched and parsed  */
    int  cmp;                   /* <0 app older, 0 same, >0 app newer   */
    int  mode;                  /* UPD_MODE_MANUAL or UPD_MODE_STARTUP  */
    int  required;              /* running version is below min_supported */
    int  placeholder;           /* the "no release published" manifest  */
    char error[256];
    UpdManifest m;
} UpdResult;

typedef struct UpdDownload {
    int     ok;
    char    error[256];
    wchar_t path[MAX_PATH];
    char    got_hash[SUITE_SHA256_HEXLEN];
    UpdManifest m;
} UpdDownload;

static HWND    g_main      = NULL;
static HANDLE  g_thread    = NULL;
static volatile LONG g_busy = 0;
static UpdManifest g_pending;     /* the offer the user is answering    */
/* Which kind of check the running worker belongs to. Written on the UI
 * thread before the worker starts and read by it; g_busy makes that
 * ordering safe because only one check is ever in flight. */
static int g_check_mode = UPD_MODE_MANUAL;

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static void upd_status(const char *text)
{
    char *copy;
    if (!g_main || !text) return;
    copy = (char *)malloc(strlen(text) + 1);
    if (!copy) return;
    strcpy(copy, text);
    if (!PostMessageA(g_main, WM_APP_UPDATE_STATUS, 0, (LPARAM)copy))
        free(copy);
}

static int url_host_allowed(const char *url);

static void copy_str(char *dst, size_t cap, const char *src)
{
    size_t n;
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    n = strlen(src);
    if (n > cap - 1) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* The manifest URL, normally the compiled-in one.
 *
 * MGT_UPDATE_MANIFEST_URL overrides it, which is how the release path
 * gets tested against a staging copy without a special build. Same
 * pattern as MGT_RIP_CAPTURE and MGT_ZMODEM_DIR elsewhere in the Suite.
 *
 * The override is NOT a hole: it goes through url_host_allowed() like
 * the download URL does, so it can only ever point at https on
 * greektimes.ca. The most an environment variable can do is move the
 * check to a different path on the project's own domain, and anyone who
 * can set your environment can already run code as you. */
const char *suite_update_manifest_url(void)
{
    static char cached[512];
    const char *env;

    if (cached[0]) return cached;
    env = getenv("MGT_UPDATE_MANIFEST_URL");
    if (env && env[0] && strlen(env) < sizeof cached && url_host_allowed(env))
        copy_str(cached, sizeof cached, env);
    else
        copy_str(cached, sizeof cached, UPDATE_MANIFEST_URL);
    return cached;
}

/* ------------------------------------------------------------------ */
/* Semantic version comparison                                         */
/* ------------------------------------------------------------------ */

/* Read the numeric triple and leave *rest pointing at the pre-release
 * suffix (past the '-'), or at "" when there is none. A leading 'v' is
 * tolerated because the Suite has always displayed one. Build metadata
 * after '+' is ignored, as semver requires. */
static void ver_split(const char *s, unsigned n[3], const char **rest)
{
    int i;
    n[0] = n[1] = n[2] = 0;
    *rest = "";
    if (!s) return;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == 'v' || *s == 'V') s++;

    for (i = 0; i < 3; i++) {
        unsigned v = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9' && digits < 9) {
            v = v * 10u + (unsigned)(*s - '0');
            s++; digits++;
        }
        n[i] = v;
        if (*s == '.') { s++; continue; }
        break;
    }
    /* Skip any further dotted numbers we do not model. */
    while (*s == '.' || (*s >= '0' && *s <= '9')) s++;
    if (*s == '-') *rest = s + 1;
}

/* Is this whole identifier decimal digits? */
static int ident_is_num(const char *p, size_t len)
{
    size_t i;
    if (len == 0) return 0;
    for (i = 0; i < len; i++)
        if (p[i] < '0' || p[i] > '9') return 0;
    return 1;
}

/* Compare two pre-release strings per semver: dot-separated
 * identifiers, numeric ones compare numerically and rank below
 * alphanumeric ones, and a shorter run of otherwise-equal identifiers
 * ranks lower. */
static int prerelease_cmp(const char *a, const char *b)
{
    for (;;) {
        const char *ae, *be;
        size_t alen, blen;
        int anum, bnum;

        if (*a == '\0' && *b == '\0') return 0;
        if (*a == '\0') return -1;
        if (*b == '\0') return 1;

        ae = strchr(a, '.'); if (!ae) ae = a + strlen(a);
        be = strchr(b, '.'); if (!be) be = b + strlen(b);
        alen = (size_t)(ae - a);
        blen = (size_t)(be - b);

        anum = ident_is_num(a, alen);
        bnum = ident_is_num(b, blen);

        if (anum && bnum) {
            unsigned long av = strtoul(a, NULL, 10);
            unsigned long bv = strtoul(b, NULL, 10);
            if (av != bv) return (av < bv) ? -1 : 1;
        } else if (anum != bnum) {
            return anum ? -1 : 1;       /* numeric ranks lower */
        } else {
            size_t n = (alen < blen) ? alen : blen;
            int c = strncmp(a, b, n);
            if (c != 0) return (c < 0) ? -1 : 1;
            if (alen != blen) return (alen < blen) ? -1 : 1;
        }

        a = (*ae == '.') ? ae + 1 : ae;
        b = (*be == '.') ? be + 1 : be;
    }
}

int suite_update_compare_versions(const char *a, const char *b)
{
    unsigned na[3], nb[3];
    const char *pa, *pb;
    int i;

    ver_split(a, na, &pa);
    ver_split(b, nb, &pb);

    for (i = 0; i < 3; i++)
        if (na[i] != nb[i]) return (na[i] < nb[i]) ? -1 : 1;

    /* Same triple. A pre-release is OLDER than the release. */
    if (*pa == '\0' && *pb == '\0') return 0;
    if (*pa == '\0') return 1;          /* a is the release, b a pre  */
    if (*pb == '\0') return -1;         /* a is a pre, b the release  */
    return prerelease_cmp(pa, pb);
}

/* ------------------------------------------------------------------ */
/* A very small JSON reader                                            */
/* ------------------------------------------------------------------ */

/* The manifest is a flat object of five known keys. This looks for
 * "key" at the top level and returns the character after its colon, or
 * NULL. It is deliberately not a general JSON parser: it does not need
 * to be, and a small readable scanner is easier to be sure of than a
 * general one. Strings inside the document cannot fool it into matching
 * a key because the match requires the quote, the exact name, the
 * closing quote and then a colon. */
static const char *json_value_for(const char *doc, const char *key)
{
    size_t klen = strlen(key);
    const char *p = doc;

    while ((p = strchr(p, '"')) != NULL) {
        if (strncmp(p + 1, key, klen) == 0 && p[1 + klen] == '"') {
            const char *q = p + 2 + klen;
            while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
            if (*q == ':') {
                q++;
                while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
                return q;
            }
        }
        p++;
    }
    return NULL;
}

/* Copy a JSON string value into out, decoding the escapes that matter
 * for this manifest. Returns 0 on success. Always terminates. */
static int json_get_string(const char *doc, const char *key,
                           char *out, size_t cap)
{
    const char *p = json_value_for(doc, key);
    size_t o = 0;

    if (!out || cap == 0) return 1;
    out[0] = '\0';
    if (!p || *p != '"') return 1;
    p++;

    while (*p && *p != '"' && o + 1 < cap) {
        if (*p == '\\') {
            p++;
            switch (*p) {
            case 'n':  out[o++] = '\n'; p++; break;
            case 'r':  out[o++] = '\r'; p++; break;
            case 't':  out[o++] = '\t'; p++; break;
            case '"':  out[o++] = '"';  p++; break;
            case '\\': out[o++] = '\\'; p++; break;
            case '/':  out[o++] = '/';  p++; break;
            case 'u': {
                /* Only the ASCII range is rendered; anything else
                 * becomes a question mark. The manifest's notes are
                 * meant to be plain text. */
                int i, v = 0;
                p++;
                for (i = 0; i < 4 && p[i]; i++) {
                    char c = p[i];
                    int d = (c >= '0' && c <= '9') ? c - '0'
                          : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                          : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
                    if (d < 0) break;
                    v = v * 16 + d;
                }
                p += i;
                out[o++] = (v >= 0x20 && v < 0x7f) ? (char)v : '?';
                break;
            }
            default:
                if (*p) { out[o++] = *p; p++; }
                break;
            }
        } else {
            out[o++] = *p++;
        }
    }
    out[o] = '\0';
    return (*p == '"') ? 0 : 1;
}

static unsigned long long json_get_uint(const char *doc, const char *key)
{
    const char *p = json_value_for(doc, key);
    unsigned long long v = 0;
    int digits = 0;
    if (!p) return 0;
    while (*p >= '0' && *p <= '9' && digits < 19) {
        v = v * 10ULL + (unsigned long long)(*p - '0');
        p++; digits++;
    }
    return v;
}

/* ------------------------------------------------------------------ */
/* Validation                                                          */
/* ------------------------------------------------------------------ */

static int is_hex64(const char *s)
{
    int i;
    if (!s) return 0;
    for (i = 0; i < 64; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F')))
            return 0;
    }
    return s[64] == '\0';
}

/* The URL must be https and its host must be greektimes.ca or something
 * under it. Parsed here rather than trusting a substring search: a URL
 * like https://greektimes.ca.evil.example/x contains the domain but is
 * not on it. */
static int url_host_allowed(const char *url)
{
    const char *h, *e;
    size_t hlen, dlen = strlen(UPDATE_ALLOWED_HOST);

    if (!url) return 0;
    if (_strnicmp(url, "https://", 8) != 0) return 0;
    h = url + 8;
    /* Reject embedded credentials outright rather than parse them. */
    for (e = h; *e && *e != '/'; e++)
        if (*e == '@') return 0;
    e = h;
    while (*e && *e != '/' && *e != ':') e++;
    hlen = (size_t)(e - h);
    if (hlen == 0) return 0;

    if (hlen == dlen)
        return _strnicmp(h, UPDATE_ALLOWED_HOST, dlen) == 0;
    if (hlen > dlen)
        return h[hlen - dlen - 1] == '.' &&
               _strnicmp(h + hlen - dlen, UPDATE_ALLOWED_HOST, dlen) == 0;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Settings, HKCU. See the header for why not HKLM.                    */
/* ------------------------------------------------------------------ */

static void settings_write(const char *name, const char *value)
{
    HKEY k;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, UPDATE_REG_KEY, 0, NULL,
                        REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, NULL,
                        &k, NULL) != ERROR_SUCCESS)
        return;
    RegSetValueExA(k, name, 0, REG_SZ, (const BYTE *)value,
                   (DWORD)(strlen(value) + 1));
    RegCloseKey(k);
}

static int settings_read(const char *name, char *out, size_t cap)
{
    HKEY  k;
    DWORD type = 0, sz = (DWORD)cap;
    LONG  rc;

    if (!out || cap == 0) return 0;
    out[0] = 0;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, UPDATE_REG_KEY, 0, KEY_QUERY_VALUE,
                      &k) != ERROR_SUCCESS)
        return 0;
    rc = RegQueryValueExA(k, name, NULL, &type, (BYTE *)out, &sz);
    RegCloseKey(k);
    if (rc != ERROR_SUCCESS || type != REG_SZ) { out[0] = 0; return 0; }
    out[cap - 1] = 0;
    return out[0] != 0;
}

static void settings_write_dword(const char *name, DWORD value)
{
    HKEY k;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, UPDATE_REG_KEY, 0, NULL,
                        REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, NULL,
                        &k, NULL) != ERROR_SUCCESS)
        return;
    RegSetValueExA(k, name, 0, REG_DWORD, (const BYTE *)&value, sizeof value);
    RegCloseKey(k);
}

/* Absent means the default, which for the startup check is ON. A fresh
 * profile therefore gets update checks without anyone opting in, and
 * turning them off is a decision the user made and we recorded. */
static int settings_read_dword(const char *name, int dflt)
{
    HKEY  k;
    DWORD type = 0, val = 0, sz = sizeof val;
    LONG  rc;

    if (RegOpenKeyExA(HKEY_CURRENT_USER, UPDATE_REG_KEY, 0, KEY_QUERY_VALUE,
                      &k) != ERROR_SUCCESS)
        return dflt;
    rc = RegQueryValueExA(k, name, NULL, &type, (BYTE *)&val, &sz);
    RegCloseKey(k);
    if (rc != ERROR_SUCCESS || type != REG_DWORD) return dflt;
    return (int)val;
}

int suite_update_startup_enabled(void)
{
    return settings_read_dword(SET_STARTUP_ENABLED, 1) != 0;
}

void suite_update_set_startup_enabled(int on)
{
    settings_write_dword(SET_STARTUP_ENABLED, on ? 1 : 0);
}

/* "YYYY-MM-DDTHH:MM:SSZ", so the value is readable in regedit and sorts
 * correctly as text. */
static void settings_stamp_check(void)
{
    SYSTEMTIME st;
    char buf[32];
    GetSystemTime(&st);
    _snprintf(buf, sizeof buf - 1, "%04d-%02d-%02dT%02d:%02d:%02dZ",
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    buf[sizeof buf - 1] = 0;
    settings_write(SET_LAST_CHECKED, buf);
}

/* Hours since the recorded check, or a very large number if there has
 * never been one or the value will not parse. Comparing as FILETIME
 * rather than as text is what makes the window a real 24 hours instead
 * of a calendar-day boundary. */
static double hours_since_last_check(void)
{
    char       buf[32];
    SYSTEMTIME st;
    FILETIME   then, now;
    ULARGE_INTEGER a, b;
    int y, mo, d, h, mi, sec;

    if (!settings_read(SET_LAST_CHECKED, buf, sizeof buf)) return 1e9;
    if (sscanf(buf, "%d-%d-%dT%d:%d:%dZ", &y, &mo, &d, &h, &mi, &sec) != 6)
        return 1e9;

    memset(&st, 0, sizeof st);
    st.wYear = (WORD)y; st.wMonth = (WORD)mo; st.wDay = (WORD)d;
    st.wHour = (WORD)h; st.wMinute = (WORD)mi; st.wSecond = (WORD)sec;
    if (!SystemTimeToFileTime(&st, &then)) return 1e9;
    GetSystemTimeAsFileTime(&now);

    a.LowPart = then.dwLowDateTime;  a.HighPart = then.dwHighDateTime;
    b.LowPart = now.dwLowDateTime;   b.HighPart = now.dwHighDateTime;
    if (b.QuadPart <= a.QuadPart) return 0.0;      /* clock moved back  */
    return (double)(b.QuadPart - a.QuadPart) / 1e7 / 3600.0;
}

/* ------------------------------------------------------------------ */
/* Where the download lands                                            */
/* ------------------------------------------------------------------ */

/* %LOCALAPPDATA%\MGT Unicorn Suite\Update. Deliberately NOT the install
 * directory: the installer is about to rewrite that, and a running
 * process must not have its working set underneath what msiexec is
 * replacing. */
static int update_dir(wchar_t *out, size_t cap)
{
    wchar_t base[MAX_PATH];
    if (!out || cap == 0) return 1;
    out[0] = L'\0';
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH) == 0) {
        if (GetTempPathW(MAX_PATH, base) == 0) return 1;
    }
    _snwprintf(out, cap - 1, L"%s\\MGT Unicorn Suite", base);
    out[cap - 1] = L'\0';
    CreateDirectoryW(out, NULL);
    _snwprintf(out, cap - 1, L"%s\\MGT Unicorn Suite\\Update", base);
    out[cap - 1] = L'\0';
    CreateDirectoryW(out, NULL);
    return 0;
}

/* The last path element of a URL, sanitized to a bare file name. The
 * sender does not get to choose a path, only a leaf, and only from a
 * small character set. Anything unusable becomes a fixed name. */
static void msi_local_name(const char *url, wchar_t *out, size_t cap)
{
    const char *slash = strrchr(url, '/');
    const char *name = slash ? slash + 1 : url;
    char clean[128];
    size_t i, o = 0;

    for (i = 0; name[i] && o + 1 < sizeof clean; i++) {
        char c = name[i];
        if (c == '?' || c == '#') break;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_')
            clean[o++] = c;
    }
    clean[o] = '\0';
    if (o < 5 || _stricmp(clean + (o > 4 ? o - 4 : 0), ".msi") != 0)
        strcpy(clean, "MGT_Unicorn_Suite_update.msi");

    MultiByteToWideChar(CP_ACP, 0, clean, -1, out, (int)cap);
}

/* ------------------------------------------------------------------ */
/* Worker: fetch and parse the manifest                                */
/* ------------------------------------------------------------------ */

static DWORD WINAPI check_thread(LPVOID lp)
{
    UpdResult *r = (UpdResult *)calloc(1, sizeof *r);
    unsigned char *body = NULL;
    DWORD len = 0;
    char err[256];
    int rc;

    (void)lp;
    if (!r) { InterlockedExchange(&g_busy, 0); return 0; }

    upd_status("Checking for updates...");

    err[0] = '\0';
    {
        wchar_t wurl[512];
        MultiByteToWideChar(CP_ACP, 0, suite_update_manifest_url(), -1,
                            wurl, (int)(sizeof wurl / sizeof wurl[0]));
        rc = http_fetch_to_memory(wurl, &body, &len,
                                  UPDATE_MANIFEST_MAX, err, sizeof err);
    }
    if (rc != HTTP_FETCH_OK) {
        copy_str(r->error, sizeof r->error,
                 err[0] ? err : "The update service could not be reached.");
        goto post;
    }

    if (json_get_string((const char *)body, "version",
                        r->m.version, sizeof r->m.version) != 0
        || r->m.version[0] == '\0') {
        copy_str(r->error, sizeof r->error,
                 "The update information was not readable (no version).");
        goto post;
    }
    if (json_get_string((const char *)body, "msi_url",
                        r->m.msi_url, sizeof r->m.msi_url) != 0
        || r->m.msi_url[0] == '\0') {
        copy_str(r->error, sizeof r->error,
                 "The update information was not readable (no download).");
        goto post;
    }
    if (json_get_string((const char *)body, "msi_sha256",
                        r->m.msi_sha256, sizeof r->m.msi_sha256) != 0
        || !is_hex64(r->m.msi_sha256)) {
        copy_str(r->error, sizeof r->error,
                 "The update information has no usable checksum. Refusing.");
        goto post;
    }
    if (!url_host_allowed(r->m.msi_url)) {
        copy_str(r->error, sizeof r->error,
                 "The update points at a host other than "
                 UPDATE_ALLOWED_HOST ". Refusing.");
        goto post;
    }
    r->m.msi_bytes = json_get_uint((const char *)body, "msi_bytes");
    json_get_string((const char *)body, "notes",
                    r->m.notes, sizeof r->m.notes);
    json_get_string((const char *)body, "min_supported",
                    r->m.min_supported, sizeof r->m.min_supported);

    /* Negative means the running app is older, which is the only case
     * that offers a download. */
    r->cmp = suite_update_compare_versions(SUITE_VERSION_STR, r->m.version);

    /* min_supported: a floor below which this build can no longer be
     * carried forward. When the running version is under it the update
     * is not optional, so the dialog drops Skip. Absent or empty means
     * no floor. */
    r->required = (r->m.min_supported[0] &&
                   suite_update_compare_versions(SUITE_VERSION_STR,
                                                 r->m.min_supported) < 0);

    /* THE PLACEHOLDER. The endpoint answers with a well-formed manifest
     * carrying version 0.0.0-placeholder, a zeroed hash and zero bytes
     * until the operator publishes a real release. Version comparison
     * already makes 0.0.0 older than anything we ship, so this is belt
     * and braces: a manifest that names no bytes and no real hash is
     * never an update, whatever its version string says. */
    r->placeholder = (r->m.msi_bytes == 0) ||
                     (strspn(r->m.msi_sha256, "0") == 64);

    r->ok = 1;

post:
    r->mode = g_check_mode;
    if (body) HeapFree(GetProcessHeap(), 0, body);
    settings_stamp_check();
    if (!PostMessageA(g_main, WM_APP_UPDATE_RESULT, 0, (LPARAM)r)) {
        free(r);
        InterlockedExchange(&g_busy, 0);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Worker: download and verify                                         */
/* ------------------------------------------------------------------ */

static int dl_progress(void *ctx, unsigned long long got,
                       unsigned long long total)
{
    char line[160];
    (void)ctx;
    if (total > 0) {
        int pct = (int)((got * 100ULL) / total);
        if (pct > 100) pct = 100;
        _snprintf(line, sizeof line - 1,
                  "Downloading the update... %d%%  (%llu of %llu bytes)",
                  pct, got, total);
    } else {
        _snprintf(line, sizeof line - 1,
                  "Downloading the update... %llu bytes", got);
    }
    line[sizeof line - 1] = '\0';
    upd_status(line);
    return 0;
}

static DWORD WINAPI download_thread(LPVOID lp)
{
    UpdDownload *d = (UpdDownload *)lp;
    wchar_t dir[MAX_PATH], name[MAX_PATH], wurl[1024];
    char err[256];
    int rc;

    if (!d) { InterlockedExchange(&g_busy, 0); return 0; }

    if (update_dir(dir, MAX_PATH) != 0) {
        copy_str(d->error, sizeof d->error,
                 "There is nowhere to save the download.");
        goto post;
    }
    msi_local_name(d->m.msi_url, name, MAX_PATH);
    _snwprintf(d->path, MAX_PATH - 1, L"%s\\%s", dir, name);
    d->path[MAX_PATH - 1] = L'\0';

    MultiByteToWideChar(CP_ACP, 0, d->m.msi_url, -1, wurl,
                        (int)(sizeof wurl / sizeof wurl[0]));

    err[0] = '\0';
    rc = http_fetch_to_file(wurl, d->path, UPDATE_MSI_MAX,
                            dl_progress, NULL, err, sizeof err);
    if (rc != HTTP_FETCH_OK) {
        copy_str(d->error, sizeof d->error,
                 err[0] ? err : "The download failed.");
        d->path[0] = L'\0';        /* http_fetch already removed it */
        goto post;
    }

    upd_status("Checking the download...");
    err[0] = '\0';
    if (suite_sha256_file(d->path, d->got_hash, err, sizeof err) != 0) {
        copy_str(d->error, sizeof d->error,
                 err[0] ? err : "The download could not be checked.");
        DeleteFileW(d->path);
        d->path[0] = L'\0';
        goto post;
    }

    /* THE CHECK. Everything else in this file is plumbing; this line is
     * the security model. A file that does not hash to what the
     * manifest promised is deleted and never handed to the installer. */
    if (!suite_sha256_hex_equal(d->got_hash, d->m.msi_sha256)) {
        copy_str(d->error, sizeof d->error,
                 "The downloaded installer does not match its published "
                 "checksum. It has been deleted and will NOT be run.");
        DeleteFileW(d->path);
        d->path[0] = L'\0';
        goto post;
    }

    d->ok = 1;

post:
    if (!PostMessageA(g_main, WM_APP_UPDATE_DLDONE, 0, (LPARAM)d)) {
        free(d);
        InterlockedExchange(&g_busy, 0);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* UI thread                                                           */
/* ------------------------------------------------------------------ */


/* ------------------------------------------------------------------ */
/* The update dialog                                                   */
/* ------------------------------------------------------------------ */
/*
 * A real dialog rather than a MessageBox, because release notes need a
 * scrollable box and the choice needs three buttons, neither of which
 * MessageBox can do.
 *
 * The template is assembled in memory and handed to
 * DialogBoxIndirectParam, the same approach gopher_search_dialog.c
 * uses, so there is no .rc dialog resource to keep in step.
 *
 * TWO SHAPES, ONE TEMPLATE BUILDER:
 *
 *   startup check   Update Now | Remind Me Later | Skip This Version
 *   manual check    Update Now | Cancel
 *
 * Skip and Remind exist only on the startup path. A manual check is the
 * user asking the question, so answering it with "remind me later" is
 * noise, and skipping a version they went looking for makes no sense.
 *
 * The startup-check toggle lives here, as a checkbox on this dialog,
 * because this is the one moment the user is certain to be thinking
 * about update checks. It is shown on both paths, so the setting is
 * reachable from Help > Update without waiting for a release.
 */

typedef struct UpdDlgData {
    const UpdManifest *m;
    int   mode;
    int   required;        /* below min_supported: Skip is withheld */
} UpdDlgData;

static void *align_dword(unsigned char *pp)
{
    ULONG_PTR a = (ULONG_PTR)pp;
    a = (a + 3) & ~(ULONG_PTR)3;
    return (void *)a;
}

static unsigned char *emit_word(unsigned char *pp, WORD v)
{
    WORD *w = (WORD *)pp;
    *w = v;
    return pp + sizeof(WORD);
}

static unsigned char *emit_utf16(unsigned char *pp, const wchar_t *text)
{
    if (!text || !*text) return emit_word(pp, 0);
    {
        size_t n = wcslen(text) + 1;
        memcpy(pp, text, n * sizeof(wchar_t));
        return pp + n * sizeof(wchar_t);
    }
}

/* Predefined control class atoms: 0x0080 button, 0x0081 edit,
 * 0x0082 static. */
static unsigned char *emit_item(unsigned char *pp, DWORD style, DWORD ex,
                                short x, short y, short cx, short cy,
                                WORD id, WORD atom, const wchar_t *title)
{
    DLGITEMTEMPLATE *it;
    pp = (unsigned char *)align_dword(pp);
    it = (DLGITEMTEMPLATE *)pp;
    it->style           = style | WS_CHILD | WS_VISIBLE;
    it->dwExtendedStyle = ex;
    it->x = x; it->y = y; it->cx = cx; it->cy = cy;
    it->id = id;
    pp = (unsigned char *)(it + 1);
    pp = emit_word(pp, 0xFFFF);
    pp = emit_word(pp, atom);
    pp = emit_utf16(pp, title);
    return emit_word(pp, 0);        /* no creation data */
}

#define UPD_DLG_W  300
#define UPD_DLG_H  196

static DLGTEMPLATE *upd_build_template(int mode, int required)
{
    static unsigned char buf[4096];
    unsigned char *pp = buf;
    DLGTEMPLATE *dt;
    WORD items;
    int three = (mode == UPD_MODE_STARTUP) && !required;

    /* heading, versions, notes label, notes box, checkbox, buttons */
    items = (WORD)(5 + (three ? 3 : 2));

    memset(buf, 0, sizeof buf);
    dt = (DLGTEMPLATE *)pp;
    dt->style = DS_MODALFRAME | DS_CENTER | DS_SETFONT |
                WS_POPUP | WS_CAPTION | WS_SYSMENU;
    dt->dwExtendedStyle = 0;
    dt->cdit = items;
    dt->x = 0; dt->y = 0; dt->cx = UPD_DLG_W; dt->cy = UPD_DLG_H;
    pp = (unsigned char *)(dt + 1);
    pp = emit_word(pp, 0);                       /* no menu    */
    pp = emit_word(pp, 0);                       /* std class  */
    pp = emit_utf16(pp, L"Software Update");     /* caption    */
    pp = emit_word(pp, 8);                       /* font size  */
    pp = emit_utf16(pp, L"MS Shell Dlg");

    pp = emit_item(pp, SS_LEFT, 0, 10, 8, 280, 12,
                   IDC_UPD_HEADING, 0x0082, L"");
    pp = emit_item(pp, SS_LEFT, 0, 10, 24, 280, 20,
                   IDC_UPD_VERSIONS, 0x0082, L"");
    pp = emit_item(pp, SS_LEFT, 0, 10, 50, 100, 9,
                   (WORD)-1, 0x0082, L"Release notes:");
    pp = emit_item(pp,
                   ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL |
                   WS_VSCROLL | WS_BORDER | WS_TABSTOP,
                   WS_EX_CLIENTEDGE, 10, 62, 280, 82,
                   IDC_UPD_NOTES, 0x0081, L"");
    pp = emit_item(pp, BS_AUTOCHECKBOX | WS_TABSTOP, 0, 10, 152, 180, 10,
                   IDC_UPD_STARTUP, 0x0080,
                   L"Check for updates on startup");

    if (three) {
        pp = emit_item(pp, BS_DEFPUSHBUTTON | WS_TABSTOP, 0,
                       84, 172, 62, 14, IDOK, 0x0080, L"Update Now");
        pp = emit_item(pp, BS_PUSHBUTTON | WS_TABSTOP, 0,
                       150, 172, 66, 14, IDCANCEL, 0x0080,
                       L"Remind Me Later");
        pp = emit_item(pp, BS_PUSHBUTTON | WS_TABSTOP, 0,
                       220, 172, 70, 14, IDD_UPD_SKIP, 0x0080,
                       L"Skip This Version");
    } else {
        pp = emit_item(pp, BS_DEFPUSHBUTTON | WS_TABSTOP, 0,
                       154, 172, 62, 14, IDOK, 0x0080, L"Update Now");
        pp = emit_item(pp, BS_PUSHBUTTON | WS_TABSTOP, 0,
                       224, 172, 62, 14, IDCANCEL, 0x0080, L"Cancel");
    }
    return dt;
}

static INT_PTR CALLBACK upd_dlg_proc(HWND dlg, UINT msg,
                                     WPARAM wParam, LPARAM lParam)
{
    static const UpdDlgData *d = NULL;

    switch (msg) {
    case WM_INITDIALOG: {
        char line[512];
        d = (const UpdDlgData *)lParam;
        if (!d) return TRUE;

        SetDlgItemTextA(dlg, IDC_UPD_HEADING,
                        d->required
                          ? "This version is no longer supported. "
                            "An update is required."
                          : "A new version of the Suite is available.");

        if (d->m->msi_bytes > 0)
            _snprintf(line, sizeof line - 1,
                      "You have %s.    Available: %s    (%llu KB)",
                      SUITE_VERSION_STR, d->m->version,
                      (d->m->msi_bytes + 1023) / 1024);
        else
            _snprintf(line, sizeof line - 1,
                      "You have %s.    Available: %s",
                      SUITE_VERSION_STR, d->m->version);
        line[sizeof line - 1] = 0;
        SetDlgItemTextA(dlg, IDC_UPD_VERSIONS, line);

        /* An edit control breaks lines on CRLF; the manifest carries
         * bare LF, which would render as one run-on paragraph. */
        {
            const char *src = d->m->notes;
            size_t srclen = strlen(src);
            char *crlf = (char *)malloc(srclen * 2 + 3);
            if (crlf) {
                size_t i, o = 0;
                for (i = 0; i < srclen; i++) {
                    if (src[i] == 10 && (i == 0 || src[i - 1] != 13))
                        crlf[o++] = 13;
                    crlf[o++] = src[i];
                }
                crlf[o] = 0;
                SetDlgItemTextA(dlg, IDC_UPD_NOTES,
                                crlf[0] ? crlf
                                        : "No release notes were published.");
                free(crlf);
            } else {
                SetDlgItemTextA(dlg, IDC_UPD_NOTES, src);
            }
        }

        CheckDlgButton(dlg, IDC_UPD_STARTUP,
                       suite_update_startup_enabled() ? BST_CHECKED
                                                      : BST_UNCHECKED);

        /* The dialog manager would hand focus to the first tabstop,
         * which is the notes box, and an edit that receives focus comes
         * up with everything selected. Release notes rendered as a
         * block of highlighted text look like an error. Put the caret
         * at the top with nothing selected and give focus to the
         * default button instead; returning FALSE tells the dialog
         * manager the focus has been placed deliberately. */
        SendDlgItemMessageA(dlg, IDC_UPD_NOTES, EM_SETSEL, 0, 0);
        SetFocus(GetDlgItem(dlg, IDOK));
        return FALSE;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id == IDOK || id == IDCANCEL || id == IDD_UPD_SKIP) {
            /* The checkbox is a setting in its own right, so it is saved
             * whichever button closes the dialog, Cancel included. */
            suite_update_set_startup_enabled(
                IsDlgButtonChecked(dlg, IDC_UPD_STARTUP) == BST_CHECKED);
            EndDialog(dlg, id);
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

/* Returns IDOK, IDCANCEL or IDD_UPD_SKIP. */
static INT_PTR upd_show_dialog(HWND parent, const UpdManifest *m,
                               int mode, int required)
{
    UpdDlgData d;
    d.m = m;
    d.mode = mode;
    d.required = required;
    return DialogBoxIndirectParamA(GetModuleHandleA(NULL),
                                   upd_build_template(mode, required),
                                   parent, upd_dlg_proc, (LPARAM)&d);
}

/* ------------------------------------------------------------------ */
/* Handing over to the installer                                       */
/* ------------------------------------------------------------------ */

/*
 * Start msiexec on the verified package, detached, then ask the Suite to
 * close so the MSI is upgrading an application that is not running.
 *
 * Two details that matter and are easy to get wrong:
 *
 *   THE CHILD'S WORKING DIRECTORY. A process inherits its parent's, and
 *   the Suite's shortcuts set WorkingDirectory to the install folder,
 *   which is exactly the directory the MSI is about to replace. The
 *   child is therefore given the system directory explicitly.
 *
 *   THE SHUTDOWN TAIL. WM_CLOSE is the clean path: DefWindowProc turns
 *   it into DestroyWindow, WM_DESTROY posts the quit message, and
 *   WinMain then joins the module workers. That tail can take several
 *   seconds, because telnet_proto_close waits up to 3 s per handle and
 *   the Zmodem receiver up to 5 s if a transfer is in flight. msiexec is
 *   already running by then; if it reaches the files first it raises its
 *   own in-use handling, which the package is authored to survive.
 */
static int launch_installer(const wchar_t *msi_path)
{
    wchar_t sysdir[MAX_PATH];
    wchar_t cmd[MAX_PATH * 2 + 64];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;

    if (!msi_path || !msi_path[0]) return 0;
    if (GetSystemDirectoryW(sysdir, MAX_PATH) == 0) return 0;

    _snwprintf(cmd, sizeof cmd / sizeof cmd[0] - 1,
               L"\"%s\\msiexec.exe\" /i \"%s\"", sysdir, msi_path);
    cmd[sizeof cmd / sizeof cmd[0] - 1] = 0;

    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    ZeroMemory(&pi, sizeof pi);

    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE,
                        0, NULL, sysdir, &si, &pi))
        return 0;

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 1;
}

/* ------------------------------------------------------------------ */
/* UI thread                                                           */
/* ------------------------------------------------------------------ */

static void join_worker(void)
{
    if (g_thread) {
        WaitForSingleObject(g_thread, 30000);
        CloseHandle(g_thread);
        g_thread = NULL;
    }
}

static void start_check(HWND main_hwnd, int mode)
{
    DWORD tid;

    g_main = main_hwnd;
    g_check_mode = mode;

    if (InterlockedCompareExchange(&g_busy, 1, 0) != 0) {
        if (mode == UPD_MODE_MANUAL)
            suite_set_status("An update check is already running.");
        return;
    }
    join_worker();

    g_thread = CreateThread(NULL, 0, check_thread, NULL, 0, &tid);
    if (!g_thread) {
        InterlockedExchange(&g_busy, 0);
        if (mode == UPD_MODE_MANUAL)
            MessageBoxA(main_hwnd, "The update check could not be started.",
                        "Software Update", MB_OK | MB_ICONWARNING);
    }
}

void suite_update_check(HWND main_hwnd)
{
    start_check(main_hwnd, UPD_MODE_MANUAL);
}

/*
 * The startup check. Two gates before a single packet leaves, in
 * cheapest-first order:
 *
 *   1. the user's setting; off means off;
 *   2. the throttle, so launching the Suite six times in an afternoon
 *      costs the update host one request rather than six.
 *
 * Everything else is decided once the manifest has arrived. This never
 * blocks: the worst case is a registry read and a CreateThread.
 */
void suite_update_startup_check(HWND main_hwnd)
{
    if (!suite_update_startup_enabled()) return;
    if (hours_since_last_check() < (double)UPDATE_THROTTLE_HOURS) return;
    start_check(main_hwnd, UPD_MODE_STARTUP);
}

static void start_download(HWND hwnd, const UpdManifest *m)
{
    UpdDownload *d;
    DWORD tid;

    d = (UpdDownload *)calloc(1, sizeof *d);
    if (!d) { InterlockedExchange(&g_busy, 0); return; }
    d->m = *m;

    join_worker();
    g_thread = CreateThread(NULL, 0, download_thread, d, 0, &tid);
    if (!g_thread) {
        free(d);
        InterlockedExchange(&g_busy, 0);
        MessageBoxA(hwnd, "The download could not be started.",
                    "Software Update", MB_OK | MB_ICONWARNING);
    }
}

static void on_result(HWND hwnd, UpdResult *r)
{
    char msg[1024];
    char skipped[64];
    int  manual;

    if (!r) return;
    manual = (r->mode == UPD_MODE_MANUAL);

    /* ---- could not check ---- */
    if (!r->ok) {
        suite_set_status("Update check failed.");
        if (manual) {
            _snprintf(msg, sizeof msg - 1,
                      "Could not check for updates.\r\n\r\n%s\r\n\r\n"
                      "You are running %s.", r->error, SUITE_VERSION_STR);
            msg[sizeof msg - 1] = 0;
            MessageBoxA(hwnd, msg, "Software Update", MB_OK | MB_ICONWARNING);
        }
        /* A startup check that cannot reach the network says nothing.
         * The user did not ask, and a modal box on every launch behind a
         * captive portal would be its own bug. */
        free(r);
        InterlockedExchange(&g_busy, 0);
        return;
    }

    /* ---- nothing published, or we are current or ahead ---- */
    if (r->placeholder || r->cmp >= 0) {
        suite_set_status("The Suite is up to date.");
        if (manual) {
            const char *tail =
                r->placeholder
                  ? "No release has been published yet."
                  : (r->cmp > 0
                       ? "That is newer than the published version, so "
                         "there is nothing to install."
                       : "This is the current version.");
            _snprintf(msg, sizeof msg - 1,
                      "You are running %s.\r\n\r\n%s",
                      SUITE_VERSION_STR, tail);
            msg[sizeof msg - 1] = 0;
            MessageBoxA(hwnd, msg, "Software Update", MB_OK);
        }
        free(r);
        InterlockedExchange(&g_busy, 0);
        return;
    }

    /* ---- newer version available ---- */

    /* A skip suppresses the STARTUP prompt only, and only up to the
     * version skipped: publish anything newer and the prompt returns. A
     * manual check always shows it, because the user just asked. */
    if (!manual && settings_read(SET_SKIPPED_VERSION, skipped, sizeof skipped)
        && suite_update_compare_versions(r->m.version, skipped) <= 0) {
        free(r);
        InterlockedExchange(&g_busy, 0);
        return;
    }

    g_pending = r->m;
    suite_set_status("An update is available.");
    {
        int mode = r->mode, required = r->required;
        char version[64];
        INT_PTR answer;

        copy_str(version, sizeof version, r->m.version);
        free(r);

        answer = upd_show_dialog(hwnd, &g_pending, mode, required);

        if (answer == IDOK) {
            start_download(hwnd, &g_pending);
            return;                       /* g_busy stays held */
        }
        if (answer == IDD_UPD_SKIP) {
            settings_write(SET_SKIPPED_VERSION, version);
            suite_set_status("This version will be skipped.");
        } else {
            suite_set_status("Update postponed.");
        }
    }
    InterlockedExchange(&g_busy, 0);
}

static void on_download_done(HWND hwnd, UpdDownload *d)
{
    char msg[1400];
    char path_a[MAX_PATH * 2];

    if (!d) return;

    if (!d->ok) {
        suite_set_status("The update was not installed.");
        _snprintf(msg, sizeof msg - 1,
                  "The update was not installed.\r\n\r\n%s\r\n\r\n"
                  "You are still running %s, and nothing has changed.",
                  d->error, SUITE_VERSION_STR);
        msg[sizeof msg - 1] = 0;
        MessageBoxA(hwnd, msg, "Software Update", MB_OK | MB_ICONWARNING);
        free(d);
        InterlockedExchange(&g_busy, 0);
        return;
    }

    WideCharToMultiByte(CP_ACP, 0, d->path, -1, path_a, sizeof path_a,
                        NULL, NULL);
    suite_set_status("Update downloaded and verified.");

    _snprintf(msg, sizeof msg - 1,
              "Version %s has been downloaded and its SHA-256 matches the "
              "published checksum.\r\n\r\n"
              "The Suite will now close and the installer will start.\r\n"
              "Reopen the Suite when it has finished.\r\n\r\n"
              "Continue?", d->m.version);
    msg[sizeof msg - 1] = 0;

    if (MessageBoxA(hwnd, msg, "Ready to install",
                    MB_OKCANCEL | MB_ICONINFORMATION) != IDOK) {
        _snprintf(msg, sizeof msg - 1,
                  "The verified installer is here if you want to run it "
                  "yourself:\r\n\r\n%s", path_a);
        msg[sizeof msg - 1] = 0;
        MessageBoxA(hwnd, msg, "Software Update", MB_OK);
        suite_set_status("Update ready but not installed.");
        free(d);
        InterlockedExchange(&g_busy, 0);
        return;
    }

    if (!launch_installer(d->path)) {
        _snprintf(msg, sizeof msg - 1,
                  "The installer could not be started.\r\n\r\n"
                  "It is here if you want to run it yourself:\r\n%s",
                  path_a);
        msg[sizeof msg - 1] = 0;
        MessageBoxA(hwnd, msg, "Software Update", MB_OK | MB_ICONWARNING);
        free(d);
        InterlockedExchange(&g_busy, 0);
        return;
    }

    /* The installer is running. Release the busy flag before asking for
     * the close: the shutdown path joins this module's worker, and a
     * flag still held would be the last thing anyone saw. */
    free(d);
    InterlockedExchange(&g_busy, 0);
    suite_set_status("Closing so the installer can run...");
    PostMessageA(hwnd, WM_CLOSE, 0, 0);
}

BOOL suite_update_on_message(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    (void)wParam;
    switch (msg) {
    case WM_APP_UPDATE_STATUS: {
        char *text = (char *)lParam;
        if (text) { suite_set_status(text); free(text); }
        return TRUE;
    }
    case WM_APP_UPDATE_RESULT:
        on_result(hwnd, (UpdResult *)lParam);
        return TRUE;
    case WM_APP_UPDATE_DLDONE:
        on_download_done(hwnd, (UpdDownload *)lParam);
        return TRUE;
    }
    return FALSE;
}

void suite_update_shutdown(void)
{
    join_worker();
}
