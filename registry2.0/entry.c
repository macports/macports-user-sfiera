/*
 * entry.c
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
#include <tcl.h>
#include <sqlite3.h>

#include "entry.h"
#include "entryobj.h"
#include "registry.h"
#include "util.h"

int registry_failed(Tcl_Interp* interp, reg_error* errPtr) {
    Tcl_Obj* result = Tcl_NewStringObj(errPtr->description, -1);
    Tcl_SetObjResult(interp, result);
    Tcl_SetErrorCode(interp, errPtr->code, NULL);
    reg_error_destruct(errPtr);
    return TCL_ERROR;
}

static reg_entry* get_entry(Tcl_Interp* interp, char* name, reg_error* errPtr) {
    return (reg_entry*)get_object(interp, name, "entry", entry_obj_cmd, errPtr);
}

static void delete_entry(ClientData clientData) {
    reg_entry_free(NULL, &clientData, 1);
}

static int set_entry(Tcl_Interp* interp, char* name, reg_entry* entry,
        reg_error* errPtr) {
    return set_object(interp, name, entry, "entry", entry_obj_cmd, delete_entry,
                errPtr);
}

/**
 * registry::entry create portname version revision variants epoch
 *
 * Unlike the old registry::new_entry, revision, variants, and epoch are all
 * required. That's OK because there's only one place this function is called,
 * and it's called with all of them there.
 */
static int entry_create(Tcl_Interp* interp, int objc, Tcl_Obj* CONST objv[]) {
    sqlite3* db = registry_db(interp, 1);
    if (objc != 7) {
        Tcl_WrongNumArgs(interp, 2, objv, "name version revision variants "
                "epoch");
        return TCL_ERROR;
    } else if (db == NULL) {
        return TCL_ERROR;
    } else {
        char* name = Tcl_GetString(objv[2]);
        char* version = Tcl_GetString(objv[3]);
        char* revision = Tcl_GetString(objv[4]);
        char* variants = Tcl_GetString(objv[5]);
        char* epoch = Tcl_GetString(objv[6]);
        reg_error error;
        reg_entry* entry = reg_entry_create(db, name, version, revision,
                variants, epoch, &error);
        if (entry != NULL) {
            char* name = unique_name(interp, "registry::entry");
            if (set_entry(interp, name, entry, &error)) {
                Tcl_Obj* res = Tcl_NewStringObj(name, -1);
                Tcl_SetObjResult(interp, res);
                free(name);
                return TCL_OK;
            } else {
                reg_error ignored;
                free(name);
                reg_entry_delete(db, &entry, 1, &ignored);
            }
        }
        return registry_failed(interp, &error);
    }
}

static int obj_to_entry(Tcl_Interp* interp, reg_entry** entry, Tcl_Obj* obj,
        reg_error* errPtr) {
    reg_entry* result = get_entry(interp, Tcl_GetString(obj), errPtr);
    if (result == NULL) {
        return 0;
    } else {
        *entry = result;
        return 1;
    }
}

/**
 * TODO: don't create procs for entries that already have them
 */
static int entry_to_obj(Tcl_Interp* interp, Tcl_Obj** obj, reg_entry* entry,
        reg_error* errPtr) {
    sqlite3* db = registry_db(interp, 0);
    if (db == NULL) {
        return 0;
    } else {
        sqlite3_stmt* stmt;
        char* query = "SELECT proc FROM entry_procs WHERE entry_id=?";
        if ((sqlite3_prepare(db, query, -1, &stmt, NULL) == SQLITE_OK)
                && (sqlite3_bind_int64(stmt, 1, entry->rowid) == SQLITE_OK)) {
            int r = sqlite3_step(stmt);
            char* name;
            switch (r) {
                case SQLITE_ROW:
                    name = sqlite3_column_text(stmt, 0);
                    *obj = Tcl_NewStringObj(name,
                            sqlite3_column_bytes(stmt, 0));
                    sqlite3_finalize(stmt);
                    return 1;
                case SQLITE_DONE:
                    name = unique_name(interp, "registry::entry");
                    sqlite3_finalize(stmt);
                    if (set_entry(interp, name, entry, errPtr)) {
                        /* insert record (not currently error-checked)
                         * TODO: check it */
                        sqlite3_prepare(db, "INSERT INTO entry_procs (entry_id,"
                                "proc) VALUES (?,?)", -1, &stmt, NULL);
                        sqlite3_bind_int64(stmt, 1, entry->rowid);
                        sqlite3_bind_text(stmt, 2, name, -1, SQLITE_STATIC);
                        sqlite3_step(stmt);
                        sqlite3_finalize(stmt);
                        *obj = Tcl_NewStringObj(name, -1);
                        free(name);
                        return 1;
                    }
                    free(name);
                    break;
                default:
                    reg_sqlite_error(db, errPtr, query);
                    break;
            }
        } else {
            reg_sqlite_error(db, errPtr, query);
        }
        sqlite3_finalize(stmt);
        return 0;
    }
}

/**
 * registry::entry delete ?entry ...?
 *
 * Deletes an entry from the registry (then closes it).
 *
 * TODO: ensure that other open instances of the entry are invalidated, because
 * they won't work once the entry is deleted.
 */
static int entry_delete(Tcl_Interp* interp, int objc, Tcl_Obj* CONST objv[]) {
    sqlite3* db = registry_db(interp, 1);
    if (db == NULL) {
        return TCL_ERROR;
    } else {
        reg_entry** entries;
        reg_error error;
        if (recast(interp, obj_to_entry, NULL, &entries, &(objv[2]), objc-2,
                    &error)) {
            if (reg_entry_delete(db, entries, objc-2, &error)) {
                free(entries);
                return TCL_OK;
            }
            free(entries);
        }
        return registry_failed(interp, &error);
    }
}

/**
 * registry::entry open portname version revision variants epoch ?name?
 *
 *
 *\
static int entry_open(Tcl_Interp* interp, int objc, Tcl_Obj* CONST objv[]) {
    sqlite3* db = registry_db(interp, 1);
    if (objc < 7 || objc > 8) {
        Tcl_WrongNumArgs(interp, 1, objv, "open portname version revision "
                "variants epoch ?name?");
        return TCL_ERROR;
    } else if (db == NULL) {
        return TCL_ERROR;
    } else {
        sqlite3_stmt* stmt;
        char* name = Tcl_GetString(objv[2]);
        char* version = Tcl_GetString(objv[3]);
        char* revision = Tcl_GetString(objv[4]);
        char* variants = Tcl_GetString(objv[5]);
        char* epoch = Tcl_GetString(objv[6]);
        char* query = "SELECT rowid FROM registry.ports WHERE "
            "name=? AND version=? AND revision=? AND variants=? AND epoch=?";
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
                    == SQLITE_OK)) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                sqlite_int64 entry = sqlite3_column_int64(stmt, 0);
                sqlite3_finalize(stmt);
                if (objc == 8) {
                    \* registry::entry open ... name *\
                    char* name = Tcl_GetString(objv[7]);
                    if (set_entry(interp, name, entry) == TCL_OK){
                        Tcl_SetObjResult(interp, objv[7]);
                        return TCL_OK;
                    }
                } else {
                    \* registry::entry open ... *\
                    char* name = unique_name(interp, "registry::entry");
                    if (set_entry(interp, name, entry) == TCL_OK) {
                        Tcl_Obj* res = Tcl_NewStringObj(name, -1);
                        Tcl_SetObjResult(interp, res);
                        free(name);
                        return TCL_OK;
                    }
                    free(name);
                }
            } else {
                Tcl_SetResult(interp, "entry not found", TCL_STATIC);
            }
        } else {
            set_sqlite_result(interp, db, query);
            sqlite3_finalize(stmt);
        }
    }
    return TCL_ERROR;
}

\*
 * registry::entry close ?entry ...?
 *
 * Closes an entry. It will remain in the registry until next time.
 */
static int entry_close(Tcl_Interp* interp, int objc, Tcl_Obj* CONST objv[]) {
    int i;
    for (i=2; i<objc; i++) {
        reg_error error;
        char* proc = Tcl_GetString(objv[i]);
        reg_entry* entry = get_entry(interp, proc, &error);
        if (entry == NULL) {
            return registry_failed(interp, &error);
        } else {
            Tcl_DeleteCommand(interp, proc);
        }
    }
    return TCL_OK;
}

/*
 * registry::entry search ?key value ...?
 *
 * Searches the registry for ports for which each key's value is equal to the
 * given value. To find all ports, call `entry search` with no key-value pairs.
 *
 * TODO: allow selection of -exact, -glob, and -regexp matching.
 */
static int entry_search(Tcl_Interp* interp, int objc, Tcl_Obj* CONST objv[]) {
    int i;
    sqlite3* db = registry_db(interp, 1);
    if (objc % 2 == 1) {
        Tcl_WrongNumArgs(interp, 2, objv, "search ?key value ...?");
        return TCL_ERROR;
    } else if (db == NULL) {
        return TCL_ERROR;
    } else {
        char** keys;
        char** vals;
        int key_count = objc/2 - 1;
        reg_entry** entries;
        reg_error error;
        int entry_count;
        /* ensure that valid search keys were used */
        for (i=2; i<objc; i+=2) {
            int index;
            if (Tcl_GetIndexFromObj(interp, objv[i], entry_props, "search key",
                        0, &index) != TCL_OK) {
                return TCL_ERROR;
            }
        }
        keys = malloc(key_count * sizeof(char*));
        vals = malloc(key_count * sizeof(char*));
        for (i=0; i<key_count; i+=2) {
            keys[i] = Tcl_GetString(objv[2*i+2]);
            vals[i] = Tcl_GetString(objv[2*i+3]);
        }
        entry_count = reg_entry_search(db, keys, vals, key_count, 0, &entries,
                &error);
        if (entry_count >= 0) {
            Tcl_Obj* resultObj;
            Tcl_Obj** objs;
            recast(interp, entry_to_obj, NULL, &objs, entries, entry_count,
                        &error);
            resultObj = Tcl_NewListObj(entry_count, objs);
            Tcl_SetObjResult(interp, resultObj);
            free(entries);
            return TCL_OK;
        }
        return registry_failed(interp, &error);
    }
}

/**
 * registry::entry exists name
 *
 * Note that this is <i>not</i> the same as entry_exists from registry1.0. This
 * simply checks if the given string is a valid entry object in the current
 * interp. No query to the database will be made.
 */
static int entry_exists(Tcl_Interp* interp, int objc, Tcl_Obj* CONST objv[]) {
    reg_error error;
    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 2, objv, "name");
        return TCL_ERROR;
    }
    if (get_entry(interp, Tcl_GetString(objv[2]), &error) == NULL) {
        reg_error_destruct(&error);
        Tcl_SetObjResult(interp, Tcl_NewBooleanObj(0));
    } else {
        Tcl_SetObjResult(interp, Tcl_NewBooleanObj(1));
    }
    return TCL_OK;
}

/**
 * registry::entry installed ?name? ?version?
 *
 * Returns a list of all installed ports. If `name` is specified, only returns
 * ports with that name, and if `version` is specified, only with that version.
 * Remember, the variants can still be different.
 *
 * TODO: add more arguments (epoch, revision, variants), maybe
 *\
static int entry_installed(Tcl_Interp* interp, int objc, Tcl_Obj* CONST objv[]){
    sqlite3* db = registry_db(interp, 1);
    if (objc > 4) {
        Tcl_WrongNumArgs(interp, 2, objv, "?name? ?version?");
        return TCL_ERROR;
    } else if (db == NULL) {
        return TCL_ERROR;
    } else {
        char* query;
        Tcl_Obj* result = Tcl_NewListObj(0, NULL);
        Tcl_SetObjResult(interp, result);
        if (objc == 2) {
            query = sqlite3_mprintf("SELECT rowid FROM registry.ports "
                    "WHERE (state == 'installed' OR state == 'active')");
        } else if (objc == 3) {
            char* name = Tcl_GetString(objv[2]);
            query = sqlite3_mprintf("SELECT rowid FROM registry.ports "
                    "WHERE (state == 'installed' OR state == 'active') "
                    "AND name='%q'", name);
        } else {
            char* name = Tcl_GetString(objv[2]);
            char* version = Tcl_GetString(objv[3]);
            query = sqlite3_mprintf("SELECT rowid FROM registry.ports "
                    "WHERE (state == 'installed' OR state == 'active') "
                    "AND name='%q' AND version='%q'", name, version);
        }
        if (all_objects(interp, db, query, "registry::entry", set_entry)
                == TCL_OK) {
            free(query);
            \* error if list length == 0 *\
            return TCL_OK;
        }
        free(query);
    }
    return TCL_ERROR;
}

\**
 * registry::entry active ?name?
 *
 * Returns a list of all active ports. If `name` is specified, only returns the
 * active port named, still in a list.
 *\
static int entry_active(Tcl_Interp* interp, int objc, Tcl_Obj* CONST objv[]) {
    sqlite3* db = registry_db(interp, 1);
    if (objc > 3) {
        Tcl_WrongNumArgs(interp, 2, objv, "?name?");
        return TCL_ERROR;
    } else if (db == NULL) {
        return TCL_ERROR;
    } else {
        char* query;
        if (objc == 2) {
            query = sqlite3_mprintf("SELECT rowid FROM registry.ports "
                    "WHERE state == 'active'");
        } else {
            char* name = Tcl_GetString(objv[2]);
            query = sqlite3_mprintf("SELECT rowid FROM registry.ports "
                    "WHERE state == 'active' AND name='%q'", name);
        }
        if (all_objects(interp, db, query, "registry::entry", set_entry)
                == TCL_OK) {
            free(query);
            \* error if list length == 0 *\
            return TCL_OK;
        }
        free(query);
    }
    return TCL_ERROR;
}

\*
static int entry_owner(Tcl_Interp* interp, int objc, Tcl_Obj* CONST objv[]) {
    sqlite3* db = registry_db(interp, 1);
    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 2, objv, "path");
        return TCL_ERROR;
    } else if (db == NULL) {
        return TCL_ERROR;
    } else {
        sqlite3_stmt* stmt;
        int len;
        char* path = Tcl_GetStringFromObj(objv[2], &len);
        char* query = "SELECT port_id FROM files WHERE path=?";
        if ((sqlite3_prepare(db, query, -1, &stmt, NULL) == SQLITE_OK)
                && (sqlite3_bind_text(stmt, 1, path, len, SQLITE_STATIC)
                    == SQLITE_OK)) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                sqlite_int64 rowid = sqlite3_column_int64(stmt, 0);
                Tcl_Obj* resultObj = Tcl_NewStringObj(result, len);
                Tcl_SetObjResult(interp, resultObj);
                sqlite3_finalize(stmt);
            } else {
                Tcl_Obj* resultObj = Tcl_NewStringObj("", 0);
                Tcl_SetObjResult(interp, resultObj);
                sqlite3_finalize(stmt);
            }
            return TCL_OK;
        } else {
            set_sqlite_result(interp, db, query);
            sqlite3_finalize(stmt);
        }
        return TCL_ERROR;
    }
}
*/

typedef struct {
    char* name;
    int (*function)(Tcl_Interp* interp, int objc, Tcl_Obj* CONST objv[]);
} entry_cmd_type;

static entry_cmd_type entry_cmds[] = {
    /* Global commands */
    { "create", entry_create },
    { "delete", entry_delete },
    /*
    { "open", entry_open },
    */
    { "close", entry_close },
    { "search", entry_search },
    { "exists", entry_exists },
    /*
    { "installed", entry_installed },
    { "active", entry_active },
    */
    { NULL, NULL }
};

/**
 * registry::entry cmd ?arg ...?
 *
 * Commands manipulating port entries in the registry. This could be called
 * `registry::port`, but that could be misleading, because `registry::item`
 * represents ports too, but not those in the registry.
 */
int entry_cmd(ClientData clientData UNUSED, Tcl_Interp* interp, int objc,
        Tcl_Obj* CONST objv[]) {
    int cmd_index;
    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "cmd ?arg ...?");
        return TCL_ERROR;
    }
    if (Tcl_GetIndexFromObjStruct(interp, objv[1], entry_cmds,
                sizeof(entry_cmd_type), "cmd", 0, &cmd_index) == TCL_OK) {
        entry_cmd_type* cmd = &entry_cmds[cmd_index];
        return cmd->function(interp, objc, objv);
    }
    return TCL_ERROR;
}
