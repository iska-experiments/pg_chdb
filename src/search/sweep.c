/*
 * The worker's startup sweep.
 *
 * The engine's directory, pg_chdb/pgsql_tmp/<dboid>/, is a cache: chDB's
 * metadata, the tables it says exist and the parts it remembers are
 * whatever the last engine saw, which a crash, a restore from backup, a
 * rewind or a promotion may leave older or newer than the index relations
 * whose pages hold the blobs. So the worker empties it before its first
 * request, and the engine starts with no tables; each index's table is
 * attached from the catalog when a request first names its generation
 * (attach.c), and the disk finds the parts in the pages. The directory and
 * socket of every database no longer in pg_database go too: a DROP
 * DATABASE whose session had not loaded the library left them.
 */

#include "postgres.h"

#include <sys/stat.h>
#include <unistd.h>

#include "access/htup_details.h"
#include "access/xact.h"
#include "catalog/pg_class.h"
#include "commands/defrem.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "utils/syscache.h"

#include "protocol.h"
#include "sweep.h"

/* The access method's name, as sql/chdb_search.sql will create it. */
#define CHDB_SEARCH_AM "chdb"

bool
chdb_search_is_index(Oid relid) {
    HeapTuple tup = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));

    if (!HeapTupleIsValid(tup)) {
        return false;
    }

    Form_pg_class cls = (Form_pg_class)GETSTRUCT(tup);
    bool ours         = cls->relkind == RELKIND_INDEX && OidIsValid(cls->relam) &&
                        cls->relam == get_am_oid(CHDB_SEARCH_AM, true);

    ReleaseSysCache(tup);
    return ours;
}

/* Removes the engine's directory, with the socket if `socket` too. */
static void
remove_engine_dir(Oid dboid, bool socket) {
    char path[MAXPGPATH];
    struct stat st;

    snprintf(path, sizeof(path), CHDB_SEARCH_ENGINE_DIR_FMT, dboid);
    if (stat(path, &st) == 0 && !rmtree(path, true)) {
        ereport(
            WARNING,
            errmsg("chdb_search: could not remove engine directory \"%s\"", path)
        );
    }
    if (socket) {
        snprintf(path, sizeof(path), CHDB_SEARCH_SOCKET_FMT, dboid);
        unlink(path);
    }
}

void
chdb_search_remove_store_dir(Oid dboid) {
    remove_engine_dir(dboid, true);
}

void
chdb_search_empty_engine_dir(Oid dboid) {
    remove_engine_dir(dboid, false);
}

/*
 * The engine directory and socket of every database no longer in
 * pg_database go: a DROP DATABASE whose session had not loaded the library
 * left them. Our own database's and entries that are not <dboid> or
 * <dboid>.sock stay.
 */
static void
sweep_orphan_dirs(Oid own) {
    DIR* dir = AllocateDir(CHDB_SEARCH_CACHE_DIR);
    struct dirent* de;
    List* gone = NIL;
    ListCell* lc;

    if (!dir) {
        return; /* nothing created yet */
    }
    while ((de = ReadDir(dir, CHDB_SEARCH_CACHE_DIR)) != NULL) {
        char* end;
        unsigned long dboid = strtoul(de->d_name, &end, 10);

        if (end == de->d_name || (*end != '\0' && strcmp(end, ".sock") != 0)) {
            continue;
        }
        if ((Oid)dboid != own) {
            gone = list_append_unique_oid(gone, (Oid)dboid);
        }
    }
    FreeDir(dir);

    foreach (lc, gone) {
        Oid dboid = lfirst_oid(lc);

        if (!SearchSysCacheExists1(DATABASEOID, ObjectIdGetDatum(dboid))) {
            ereport(
                LOG, errmsg("chdb_search: sweeping store of dropped database %u", dboid)
            );
            chdb_search_remove_store_dir(dboid);
        }
    }
}

/* The catalog lookups need a transaction. */
void
chdb_search_sweep(Oid dboid) {
    StartTransactionCommand();
    sweep_orphan_dirs(dboid);
    CommitTransactionCommand();
    remove_engine_dir(dboid, false);
}
