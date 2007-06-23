/*
 * sql.c
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

#include <tcl.h>
#include <sqlite3.h>
#include <string.h>
#include <time.h>
#include <ctype.h>

#include "util.h"

/**
 * REGEXP function for sqlite3.
 *
 * Takes two arguments; the first is the value and the second the pattern. If
 * the pattern is invalid, errors out. Otherwise, returns true if the value
 * matches the pattern and false otherwise.
 *
 * This function is available in sqlite3 as the REGEXP operator.
 */
static void sql_regexp(sqlite3_context* context, int argc UNUSED,
        sqlite3_value** argv) {
    const char* value = sqlite3_value_text(argv[0]);
    const char* pattern = sqlite3_value_text(argv[1]);
    switch (Tcl_RegExpMatch(NULL, value, pattern)) {
        case 0:
            sqlite3_result_int(context, 0);
            break;
        case 1:
            sqlite3_result_int(context, 1);
            break;
        case -1:
            sqlite3_result_error(context, "invalid pattern", -1);
            break;
    }
}

/**
 * NOW function for sqlite3.
 *
 * Takes no arguments. Returns the unix timestamp of now.
 */
static void sql_now(sqlite3_context* context, int argc UNUSED,
        sqlite3_value** argv UNUSED) {
    sqlite3_result_int(context, time(NULL));
}

static int rpm_vercomp (const char *versionA, const char *versionB) {
	const char *ptrA, *ptrB;
	const char *eptrA, *eptrB;

	/* if versions equal, return zero */
	if(!strcmp(versionA, versionB))
		return 0;

	ptrA = versionA;
	ptrB = versionB;
	while (*ptrA != '\0' && *ptrB != '\0') {
		/* skip all non-alphanumeric characters */
		while (*ptrA != '\0' && !isalnum(*ptrA))
			ptrA++;
		while (*ptrB != '\0' && !isalnum(*ptrB))
			ptrB++;

		eptrA = ptrA;
		eptrB = ptrB;

		/* Somewhat arbitrary rules as per RPM's implementation.
		 * This code could be more clever, but we're aiming
		 * for clarity instead. */

		/* If versionB's segment is not a digit segment, but
		 * versionA's segment IS a digit segment, return 1.
		 * (Added for redhat compatibility. See redhat bugzilla
		 * #50977 for details) */
		if (!isdigit(*ptrB)) {
			if (isdigit(*ptrA))
				return 1;
		}

		/* Otherwise, if the segments are of different types,
		 * return -1 */

		if ((isdigit(*ptrA) && isalpha(*ptrB)) || (isalpha(*ptrA) && isdigit(*ptrB)))
			return -1;

		/* Find the first segment composed of entirely alphabetical
		 * or numeric members */
		if (isalpha(*ptrA)) {
			while (*eptrA != '\0' && isalpha(*eptrA))
				eptrA++;

			while (*eptrB != '\0' && isalpha(*eptrB))
				eptrB++;
		} else {
			int countA = 0, countB = 0;
			while (*eptrA != '\0' && isdigit(*eptrA)) {
				countA++;
				eptrA++;
			}
			while (*eptrB != '\0' && isdigit(*eptrB)) {
				countB++;
				eptrB++;
			}

			/* skip leading '0' characters */
			while (ptrA != eptrA && *ptrA == '0') {
				ptrA++;
				countA--;
			}
			while (ptrB != eptrB && *ptrB == '0') {
				ptrB++;
				countB--;
			}

			/* If A is longer than B, return 1 */
			if (countA > countB)
				return 1;

			/* If B is longer than A, return -1 */
			if (countB > countA)
				return -1;
		}
		/* Compare strings lexicographically */
		while (ptrA != eptrA && ptrB != eptrB && *ptrA == *ptrB) {
				ptrA++;
				ptrB++;
		}
		if (ptrA != eptrA && ptrB != eptrB)
			return *ptrA - *ptrB;

		ptrA = eptrA;
		ptrB = eptrB;
	}

	/* If both pointers are null, all alphanumeric
	 * characters were identical and only seperating
	 * characters differed. According to RPM, these
	 * version strings are equal */
	if (*ptrA == '\0' && *ptrB == '\0')
		return 0;

	/* If A has unchecked characters, return 1
	 * Otherwise, if B has remaining unchecked characters,
	 * return -1 */
	if (*ptrA != '\0')
		return 1;
	else
		return -1;
}

/**
 * VERSION collation for sqlite3.
 *
 * This function collates text according to pextlib's rpm-vercomp function. This
 * allows direct comparison and sorting of version columns, such as port.version
 * and port.revision.
 *
 * TODO: share rpm-vercomp properly with pextlib. Currently it's copy-pasted in.
 */
static int sql_version(void* userdata UNUSED, int alen UNUSED, const void* a,
        int blen UNUSED, const void* b) {
    /* I really hope that the strings are null-terminated. sqlite doesn't
     * describe this API well.
     */
    return rpm_vercomp((const char*)a, (const char*)b);
}

/**
 * Creates tables in the registry.
 *
 * This function is called upon an uninitialized database to create the tables
 * needed to record state between invocations of `port`.
 */
int create_tables(Tcl_Interp* interp, sqlite3* db) {
    static char* queries[] = {
        "BEGIN",

        /* metadata table */
        "CREATE TABLE registry.metadata (key UNIQUE, value)",
        "INSERT INTO registry.metadata (key, value) VALUES ('version', 1.000)",
        "INSERT INTO registry.metadata (key, value) VALUES ('created', NOW())",

        /* ports table */
        "CREATE TABLE registry.ports ("
            "name, portfile, url, location, epoch, version COLLATE VERSION, "
            "revision COLLATE VERSION, variants, state, date, "
            "UNIQUE (name, epoch, version, revision, variants), "
            "UNIQUE (url, epoch, version, revision, variants)"
            ")",
        "CREATE INDEX registry.port_name ON ports "
            "(name, epoch, version, revision, variants)",
        "CREATE INDEX registry.port_url ON ports "
            "(url, epoch, version, revision, variants)",
        "CREATE INDEX registry.port_state ON ports (state)",

        /* file map */
        "CREATE TABLE registry.files (port_id, path UNIQUE, mtime)",
        "CREATE INDEX registry.file_port ON files (port_id)",

        "END",
        NULL
    };
    return do_queries(interp, db, queries);
}

/**
 * Initializes database connection.
 *
 * This function creates all the temporary tables used by the registry. It also
 * registers the user functions and collations declared in "sql.h", making them
 * available.
 */
int init_db(Tcl_Interp* interp, sqlite3* db) {
    static char* queries[] = {
        "BEGIN",

        /* items cache */
        "CREATE TEMPORARY TABLE items (refcount, proc UNIQUE, name, url, path, "
            "worker, options, variants)",

        /* indexes list */
        "CREATE TEMPORARY TABLE indexes (file, name, attached)",

        /* entry => proc mapping */
        "CREATE TEMPORARY TABLE entry_procs (entry_id UNIQUE, proc UNIQUE)",

        "END",
        NULL
    };

    /* I'm not error-checking these. I don't think I need to. */
    sqlite3_create_function(db, "REGEXP", 2, SQLITE_UTF8, NULL, sql_regexp,
            NULL, NULL);
    sqlite3_create_function(db, "NOW", 0, SQLITE_ANY, NULL, sql_now, NULL,
            NULL);

    sqlite3_create_collation(db, "VERSION", SQLITE_UTF8, NULL, sql_version);

    return do_queries(interp, db, queries);
}

