/*
 * centry.h
 * $Id: $
 *
 * Copyright (c) 2007 Chris Pickel <sfiera@macports.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#if HAVE_CONFIG_H
#include <config.h>
#endif

#include <sqlite3.h>

typedef void reg_error_destructor(char* description);

typedef struct {
    char* code;
    const char* description;
    reg_error_destructor* free;
} reg_error;

typedef struct {
    sqlite_int64 rowid;
    sqlite3* db;
} reg_entry;

typedef int (cast_function)(void* userdata, void** dst, void* src,
        reg_error* errPtr);
typedef void (free_function)(void* userdata, void** list, int count);

void reg_error_destruct(reg_error* errPtr);

reg_entry* reg_entry_create(sqlite3* db, char* name, char* version,
        char* revision, char* variants, char* epoch, reg_error* errPtr);

int reg_entry_delete(sqlite3* db, reg_entry** entries, int entry_count,
        reg_error* errPtr);

void reg_entry_free(sqlite3* db, reg_entry** entries, int entry_count);

int reg_entry_search(sqlite3* db, char** keys, char** vals, int key_count,
        int strategy, reg_entry*** entries, reg_error* errPtr);

int reg_entry_installed(sqlite3* db, char* name, char* version, 
        reg_entry*** entries, reg_error* errPtr);

int reg_entry_active(sqlite3* db, char* name, char* version, 
        reg_entry*** entries, reg_error* errPtr);

int reg_entry_owner(sqlite3* db, char* path, reg_entry** entry,
        reg_error* errPtr);
