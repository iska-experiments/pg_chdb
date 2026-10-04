#ifndef CHDB_HELPER_H
#define CHDB_HELPER_H

#include "postgres.h"

#include "lib/stringinfo.h"

#include "channel.h"
#include "setup.h"

/*
 * The chdb_helper process answering one COPY. The two sides meet on fixed
 * descriptors:
 *
 *   fd 0   Native blocks in, INSERT, COPY TO
 *   fd 1   Native blocks out, SELECT, COPY FROM
 *   fd 2   error text
 *   fd 3   setup payload, closed once written
 *   exit   0 on success, CHDB_HELPER_LOST_BACKEND when the channel broke and
 *          there was nobody left to tell, else nonzero with fd 2 saying why
 *
 * Row counts never cross: the Postgres side counts what it scanned or stored.
 */

/*
 * Starts the helper on `query`, bound to `nparams` named parameters. The helper
 * dies with the backend.
 */
extern chdbChannel*
chdb_helper_start(
    chdbHelperContext* ctx,
    const char* query,
    char* const* names,
    char* const* values,
    size_t nparams
);

/* Ends the stream and waits for the helper, raising unless it exited cleanly. */
extern void
chdb_helper_finish(chdbChannel* helper);

/* Appends the setup payload of setup.h for `query` and its parameters to `buf`. */
extern void
chdb_helper_build_setup(
    StringInfo buf,
    chdbHelperContext* ctx,
    const char* query,
    char* const* names,
    char* const* values,
    size_t nparams
);

#endif /* CHDB_HELPER_H */
