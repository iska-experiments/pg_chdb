#ifndef CHDB_SEARCH_PAGESTORE_ROUTES_H
#define CHDB_SEARCH_PAGESTORE_ROUTES_H

/*
 * Which relation holds a blob (routes.c). The engine names a storage
 * pg_<indexoid> and a key g<generation>/..., the generation being the key
 * prefix of the store table's disk (ddl.c); the worker learns from every
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

/* Every index noted: the storages the engine registers after a restart. */
extern void
chdb_routes_storages(chdbBlobNameSink sink, void* ud);

/* Clears the dirty flag of every relation this worker took pages in. */
extern void
chdb_routes_clean(void);

#endif /* CHDB_SEARCH_PAGESTORE_ROUTES_H */
