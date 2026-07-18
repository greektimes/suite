/*
 * win_stubs.c - Windows stubs for freeWAIS futil functions
 *
 * Copyright (c) 2026, Dimitri Papadopoulos and The Montreal Greek Times
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   1. Redistributions of source code must retain the above copyright notice,
 *      this list of conditions and the following disclaimer.
 *
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *
 *   3. Neither the name of Dimitri Papadopoulos, The Montreal Greek Times,
 *      nor the names of its contributors may be used to endorse or promote
 *      products derived from this software without specific prior written
 *      permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES HOWEVER CAUSED.
 * See the LICENSE file in the repository root for the full license text.
 *
 * Part of the Greek Times WAIS Search project (v0.9.1-beta2).
 * https://github.com/greektimes/wais
 */

#include "win_platform.h"
#include "cdialect.h"
#include "cutil.h"
#include "futil.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*----------------------------------------------------------------------*/
/* File utility stubs (from futil.h)                                     */
/*----------------------------------------------------------------------*/

FILE *
fs_fopen(fileName, mode)
char *fileName;
char *mode;
{
    return fopen(fileName, mode);
}

long
fs_fclose(file)
FILE *file;
{
    if (file != NULL)
        return fclose(file);
    return 0;
}

long
fs_fseek(file, offset, wherefrom)
FILE *file;
long offset;
long wherefrom;
{
    return fseek(file, offset, (int)wherefrom);
}

long
fs_ftell(file)
FILE *file;
{
    return ftell(file);
}

char *
fs_fzcat(fileName)
char *fileName;
{
    (void)fileName;
    return NULL;
}

void
grow_file(file, length)
FILE *file;
long length;
{
    (void)file;
    (void)length;
}

long
read_bytes(n_bytes, stream)
long n_bytes;
FILE *stream;
{
    (void)n_bytes;
    (void)stream;
    return 0;
}

long
write_bytes(value, n_bytes, stream)
long value;
long n_bytes;
FILE *stream;
{
    (void)value;
    (void)n_bytes;
    (void)stream;
    return 0;
}

long
read_bytes_from_memory(n_bytes, block)
long n_bytes;
unsigned char *block;
{
    (void)n_bytes;
    (void)block;
    return 0;
}

time_t
file_write_date(filename)
char *filename;
{
    (void)filename;
    return 0;
}

char *
truename(filename, full_path)
char *filename;
char *full_path;
{
    strcpy(full_path, filename);
    return full_path;
}

long
file_length(stream)
FILE *stream;
{
    long pos, len;
    pos = ftell(stream);
    fseek(stream, 0, SEEK_END);
    len = ftell(stream);
    fseek(stream, pos, SEEK_SET);
    return len;
}

char *
pathname_name(pathname)
char *pathname;
{
    char *p = strrchr(pathname, '/');
    char *q = strrchr(pathname, '\\');
    if (q > p) p = q;
    return p ? p + 1 : pathname;
}

char *
pathname_directory(pathname, destination)
char *pathname;
char *destination;
{
    char *p = strrchr(pathname, '/');
    char *q = strrchr(pathname, '\\');
    if (q > p) p = q;
    if (p) {
        long len = p - pathname;
        strncpy(destination, pathname, len);
        destination[len] = '\0';
    } else {
        strcpy(destination, ".");
    }
    return destination;
}

char *
current_user_name(void)
{
    return getuser_name();
}

boolean
probe_file(filename)
char *filename;
{
    FILE *f = fopen(filename, "r");
    if (f) { fclose(f); return true; }
    return false;
}

boolean
probe_file_possibly_compressed(filename)
char *filename;
{
    return probe_file(filename);
}

boolean
touch_file(filename)
char *filename;
{
    FILE *f = fopen(filename, "a");
    if (f) { fclose(f); return true; }
    return false;
}

char *
merge_pathnames(pathname, directory)
char *pathname;
char *directory;
{
    (void)directory;
    return pathname;
}

boolean
read_string_from_file(stream, array, array_length)
FILE *stream;
char *array;
long array_length;
{
    if (fgets(array, (int)array_length, stream))
        return true;
    return false;
}

long
count_lines(stream)
FILE *stream;
{
    (void)stream;
    return 0;
}

/* End of futil stubs */
