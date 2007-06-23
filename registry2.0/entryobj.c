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

#include "entryobj.h"
#include "registry.h"
#include "util.h"

const char* entry_props[] = {
    "name",
    "portfile",
    "url",
    "location",
    "epoch",
    "version",
    "revision",
    "variants",
    "date",
    "state",
    NULL
};

/* ${entry} prop name ?value? */
static int entry_obj_prop(Tcl_Interp* interp, entry_t* entry, int objc,
        Tcl_Obj* CONST objv[]) {
    int index;
    if (objc == 2) {
        /* ${entry} prop name; return the current value */
        if (Tcl_GetIndexFromObj(interp, objv[1], entry_props, "prop", 0, &index)
                == TCL_OK) {
            sqlite3_stmt* stmt;
            char* prop = Tcl_GetString(objv[1]);
            char* query = sqlite3_mprintf("SELECT %s FROM registry.ports WHERE "
                    "rowid='%lld'", prop, entry->rowid);
            if ((sqlite3_prepare(entry->db, query, -1, &stmt, NULL)
                        == SQLITE_OK)
                    && (sqlite3_step(stmt) == SQLITE_ROW)) {
                /* eliminate compiler warning about signedness */
                const char* result = sqlite3_column_text(stmt, 0);
                int len = sqlite3_column_bytes(stmt, 0);
                Tcl_Obj* resultObj = Tcl_NewStringObj(result, len);
                Tcl_SetObjResult(interp, resultObj);
                sqlite3_finalize(stmt);
                return TCL_OK;
            } else {
                set_sqlite_result(interp, entry->db, query);
                sqlite3_finalize(stmt);
            }
        }
    } else if (objc == 3) {
        /* ${entry} prop name value; set a new value */
        if (Tcl_GetIndexFromObj(interp, objv[1], entry_props, "prop", 0, &index)
                == TCL_OK) {
            sqlite3_stmt* stmt;
            char* prop = Tcl_GetString(objv[1]);
            char* value = Tcl_GetString(objv[2]);
            char* query = sqlite3_mprintf("UPDATE registry.ports SET %s='%q' "
                    "WHERE rowid='%lld'", prop, value, entry->rowid);
            if ((sqlite3_prepare(entry->db, query, -1, &stmt, NULL)
                        == SQLITE_OK)
                    && (sqlite3_step(stmt) == SQLITE_DONE)) {
                sqlite3_finalize(stmt);
                return TCL_OK;
            } else {
                set_sqlite_result(interp, entry->db, query);
                sqlite3_finalize(stmt);
            }
        }
    } else {
        Tcl_WrongNumArgs(interp, 2, objv, "?value?");
    }
    return TCL_ERROR;
}

/*
 * ${entry} map ?file ...?
 *
 * Maps the listed files to the port represented by ${entry}. This will throw an
 * error if a file is mapped to an already-existing file, but not a very
 * descriptive one.
 *
 * TODO: more descriptive error on duplicated file
 */
static int entry_obj_map(Tcl_Interp* interp, entry_t* entry, int objc,
        Tcl_Obj* CONST objv[]) {
    sqlite3_stmt* stmt;
    char* query = "INSERT INTO files (port_id, path) VALUES (?, ?)";
    /* BEGIN */
    if ((sqlite3_prepare(entry->db, query, -1, &stmt, NULL) == SQLITE_OK)
            && (sqlite3_bind_int(stmt, 1, entry->rowid) == SQLITE_OK)) {
        int i;
        for (i=2; i<objc; i++) {
            int len;
            char* path = Tcl_GetStringFromObj(objv[i], &len);
            if ((sqlite3_bind_text(stmt, 2, path, len, SQLITE_STATIC)
                        != SQLITE_OK)
                    || (sqlite3_step(stmt) != SQLITE_DONE)) {
                set_sqlite_result(interp, entry->db, query);
                sqlite3_finalize(stmt);
                /* END or ROLLBACK? */
                return TCL_ERROR;
            }
            sqlite3_reset(stmt);
        }
        sqlite3_finalize(stmt);
        /* END */
        return TCL_OK;
    } else {
        set_sqlite_result(interp, entry->db, query);
        sqlite3_finalize(stmt);
        /* END */
        return TCL_ERROR;
    }
}

/*
 * ${entry} unmap ?file ...?
 *
 * Unmaps the listed files from the given port. Will throw an error if a file
 * that is not mapped to the port is attempted to be unmapped.
 */
static int entry_obj_unmap(Tcl_Interp* interp, entry_t* entry, int objc,
        Tcl_Obj* CONST objv[]) {
    sqlite3_stmt* stmt;
    char* query = "DELETE FROM files WHERE port_id=? AND path=?";
    /* BEGIN */
    if (sqlite3_prepare(entry->db, query, -1, &stmt, NULL) == SQLITE_OK) {
        int i;
        for (i=2; i<objc; i++) {
            int len;
            char* path = Tcl_GetStringFromObj(objv[i], &len);
            if ((sqlite3_bind_int(stmt, 1, entry->rowid) != SQLITE_OK)
                    || (sqlite3_bind_text(stmt, 2, path, len, SQLITE_STATIC)
                        != SQLITE_OK)
                    || (sqlite3_step(stmt) != SQLITE_DONE)) {
                if (sqlite3_reset(stmt) == SQLITE_CONSTRAINT) {
                    Tcl_AppendResult(interp, "an existing port owns \"", path,
                            "\"", NULL);
                } else {
                    set_sqlite_result(interp, entry->db, query);
                }
                sqlite3_finalize(stmt);
                /* END or ROLLBACK? */
                return TCL_ERROR;
            }
            if (sqlite3_changes(entry->db) == 0) {
                Tcl_AppendResult(interp, Tcl_GetString(objv[i]), " is not "
                        "mapped to this entry", NULL);
                sqlite3_finalize(stmt);
                /* END or ROLLBACK? */
                return TCL_ERROR;
            }
            sqlite3_reset(stmt);
        }
        sqlite3_finalize(stmt);
        /* END */
        return TCL_OK;
    } else {
        set_sqlite_result(interp, entry->db, query);
        sqlite3_finalize(stmt);
        /* END */
        return TCL_ERROR;
    }
}

static int entry_obj_files(Tcl_Interp* interp, entry_t* entry, int objc,
        Tcl_Obj* CONST objv[]) {
    sqlite3_stmt* stmt;
    char* query = "SELECT path FROM files WHERE port_id=?";
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "files");
        return TCL_ERROR;
    }
    if ((sqlite3_prepare(entry->db, query, -1, &stmt, NULL) == SQLITE_OK)
            && (sqlite3_bind_int64(stmt, 1, entry->rowid) == SQLITE_OK)) {
        Tcl_Obj* result = Tcl_NewListObj(0, NULL);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            char* value = sqlite3_column_text(stmt, 0);
            int len = sqlite3_column_bytes(stmt, 0);
            Tcl_Obj* element = Tcl_NewStringObj(value, len);
            Tcl_ListObjAppendElement(interp, result, element);
        }
        sqlite3_finalize(stmt);
        Tcl_SetObjResult(interp, result);
        return TCL_OK;
    } else {
        set_sqlite_result(interp, entry->db, query);
        sqlite3_finalize(stmt);
        return TCL_ERROR;
    }
}

typedef struct {
    char* name;
    int (*function)(Tcl_Interp* interp, entry_t* entry, int objc,
            Tcl_Obj* CONST objv[]);
} entry_obj_cmd_type;

static entry_obj_cmd_type entry_cmds[] = {
    { "name", entry_obj_prop },
    { "portfile", entry_obj_prop },
    { "url", entry_obj_prop },
    { "location", entry_obj_prop },
    { "epoch", entry_obj_prop },
    { "version", entry_obj_prop },
    { "revision", entry_obj_prop },
    { "variants", entry_obj_prop },
    { "date", entry_obj_prop },
    { "state", entry_obj_prop },
    { "map", entry_obj_map },
    { "unmap", entry_obj_unmap },
    { "files", entry_obj_files },
    { NULL, NULL }
};

/* ${entry} cmd ?arg ...? */
/* This function implements the command that will be called when an entry created
 * by `registry::entry` is used as a procedure. Since all data is kept in a
 * temporary sqlite3 database that is created for the current interpreter, none
 * of the sqlite3 functions used have any error checking. That should be a safe
 * assumption, since nothing outside of registry:: should ever have the chance
 * to touch it.
 */
int entry_obj_cmd(ClientData clientData, Tcl_Interp* interp, int objc,
        Tcl_Obj* CONST objv[]) {
    int cmd_index;
    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "cmd ?arg ...?");
        return TCL_ERROR;
    }
    if (Tcl_GetIndexFromObjStruct(interp, objv[1], entry_cmds,
                sizeof(entry_obj_cmd_type), "cmd", 0, &cmd_index) == TCL_OK) {
        entry_obj_cmd_type* cmd = &entry_cmds[cmd_index];
        return cmd->function(interp, (entry_t*)clientData, objc, objv);
    }
    return TCL_ERROR;
}
