/*
 * registry.c
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

#include <stdio.h>
#include <unistd.h>
#include <tcl.h>
#include <sqlite3.h>

#include "graph.h"
#include "item.h"
#include "entry.h"
#include "util.h"
#include "sql.h"

/**
 * Deletes the sqlite3 DB associated with interp.
 *
 * This function will close an interp's associated DB, although there doesn't
 * seem to be a way of verifying that it happened properly. This will be a
 * problem if we get lazy and forget to finalize a sqlite3_stmt somewhere, so
 * this function will be noisy and complain if we do.
 *
 * Then it will leak memory :(
 */
static void delete_db(ClientData db, Tcl_Interp* interp UNUSED) {
    if (sqlite3_close((sqlite3*)db) != SQLITE_OK) {
        fprintf(stderr, "error: registry db not closed correctly (%s)\n",
                sqlite3_errmsg((sqlite3*)db));
    }
}

/**
 * Returns the sqlite3 DB associated with interp.
 *
 * The registry keeps its state in a sqlite3 database that is keyed to the
 * current interpreter context. Different interps will have different instances
 * of the connection, although I don't know if the Apple-provided sqlite3 lib
 * was compiled with thread-safety, so I can't be certain that it's safe to use
 * the registry from multiple threads. I'm pretty sure it's unsafe to alias a
 * registry function into a different thread.
 *
 * If `attached` is set to true, then this function will additionally check if
 * a real registry database has been attached. If not, then it will return NULL.
 *
 * This function sets its own Tcl result.
 */
sqlite3* registry_db(Tcl_Interp* interp, int attached) {
    sqlite3* db = Tcl_GetAssocData(interp, "registry::db", NULL);
    if (db == NULL) {
        if (sqlite3_open(NULL, &db) == SQLITE_OK) {
            if (init_db(interp, db) == TCL_OK) {
                Tcl_SetAssocData(interp, "registry::db", delete_db, db);
            } else {
                sqlite3_close(db);
                db = NULL;
            }
        } else {
            set_sqlite_result(interp, db, NULL);
        }
    }
    if (attached) {
        if (!Tcl_GetAssocData(interp, "registry::attached", NULL)) {
            Tcl_SetResult(interp, "registry is not open", TCL_STATIC);
            db = NULL;
        }
    }
    return db;
}

static int registry_open(ClientData clientData UNUSED, Tcl_Interp* interp,
        int objc, Tcl_Obj* CONST objv[]) {
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "db-file");
        return TCL_ERROR;
    } else {
        int needsInit = (Tcl_FSAccess(objv[1], F_OK) != 0);
        char* file = Tcl_GetString(objv[1]);
        /*
         * If registry needs initialization, we need write access. However, it
         * seems you can't test W_OK for a file that doesn't yet exist. We need
         * a better way to test if a location is writable, then. Maybe strip off
         * the last path component?
         */
        /* if (!needsInit || (Tcl_FSAccess(objv[1], W_OK) == 0)) { */
        if (1) {
            sqlite3* db = registry_db(interp, 0);
            sqlite3_stmt* stmt;
            char* query = sqlite3_mprintf("ATTACH DATABASE '%q' AS registry",
                    file);
            if ((sqlite3_prepare(db, query, -1, &stmt, NULL) == SQLITE_OK)
                    && (sqlite3_step(stmt) == SQLITE_DONE)) {
                if (!needsInit || (create_tables(interp, db) == TCL_OK)) {
                    Tcl_SetAssocData(interp, "registry::attached", NULL,
                            (void*)1);
                    return TCL_OK;
                }
            } else {
                set_sqlite_result(interp, db, query);
            }
        } else {
            Tcl_ResetResult(interp);
            Tcl_AppendResult(interp, "port registry doesn't exist at ", file,
                    " and couldn't write layout to this location", NULL);
        }
    }
    return TCL_ERROR;
}

static int registry_close(ClientData clientData UNUSED, Tcl_Interp* interp,
        int objc, Tcl_Obj* CONST objv[]) {
    if (objc != 1) {
        Tcl_WrongNumArgs(interp, 1, objv, NULL);
        return TCL_ERROR;
    } else {
        sqlite3* db = registry_db(interp, 1);
        if (db == NULL) {
            Tcl_SetResult(interp, "registry is not open", TCL_STATIC);
        } else {
            sqlite3_stmt* stmt;
            char* query = "DETACH DATABASE registry";
            if ((sqlite3_prepare(db, query, -1, &stmt, NULL) == SQLITE_OK)
                    && (sqlite3_step(stmt) == SQLITE_DONE)) {
                sqlite3_finalize(stmt);
                Tcl_SetAssocData(interp, "registry::attached", NULL, (void*)0);
                return TCL_OK;
            } else {
                set_sqlite_result(interp, db, query);
                sqlite3_finalize(stmt);
            }
        }
    }
    return TCL_ERROR;
}

/**
 * Initializer for the registry lib.
 *
 * This function is called automatically by Tcl upon loading of registry.dylib.
 * It creates the global commands made available in the registry namespace.
 */
int Registry_Init(Tcl_Interp* interp) {
    if (Tcl_InitStubs(interp, "8.3", 0) == NULL) {
        return TCL_ERROR;
    }
    Tcl_CreateObjCommand(interp, "registry::open", registry_open, NULL,
            NULL);
    Tcl_CreateObjCommand(interp, "registry::close", registry_close, NULL,
            NULL);
    /* Tcl_CreateObjCommand(interp, "registry::graph", GraphCmd, NULL, NULL); */
    /* Tcl_CreateObjCommand(interp, "registry::item", item_cmd, NULL, NULL); */
    Tcl_CreateObjCommand(interp, "registry::entry", entry_cmd, NULL, NULL);
    if (Tcl_PkgProvide(interp, "registry", "2.0") != TCL_OK) {
        return TCL_ERROR;
    }
    return TCL_OK;
}
