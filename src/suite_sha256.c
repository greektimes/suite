/*
 * suite_sha256.c - SHA-256 over a file, via Windows CNG. See the header.
 *
 * Part of the Montreal Greek Times Unicorn Suite.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>

#include "suite_sha256.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)
#endif

#define SHA256_CHUNK  65536

static void sh_err(char *err, size_t cap, const char *fmt, ...)
{
    va_list ap;
    if (!err || cap == 0) return;
    va_start(ap, fmt);
    _vsnprintf(err, cap - 1, fmt, ap);
    va_end(ap);
    err[cap - 1] = '\0';
}

int suite_sha256_file(const wchar_t *path, char *out_hex,
                      char *err, size_t errcap)
{
    static const char hexdig[] = "0123456789abcdef";

    BCRYPT_ALG_HANDLE  alg  = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    HANDLE   fh   = INVALID_HANDLE_VALUE;
    PUCHAR   obj  = NULL;
    PUCHAR   buf  = NULL;
    UCHAR    digest[32];
    DWORD    obj_len = 0, cb = 0, hash_len = 0;
    NTSTATUS st;
    int      rc = 1;
    DWORD    i;

    if (!out_hex) return 1;
    out_hex[0] = '\0';
    if (!path) { sh_err(err, errcap, "No file to hash."); return 1; }

    st = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0);
    if (st != STATUS_SUCCESS) {
        sh_err(err, errcap, "SHA-256 is unavailable (0x%08lx).",
               (unsigned long)st);
        goto done;
    }
    st = BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&obj_len,
                           sizeof(obj_len), &cb, 0);
    if (st != STATUS_SUCCESS) {
        sh_err(err, errcap, "SHA-256 setup failed (0x%08lx).",
               (unsigned long)st);
        goto done;
    }
    st = BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, (PUCHAR)&hash_len,
                           sizeof(hash_len), &cb, 0);
    if (st != STATUS_SUCCESS || hash_len != sizeof(digest)) {
        sh_err(err, errcap, "SHA-256 reported an unexpected digest size.");
        goto done;
    }

    obj = (PUCHAR)HeapAlloc(GetProcessHeap(), 0, obj_len);
    buf = (PUCHAR)HeapAlloc(GetProcessHeap(), 0, SHA256_CHUNK);
    if (!obj || !buf) { sh_err(err, errcap, "Out of memory."); goto done; }

    st = BCryptCreateHash(alg, &hash, obj, obj_len, NULL, 0, 0);
    if (st != STATUS_SUCCESS) {
        sh_err(err, errcap, "SHA-256 could not start (0x%08lx).",
               (unsigned long)st);
        goto done;
    }

    fh = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (fh == INVALID_HANDLE_VALUE) {
        sh_err(err, errcap, "Could not open the file to check it (err %lu).",
               GetLastError());
        goto done;
    }

    for (;;) {
        DWORD got = 0;
        if (!ReadFile(fh, buf, SHA256_CHUNK, &got, NULL)) {
            sh_err(err, errcap, "Could not read the file to check it (err %lu).",
                   GetLastError());
            goto done;
        }
        if (got == 0) break;
        st = BCryptHashData(hash, buf, got, 0);
        if (st != STATUS_SUCCESS) {
            sh_err(err, errcap, "SHA-256 failed while reading (0x%08lx).",
                   (unsigned long)st);
            goto done;
        }
    }

    st = BCryptFinishHash(hash, digest, hash_len, 0);
    if (st != STATUS_SUCCESS) {
        sh_err(err, errcap, "SHA-256 could not finish (0x%08lx).",
               (unsigned long)st);
        goto done;
    }

    for (i = 0; i < hash_len; i++) {
        out_hex[i * 2]     = hexdig[(digest[i] >> 4) & 0x0f];
        out_hex[i * 2 + 1] = hexdig[digest[i] & 0x0f];
    }
    out_hex[hash_len * 2] = '\0';
    rc = 0;

done:
    if (fh != INVALID_HANDLE_VALUE) CloseHandle(fh);
    if (hash) BCryptDestroyHash(hash);
    if (alg)  BCryptCloseAlgorithmProvider(alg, 0);
    if (obj)  HeapFree(GetProcessHeap(), 0, obj);
    if (buf)  HeapFree(GetProcessHeap(), 0, buf);
    if (rc)   out_hex[0] = '\0';
    return rc;
}

int suite_sha256_hex_equal(const char *a, const char *b)
{
    size_t i;
    unsigned diff = 0;

    if (!a || !b) return 0;
    if (strlen(a) != 64 || strlen(b) != 64) return 0;

    for (i = 0; i < 64; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca >= 'A' && ca <= 'F') ca = (unsigned char)(ca + 32);
        if (cb >= 'A' && cb <= 'F') cb = (unsigned char)(cb + 32);
        diff |= (unsigned)(ca ^ cb);
    }
    return diff == 0;
}
