/*
 * centry.c
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

#include <string.h>
#include <stdlib.h>
#include <sqlite3.h>

#include "centry.h"

/**
 * Concatenates `src` to string `dst`.
 *
 * Simple concatenation. Only guaranteed to work with strings that have been
 * allocated with `malloc`. Amortizes cost of expanding string buffer for O(N)
 * concatenation and such. Uses `memcpy` in favor of `strcpy` in hopes it will
 * perform a bit better. If passing in a static string to dst, make sure
 * dst_space starts at dst_len. Also make sure dst_space is never 0 (so don't
 * use "" as the starter string, allocate some space);
 */
void reg_strcat(char** dst, int* dst_len, int* dst_space, char* src) {
    int src_len = strlen(src);
    if (*dst_len + src_len >= *dst_space) {
        char* old_dst = *dst;
        char* new_dst = malloc(*dst_space * 2 * sizeof(char));
        *dst_space *= 2;
        memcpy(new_dst, old_dst, *dst_len);
        *dst = new_dst;
        free(old_dst);
    }
    memcpy(&((*dst)[*dst_len]), src, src_len+1);
    *dst_len += src_len;
}

void reg_error_destruct(reg_error* errPtr) {
    if (errPtr->free) {
        errPtr->free(errPtr->description);
    }
}

/**
 * Appends `src` to the list `dst`.
 *
 * It's like `reg_strcat`, except `src` represents an element and not a sequence
 * of `char`s.
 */
static void reg_listcat(void*** dst, int* dst_len, int* dst_space, void* src) {
    if (*dst_len == *dst_space) {
        void** old_dst = *dst;
        void** new_dst = malloc(*dst_space * 2 * sizeof(void*));
        *dst_space *= 2;
        memcpy(new_dst, old_dst, *dst_len);
        *dst = new_dst;
        free(old_dst);
    }
    (*dst)[*dst_len] = src;
    (*dst_len)++;
}

/**
 * Returns the operator to use for the given strategy.
 */
static char* reg_strategy_op(int strategy, reg_error* errPtr) {
    switch (strategy) {
        case 0:
            return "=";
        case 1:
            return " GLOB ";
        case 2:
            return " REGEXP ";
        default:
            errPtr->code = "registry::invalid-strategy";
            errPtr->description = "invalid matching strategy specified";
            errPtr->free = NULL;
            return NULL;
    }
}

/**
 * Sets `errPtr` according to the last error in `db`.
 *
 * TODO: implement error_free so this won't leak
 */
void reg_sqlite_error(sqlite3* db, reg_error* errPtr, char* query) {
    errPtr->code = "registry::sqlite-error";
    errPtr->free = sqlite3_free;
    if (query == NULL) {
        errPtr->description = sqlite3_mprintf("sqlite error: %s",
                sqlite3_errmsg(db));
    } else {
        errPtr->description = sqlite3_mprintf("sqlite error: %s while "
                "executing query: %s", sqlite3_errmsg(db), query);
    }
}

/**
 * registry::entry create portname version revision variants epoch ?name?
 *
 * Unlike the old registry::new_entry, revision, variants, and epoch are all
 * required. That's OK because there's only one place this function is called,
 * and it's called with all of them there.
 */
reg_entry* reg_entry_create(sqlite3* db, char* name, char* version,
        char* revision, char* variants, char* epoch, reg_error* errPtr) {
    sqlite3_stmt* stmt;
    char* query = "INSERT INTO registry.ports "
        "(name, version, revision, variants, epoch) VALUES (?, ?, ?, ?, ?)";
    if ((sqlite3_prepare(db, query, -1, &stmt, NULL) == SQLITE_OK)
            && (sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC)
                == SQLITE_OK)
            && (sqlite3_bind_text(stmt, 2, version, -1, SQLITE_STATIC)
                == SQLITE_OK)
            && (sqlite3_bind_text(stmt, 3, revision, -1, SQLITE_STATIC)
                == SQLITE_OK)
            && (sqlite3_bind_text(stmt, 4, variants, -1, SQLITE_STATIC)
                == SQLITE_OK)
            && (sqlite3_bind_text(stmt, 5, epoch, -1, SQLITE_STATIC)
                == SQLITE_OK)
            && (sqlite3_step(stmt) == SQLITE_DONE)) {
        sqlite_int64 rowid = sqlite3_last_insert_rowid(db);
        reg_entry* entry = malloc(sizeof(reg_entry));
        entry->rowid = rowid;
        entry->db = db;
        sqlite3_finalize(stmt);
        return entry;
    } else {
        reg_sqlite_error(db, errPtr, query);
        sqlite3_finalize(stmt);
        return NULL;
    }
}

/**
 * returns the number actually deleted
 */
int reg_entry_delete(sqlite3* db, reg_entry** entries, int entry_count,
        reg_error* errPtr) {
    sqlite3_stmt* stmt;
    /* BEGIN */
    char* query = "DELETE FROM registry.ports WHERE rowid=?";
    if (sqlite3_prepare(db, query, -1, &stmt, NULL) == SQLITE_OK) {
        int i;
        for (i=0; i<entry_count; i++) {
            if ((sqlite3_bind_int64(stmt, 1, entries[i]->rowid) == SQLITE_OK)
                    && (sqlite3_step(stmt) == SQLITE_DONE)) {
                if (sqlite3_changes(db) == 0) {
                    errPtr->code = "registry::invalid-entry";
                    errPtr->description = "an invalid entry was passed";
                    errPtr->free = NULL;
                    reg_entry_free(db, entries, i);
                    /* COMMIT */
                    return i;
                }
            } else {
                reg_sqlite_error(db, errPtr, query);
                reg_entry_free(db, entries, i);
                /* COMMIT */
                return i;
            }
            sqlite3_reset(stmt);
        }
        reg_entry_free(db, entries, entry_count);
        /* COMMIT */
        return entry_count;
    } else {
        reg_sqlite_error(db, errPtr, query);
        /* ROLLBACK */
        return 0;
    }
}

/*
 * Frees the entries in `entries`.
 */
void reg_entry_free(sqlite3* db UNUSED, reg_entry** entries, int entry_count) {
    int i;
    for (i=0; i<entry_count; i++) {
        free(entries[i]);
    }
}

static int reg_stmt_to_entry(void* userdata, void** entry, void* stmt,
        reg_error* errPtr UNUSED) {
    reg_entry* e = malloc(sizeof(reg_entry));
    e->db = (sqlite3*)userdata;
    e->rowid = sqlite3_column_int64(stmt, 0);
    *entry = e;
    return 1;
}

static int reg_all_objects(sqlite3* db, char* query, int query_len,
        void*** objects, cast_function* fn, free_function* del,
        reg_error* errPtr) {
    int r = SQLITE_ROW;
    reg_entry* entry;
    void** results = malloc(10*sizeof(void*));
    int result_count = 0;
    int result_space = 10;
    sqlite3_stmt* stmt;
    if (sqlite3_prepare(db, query, query_len, &stmt, NULL) == SQLITE_OK) {
        while (r != SQLITE_DONE) {
            r = sqlite3_step(stmt);
            switch (r) {
                case SQLITE_ROW:
                    if (fn(db, (void**)&entry, stmt, errPtr)) {
                        reg_listcat(&results, &result_count, &result_space,
                                entry);
                        continue;
                    } else {
                        del(NULL, results, result_count);
                        free(results);
                        sqlite3_finalize(stmt);
                        return -1;
                    }
                case SQLITE_DONE:
                    break;
                default:
                    del(NULL, results, result_count);
                    free(results);
                    sqlite3_reset(stmt);
                    reg_sqlite_error(db, errPtr, query);
                    return -1;
            }
        }
        *objects = results;
        return result_count;
    } else {
        reg_sqlite_error(db, errPtr, query);
        free(results);
        return -1;
    }
}

/*
 * Searches the registry for ports for which each key's value is equal to the
 * given value. To find all ports, pass 0 key-value pairs.
 *
 * Vulnerable to SQL-injection attacks in the `keys` field. Pass it valid keys,
 * please.
 */
int reg_entry_search(sqlite3* db, char** keys, char** vals, int key_count,
        int strategy, reg_entry*** entries, reg_error* errPtr) {
    int i;
    char* kwd = " WHERE ";
    char* query;
    int query_len = 0;
    int query_space = 32;
    int result;
    /* get the strategy */
    char* op = reg_strategy_op(strategy, errPtr);
    if (op == NULL) {
        return -1;
    }
    query = malloc(33);
    reg_strcat(&query, &query_len, &query_space,
            "SELECT rowid FROM registry.ports");
    /* build the query */
    for (i=0; i<key_count; i+=1) {
        char* cond = sqlite3_mprintf("%s%s%s'%q'", kwd, keys[i], op, vals[i]);
        reg_strcat(&query, &query_len, &query_space, cond);
        sqlite3_free(cond);
        kwd = " AND ";
    }
    /* do the query */
    result = reg_all_objects(db, query, query_len, (void***)entries,
            reg_stmt_to_entry, (free_function*)reg_entry_free, errPtr);
    free(query);
    return result;
}

/**
 * TODO: fix this to return ports where state=active too
 * TODO: add more arguments (epoch, revision, variants), maybe
 */
int reg_entry_installed(sqlite3* db, char* name, char* version, 
        reg_entry*** entries, reg_error* errPtr) {
    char* keys[] = { "state", "name", "version" };
    char* values[] = { "installed", NULL, NULL };
    int key_count;
    if (name == NULL) {
        key_count = 1;
    } else {
        values[1] = name;
        if (version == NULL) {
            key_count = 2;
        } else {
            key_count = 3;
            values[2] = version;
        }
    }
    return reg_entry_search(db, keys, values, 0, key_count, entries, errPtr);
}

/**
 */
int reg_entry_active(sqlite3* db, char* name, char* version, 
        reg_entry*** entries, reg_error* errPtr) {
    char* keys[] = { "state", "name", "version" };
    char* values[] = { "active", NULL, NULL };
    int key_count;
    if (name == NULL) {
        key_count = 1;
    } else {
        values[1] = name;
        if (version == NULL) {
            key_count = 2;
        } else {
            key_count = 3;
            values[2] = version;
        }
    }
    return reg_entry_search(db, keys, values, 0, key_count, entries, errPtr);
}

int reg_entry_owner(sqlite3* db, char* path, reg_entry** entry,
        reg_error* errPtr) {
    sqlite3_stmt* stmt;
    reg_entry* result;
    char* query = "SELECT port_id FROM files WHERE path=?";
    if ((sqlite3_prepare(db, query, -1, &stmt, NULL) == SQLITE_OK)
            && (sqlite3_bind_text(stmt, 1, path, -1, SQLITE_STATIC)
                == SQLITE_OK)) {
        int r = sqlite3_step(stmt);
        switch (r) {
            case SQLITE_ROW:
                result = malloc(sizeof(reg_entry));
                result->rowid = sqlite3_column_int64(stmt, 0);
                result->db = db;
                sqlite3_finalize(stmt);
                *entry = result;
                return 1;
            case SQLITE_DONE:
                sqlite3_finalize(stmt);
                *entry = NULL;
                return 1;
            default:
                /* barf */
                sqlite3_finalize(stmt);
                return 0;
        }
    } else {
        reg_sqlite_error(db, errPtr, query);
        sqlite3_finalize(stmt);
        return 0;
    }
}

int reg_entry_propget(sqlite3* db, reg_entry* entry, char* key, char** value,
        reg_error* errPtr) {
    sqlite3_stmt* stmt;
    char* query = sqlite3_mprintf("SELECT `%q` FROM registry.entries "
            "WHERE rowid=%lld", key, entry->rowid);
    if (sqlite3_prepare(db, query, -1, &stmt, NULL) == SQLITE_OK) {
        int r = sqlite3_step(stmt);
        const char* column;
        int len;
        switch (r) {
            case SQLITE_ROW:
                column = sqlite3_column_text(stmt, 0);
                len = sqlite3_column_bytes(stmt, 0);
                *value = malloc(1 + len);
                strcpy(*value, column);
                sqlite3_finalize(stmt);
                return 1;
            case SQLITE_DONE:
                errPtr->code = "registry::invalid-entry";
                errPtr->description = "an invalid entry was passed";
                errPtr->free = NULL;
                sqlite3_finalize(stmt);
                return 0;
            default:
                reg_sqlite_error(db, errPtr, query);
                sqlite3_finalize(stmt);
                return 0;
        }
    } else {
        reg_sqlite_error(db, errPtr, query);
        return 0;
    }
}

int reg_entry_propset(sqlite3* db, reg_entry* entry, char* key, char* value,
        reg_error* errPtr) {
    sqlite3_stmt* stmt;
    char* query = sqlite3_mprintf("UPDATE registry.entries SET `%q` = '%q' "
            "WHERE rowid=%lld", key, value, entry->rowid);
    if (sqlite3_prepare(db, query, -1, &stmt, NULL) == SQLITE_OK) {
        int r = sqlite3_step(stmt);
        switch (r) {
            case SQLITE_DONE:
                sqlite3_finalize(stmt);
                return 1;
            default:
                switch (sqlite3_reset(stmt)) {
                    case SQLITE_CONSTRAINT:
                        errPtr->code = "registry::constraint";
                        errPtr->description = "a constraint was disobeyed";
                        errPtr->free = NULL;
                        sqlite3_finalize(stmt);
                        return 0;
                    default:
                        reg_sqlite_error(db, errPtr, query);
                        sqlite3_finalize(stmt);
                        return 0;
                }
        }
    } else {
        reg_sqlite_error(db, errPtr, query);
        return 0;
    }
}

int reg_entry_map(sqlite3* db, reg_entry* entry, char** files, int file_count,
        reg_error* errPtr) {
    sqlite3_stmt* stmt;
    char* query = "INSERT INTO registry.files (port_id, path) VALUES (?, ?)";
    if ((sqlite3_prepare(db, query, -1, &stmt, NULL) == SQLITE_OK)
            && (sqlite3_bind_int64(stmt, 1, entry->rowid) == SQLITE_OK)) {
        int i;
        for (i=0; i<file_count; i++) {
            if (sqlite3_bind_text(stmt, 2, files[i], -1, SQLITE_STATIC)
                    == SQLITE_OK) {
                int r = sqlite3_step(stmt);
                switch (r) {
                    case SQLITE_DONE:
                        sqlite3_reset(stmt);
                        continue;
                    case SQLITE_ERROR:
                        switch (sqlite3_reset(stmt)) {
                            case SQLITE_CONSTRAINT:
                                errPtr->code = "registry::already-owned";
                                errPtr->description = "mapped file is already "
                                    "owned by another entry";
                                errPtr->free = NULL;
                                sqlite3_finalize(stmt);
                                return i;
                            default:
                                reg_sqlite_error(db, errPtr, query);
                                sqlite3_finalize(stmt);
                                return i;
                        }
                }
            } else {
                reg_sqlite_error(db, errPtr, query);
                sqlite3_finalize(stmt);
                return i;
            }
        }
        sqlite3_finalize(stmt);
        return file_count;
    } else {
        reg_sqlite_error(db, errPtr, query);
        sqlite3_finalize(stmt);
        return 0;
    }
}

int reg_entry_unmap(sqlite3* db, reg_entry* entry, char** files, int file_count,
        reg_error* errPtr) {
    sqlite3_stmt* stmt;
    char* query = "DELETE FROM registry.files WHERE port_id=? AND path=?";
    if ((sqlite3_prepare(db, query, -1, &stmt, NULL) == SQLITE_OK)
            && (sqlite3_bind_int64(stmt, 1, entry->rowid) == SQLITE_OK)) {
        int i;
        for (i=0; i<file_count; i++) {
            if (sqlite3_bind_text(stmt, 2, files[i], -1, SQLITE_STATIC)
                    == SQLITE_OK) {
                int r = sqlite3_step(stmt);
                switch (r) {
                    case SQLITE_DONE:
                        if (sqlite3_changes(db) == 0) {
                            errPtr->code = "registry::not-owned";
                            errPtr->code = "this entry does not own the given "
                                "file";
                            errPtr->free = NULL;
                            sqlite3_finalize(stmt);
                            return i;
                        } else {
                            sqlite3_reset(stmt);
                            continue;
                        }
                    default:
                        reg_sqlite_error(db, errPtr, query);
                        sqlite3_finalize(stmt);
                        return i;
                }
            } else {
                reg_sqlite_error(db, errPtr, query);
                sqlite3_finalize(stmt);
                return i;
            }
        }
        sqlite3_finalize(stmt);
        return file_count;
    } else {
        reg_sqlite_error(db, errPtr, query);
        sqlite3_finalize(stmt);
        return 0;
    }
}

int reg_entry_files(sqlite3* db, reg_entry* entry, char*** files,
        reg_error* errPtr) {
    sqlite3_stmt* stmt;
    char* query = "SELECT path FROM files WHERE port_id=?";
    if ((sqlite3_prepare(db, query, -1, &stmt, NULL) == SQLITE_OK)
            && (sqlite3_bind_int64(stmt, 1, entry->rowid) == SQLITE_OK)) {
        char** result = malloc(10*sizeof(char*));
        int result_count = 0;
        int result_space = 10;
        while (1) {
            char* element;
            const char* column;
            int len, i, r;
            r = sqlite3_step(stmt);
            switch (r) {
                case SQLITE_ROW:
                    column = sqlite3_column_text(stmt, 0);
                    len = sqlite3_column_bytes(stmt, 0);
                    element = malloc(1+len);
                    memcpy(element, column, len+1);
                    reg_listcat((void*)&result, &result_count, &result_space,
                            element);
                    continue;
                case SQLITE_DONE:
                    break;
                default:
                    for (i=0; i<result_count; i++) {
                        free(result[i]);
                    }
                    free(result);
                    reg_sqlite_error(db, errPtr, query);
                    sqlite3_finalize(stmt);
                    return -1;
            }
        }
        sqlite3_finalize(stmt);
        *files = result;
        return result_count;
    } else {
        reg_sqlite_error(db, errPtr, query);
        sqlite3_finalize(stmt);
        return -1;
    }
}

