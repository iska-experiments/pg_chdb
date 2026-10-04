#ifndef CHDB_SEARCH_PAGESTORE_ROUTES_H
#define CHDB_SEARCH_PAGESTORE_ROUTES_H

/*
 * Which relation holds a blob (routes.c). The engine names a storage
 * pg_<indexoid> and a key under the prefix of the table's disk (ddl.c),
 * g<generation>/ or s<generation>_tx_<fxid>/ for a staging table, which
 * both name the generation; the worker learns from every
 * request which relation holds which generation, reading the generation
 * off the relation's metapage, since a build's relation is in no catalog
 * the worker can see. A pair it has not been told of is an empty storage:
 * the generation of a rebuild that rolled back, whose relation is gone.
 */

#include "postgres.h"

#include "storage/relfilelocator.h"

#include "store.h"

extern void
chdb_routes_init(void);

/* A request named this relation of the index; its generation is read off it. */
extern void
chdb_routes_note(Oid index, RelFileLocator loc);

/* Before the index's database is dropped: its blobs go with its relations. */
extern void
chdb_routes_forget(Oid index);

/*
 * The relation for a storage and a key, or false for a pair not noted.
 * Raises for a storage name or key that is not of the shape above.
 */
extern bool
chdb_routes_find(const char* storage, const char* key, RelFileLocator* loc);

/*
 * For a relation found gone: whether the catalog's index holds the key's
 * generation in another relation, as ALTER INDEX SET TABLESPACE leaves it,
 * which is then the route. False, and the storage is empty for good, when
 * the index or the generation is gone, or when asked inside a transaction.
 */
extern bool
chdb_routes_refresh(const char* storage, const char* key, RelFileLocator* loc);

/* Every index noted: the storages the engine registers after a restart. */
extern void
chdb_routes_storages(chdbBlobNameSink sink, void* ud);

/* Clears the dirty flag of every relation this worker took pages in. */
extern void
chdb_routes_clean(void);

#endif /* CHDB_SEARCH_PAGESTORE_ROUTES_H */
