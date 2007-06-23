/*
 * util.c
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

#include "util.h"

/**
 * Generates a unique proc name starting with prefix.
 *
 * This function loops through the integers trying to find a name
 * "<prefix><int>" such that no command with that name exists within the given
 * Tcl interp context. This behavior is similar to that of the builtin
 * `interp create` command, and is intended to generate names for created
 * objects of a similar nature.
 *
 * TODO: add a int* parameter so that functions which need large numbers of
 * unique names can keep track of the lower bound between calls,thereby turning
 * N^2 to N. It'll be alchemy for the 21st century.
 */
char* unique_name(Tcl_Interp* interp, char* prefix) {
    char* result = malloc(strlen(prefix) + TCL_INTEGER_SPACE + 1);
    Tcl_CmdInfo info;
    int i;
    for (i=0; ; i++) {
        sprintf(result, "%s%d", prefix, i);
        if (Tcl_GetCommandInfo(interp, result, &info) == 0) {
            break;
        }
    }
    return result;
}

/**
 * Parses flags given to a Tcl command.
 *
 * Starting at `objv[start]`, this function will loop through the remaining
 * arguments until a non-flag argument is found, or an END_FLAGS flag is found,
 * or an invalid flag is found. In the first two cases, TCL_OK will be returned
 * and `start` will be moved to the first non-flag argument; in the third,
 * TCL_ERROR will be returned.
 *
 * It is recommended that all callers of this function include the entry
 * `{ "--", END_FLAGS }` in the NULL-terminated list `options`. For other,
 * non-zero flag values in `options`, flags will be bitwise or'ed by that value.
 *
 * Note that `alpha -beta gamma -delta epsilon` will be recognized as three
 * arguments following one flag. This could be changed but would make things
 * much more difficult.
 *
 * TODO: support flags of the form ?-flag value?. No functions currently have a
 * use for this yet, so it's not a priority, but it should be there for
 * completeness.
 */
int parse_flags(Tcl_Interp* interp, int objc, Tcl_Obj* CONST objv[], int* start,
        option_spec options[], int* flags) {
    int i;
    int index;
    *flags = 0;
    for (i=*start; i<objc; i++) {
        if (Tcl_GetString(objv[i])[0] != '-') {
            break;
        }
        if (Tcl_GetIndexFromObjStruct(interp, objv[i], options,
                    sizeof(option_spec), "option", 0, &index) == TCL_OK) {
            if (options[index].flag == END_FLAGS) {
                i++;
                break;
            } else {
                *flags |= options[index].flag;
            }
        } else {
            return TCL_ERROR;
        }
    }
    *start = i;
    return TCL_OK;
}

/**
 * Retrieves the object whose proc is named by `name`.
 *
 * A common design pattern is to have an object be a proc whose clientData
 * points to the object and whose function points to an object function. This
 * function retrieves such an object.
 *
 * `proc` is used to verify that a proc names an instance of the object. If not,
 * `type` is used to construct an appropriate error message before it returns
 * NULL.
 */
void* get_object(Tcl_Interp* interp, char* name, char* type,
        Tcl_ObjCmdProc* proc, reg_error* errPtr) {
    Tcl_CmdInfo info;
    if (Tcl_GetCommandInfo(interp, name, &info) && info.objProc == proc){
        return info.objClientData;
    } else {
        errPtr->code = "registry::not-found";
        errPtr->description = sqlite3_mprintf("could not find %s \"%s\"", type,
                name);
        errPtr->free = sqlite3_free;
        return NULL;
    }
}

/**
 * Sets the object whose proc is named by `name`.
 *
 * See the documentation for `get_object`. This function registers such an
 * object, and additionally requires the `deleteProc` argument, which will be
 * used to free the object.
 *
 * TODO: cause the error used here not to leak memory. This probably needs to be
 *       addressed as a generic "reg_error_free" routine
 */
int set_object(Tcl_Interp* interp, char* name, void* value, char* type,
        Tcl_ObjCmdProc* proc, Tcl_CmdDeleteProc* deleteProc, reg_error* errPtr){
    Tcl_CmdInfo info;
    Tcl_GetCommandInfo(interp, name, &info);
    if (info.objProc == proc) {
        errPtr->code = "registry::duplicate-object";
        errPtr->description = sqlite3_mprintf("%s named \"%s\" already exists, "
                "cannot create", type, name);
        errPtr->free = sqlite3_free;
        return 0;
    }
    Tcl_CreateObjCommand(interp, name, proc, value, deleteProc);
    return 1;
}

/**
 * Executes a null-terminated list of queries.
 *
 * Pass it a list of queries, it'll execute them. This is mainly intended for
 * initialization, when you have a number of standard queries to execute.
 */
int do_queries(Tcl_Interp* interp, sqlite3* db, char** queries) {
    char** query;
    for (query = queries; *query != NULL; query++) {
        sqlite3_stmt* stmt;
        if ((sqlite3_prepare(db, *query, -1, &stmt, NULL) != SQLITE_OK)
                || (sqlite3_step(stmt) != SQLITE_DONE)) {
            set_sqlite_result(interp, db, *query);
            sqlite3_finalize(stmt);
            return TCL_ERROR;
        }
        sqlite3_finalize(stmt);
    }
    return TCL_OK;
}

/**
 * Reports a sqlite3 error to Tcl.
 *
 * Queries the database for the most recent error message and sets it as the
 * result of the given interpreter. If a query is optionally passed, also
 * records what it was.
 */
void set_sqlite_result(Tcl_Interp* interp, sqlite3* db, const char* query) {
    Tcl_ResetResult(interp);
    if (query == NULL) {
        Tcl_AppendResult(interp, "sqlite error: ", sqlite3_errmsg(db), NULL);
    } else {
        Tcl_AppendResult(interp, "sqlite error executing \"", query, "\": ",
                sqlite3_errmsg(db), NULL);
    }
}

/**
 * Sets the result of the interpreter to all objects returned by a query.
 *
 * This function executes `query` on `db` It expects that the query will return
 * records of a single column, `rowid`. It will then use `prefix` to construct
 * unique names for these records, and call `setter` to construct their proc
 * objects. The result of `interp` will be set to a list of all such objects.
 *
 * If TCL_OK is returned, then a list is in the result. If TCL_ERROR is, then an
 * error is there.
 */
int all_objects(Tcl_Interp* interp, sqlite3* db, char* query, char* prefix,
        set_object_function* setter) {
    sqlite3_stmt* stmt;
    if (sqlite3_prepare(db, query, -1, &stmt, NULL) == SQLITE_OK) {
        Tcl_Obj* result = Tcl_NewListObj(0, NULL);
        Tcl_SetObjResult(interp, result);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            sqlite_int64 rowid = sqlite3_column_int64(stmt, 0);
            char* name = unique_name(interp, prefix);
            if (setter(interp, name, rowid) == TCL_OK) {
                Tcl_Obj* element = Tcl_NewStringObj(name, -1);
                Tcl_ListObjAppendElement(interp, result, element);
                free(name);
            } else {
                free(name);
                return TCL_ERROR;
            }
        }
        return TCL_OK;
    } else {
        sqlite3_free(query);
        set_sqlite_result(interp, db, query);
        return TCL_ERROR;
    }
    return TCL_ERROR;
}

int recast(void* userdata, cast_function* fn, free_function* del, void*** outv,
        void** inv, int inc, reg_error* errPtr) {
    void** result = malloc(inc*sizeof(void*));
    int i;
    for (i=0; i<inc; i++) {
        if (!fn(userdata, &result[i], inv[i], errPtr)) {
            if (del != NULL) {
                del(userdata, result, i);
            }
            free(result);
            return 0;
        }
    }
    *outv = result;
    return 1;
}
