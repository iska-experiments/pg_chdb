#ifndef CHDB_SEARCH_STUB_H
#define CHDB_SEARCH_STUB_H

/*
 * The stub worker client's two halves: client_stub.c fakes the connection
 * and declares the GUCs, stub_answers.c encodes what a statement is answered
 * with. Linked with CHDB_SEARCH_STUB=1 only.
 */

#include "postgres.h"

/* The GUCs, defined in client_stub.c and documented there. */
extern char* chdb_search_stub_ctids;
extern char* chdb_search_stub_tokens;
extern char* chdb_search_stub_frequencies;

/*
 * The Native block a select is answered with, palloc'd into *out, and its
 * length: zero bytes for no block at all. The statement decides: tokens()
 * gets the tokens GUC, a count() the ctids' number, or a token's frequency
 * when its WHERE asks for one as the score does, anything else the ctids.
 */
extern size_t
chdb_stub_answer(const char* sql, Oid indexoid, void** out);

#endif /* CHDB_SEARCH_STUB_H */
