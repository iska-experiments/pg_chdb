/*
 * Phrase search with slop in ClickHouse. The text index stores positions
 * only with support_phrase_search, and hasPhrase knows no slop, so a phrase
 * whose tokens may be some others apart is answered from the tokens of the
 * text at query time: hasAllTokens(col, needle) narrows the rows through
 * the index, then the positions decide.
 *
 * With T the tokens of the column and N those of the needle, both through
 * the index's tokenizer and preprocessor so that they agree with it:
 *
 *   starts     = positions p in T with T[p] = N[1]
 *   end(i)     = arrayFold over N[2..] from i: the first position after the
 *                accumulator holding the next token, or 0 once one is missing
 *   match      = exists a start i with end(i) > 0 and
 *                (end(i) - i + 1) - length(N) <= slop
 *
 * The fold takes the nearest next occurrence of each token, which is the
 * shortest span from that start, so the needle's tokens must appear in
 * order and the slop is the number of other tokens between the first and
 * the last of them. A needle of one token matches wherever it appears, one
 * without tokens matches nothing, as hasAllTokens does. The whole is
 * parenthesized, as it may follow a NOT.
 */

#include "postgres.h"

#include "query.h"
#include "search.h"

void
chdb_search_append_slop_phrase(
    StringInfo buf,
    const ChdbColumn* column,
    const char* needle,
    int32 slop
) {
    StringInfoData lit;
    char *T, *N, *starts;

    initStringInfo(&lit);
    chdb_search_append_string(&lit, needle);
    T = chdb_search_tokens_call(column, column->name, false);
    N = chdb_search_tokens_call(column, lit.data, true);
    starts =
        psprintf("arrayFilter((p, x) -> x = %s[1], arrayEnumerate(%s), %s)", N, T, T);
    appendStringInfo(
        buf,
        "(hasAllTokens(%s, %s) AND arrayExists(d -> d >= 0 AND d + 1 - length(%s) "
        "<= %d, arrayMap(i -> toInt64(arrayFold((acc, t) -> if(acc = 0, toUInt32(0), "
        "arrayFirst(p -> p > acc, arrayFilter((p, x) -> x = t, arrayEnumerate(%s), "
        "%s))), arraySlice(%s, 2), i)) - toInt64(i), %s)))",
        column->name,
        lit.data,
        N,
        slop,
        T,
        T,
        N,
        starts
    );
}
