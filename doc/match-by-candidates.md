# MatchByCandidates search hint

`MatchByCandidates` is an opt-in execution hint for queries that combine
`MATCH()` with a selective attribute or document-ID predicate. It asks the
engine to identify candidate rows first and restrict full-text posting work to
those rows. Matching, positions, ranking, residual filters, sorting, and result
limits still use the normal search machinery. Unhinted queries retain their
existing execution path. The change requires no reindexing or on-disk format
change.

The implementation was developed from upstream commit
`df6fc413f8ba800e26272dcf57dcdd5469bc92c6` and committed as
`ff5cb3c33eeddb581a537c5116ceec202fa34d41` on
`feature/MatchByCandidates`. This document describes that feature commit;
local RPM packaging and version-stamping edits are separate.

## SQL and intended use

Place the argument-free hint in a SQL hint comment on the `SELECT` statement:

```sql
SELECT id, title
FROM subtitles
WHERE MATCH('"what s going on up"~1')
  AND uploaderid IN ('channel1', 'channel2')
OPTION ranker=none /*+ MatchByCandidates */;

SELECT id FROM subtitles
WHERE id IN (101, 205, 309) AND MATCH('common terms')
/*+ MatchByCandidates DocidIndex(id) */;
```

The second example combines it with an existing access-path hint. There is no
`MatchByCandidates(...)` syntax; the row limit is a daemon setting. The hint
is useful when an indexed predicate or ID lookup yields few rows while common
terms, a conjunction, or a positional `MATCH()` expression would otherwise
visit many postings. Benchmark both forms for the actual query and data.
Broad predicates can be slower because candidate collection is attempted
before ordinary matching. A query without `MATCH()` has no posting work to
restrict. Joins and KNN queries retain their normal execution path.

## Execution and correctness decisions

1. The existing disk planner selects a candidate-producing ID lookup,
   secondary-index iterator, or columnar analyzer. The hint retains an
   available candidate plan even when the normal full-text-first cost
   comparison would reject it; it does not invent an index or force every
   indexed predicate into the plan.
2. Each disk chunk buffers the producer's sorted local row IDs, up to its
   configured limit. The producer is still replayed through the existing outer
   intersection. Candidate-aware posting leaves skip work *inside* that
   intersection. Result `LIMIT`, `max_matches`, and `cutoff` do not truncate
   candidate collection: later candidates may be the first to satisfy MATCH.
3. Each posting leaf owns an independent forward cursor over the completed
   candidate set. Ordinary term leaves and the optimized multi-term AND path
   both use it. The cursor retains an overshooting posting so a later
   candidate cannot be skipped. Existing hit/position processing runs for
   retained rows, and corpus-wide keyword statistics remain unrestricted.
4. For RAM segments, the engine scans live rows under the segment lock using
   safe native filters, then resets the ranker and applies the completed set
   to sequential postings. A flat AND may use a safe subset of predicates;
   an unsafe filter tree falls back rather than dropping a possible OR branch.
   User-variable predicates are excluded from RAM prefiltering because a
   second binding could observe a different value set during the same query.
5. The normal residual filters, dead-row checks, field and position operators,
   rankers, sorters, and final expressions remain authoritative. A candidate
   may still fail MATCH or a residual filter. The optimization does not use
   `HIGHLIGHT()` as an approximation of matching.

A candidate set activates only after complete collection. If a disk producer
exceeds the limit, its wrapper replays the copied prefix, the first unconsumed
block, and the remaining source in order; normal matching continues without
candidate restriction. RAM fallback has not consumed postings, so it needs no
replay. An interrupted partial set is never activated, and interrupted totals
are marked approximate. Empty *complete* sets are valid and can stop matching
early.

The disk reader uses the existing skip checkpoints. Once candidates are active,
a forward seek on a file-backed reader can use its existing small 1,024-byte
buffer rather than retaining a large read-ahead hint. Mapped readers and
unhinted/fallback reads retain their normal policy. Posting, hitlist, and
skiplist file formats and decoding are unchanged. RAM postings have no
equivalent checkpoints and are advanced sequentially.

Wildcard and other payload expansions can use a separate cached leaf. The
candidate set filters its matching cache, including reset-time RAM hit reads.
Its initial corpus-statistics/cache pass remains unrestricted so rankings are
preserved. An empty-set guard also avoids an invalid binary-search range when
a reused RAM candidate vector has retained storage. The unrestricted setup
limits the speedup for some wildcard queries.

## Limit, memory, and concurrency

The per-daemon default is 65,536 candidate rows per disk chunk or RAM segment;
accepted values are 1 through 524,288:

```ini
searchd {
    match_by_candidates_limit = 100000
}
```

```sql
SET GLOBAL match_by_candidates_limit = 100000;
SHOW GLOBAL VARIABLES LIKE 'match_by_candidates_limit';
```

Startup and `SET GLOBAL` reject out-of-range values. A query context samples
the setting once, so an in-flight query keeps its limit. Runtime changes do
not survive a restart; each remote daemon has its own setting. Different
limits can change activation and latency without changing correct results.

The buffer stores sorted, chunk-local `RowID_t` values, not external document
IDs or full rows. It grows only as candidates arrive. At four bytes per ID,
the default copied-row bound is 256 KiB and the maximum is 2 MiB per context;
other iterator, ranker, and read buffers add to total query memory. Disk
chunks searched in parallel own separate buffers, so their peaks add. RAM
segments reuse one buffer within a context. The vector is destroyed at the
end of the query context; an allocator may retain freed pages in process RSS.

## SQL integration, agents, and observability

The SQL lexer recognizes `MatchByCandidates` only inside `/*+ ... */`; the
same word remains an ordinary identifier elsewhere. Quoted JSON subkeys and
comments inside a hint restore the hint lexer state. Generated FACET queries
inherit the flag, SQL query logs reproduce it, and a batch cannot share one
matching execution between hinted and unhinted queries. Hinted disk queries
bypass the query cache before ranker construction, so diagnostics reflect work
that actually ran.

For distributed search, the patched coordinator sends the hint in existing
query flag bit 18. A patched agent executes it and returns opt-in iterator
statistics in agent reply stat-mask bit 8. An unpatched agent whose protocol
otherwise accepts the coordinator's request ignores the unknown bit, returns
ordinary correct matches, and is reported as
`MatchByCandidatesFallback(agent_not_acknowledged)`. A coordinator without
this patch cannot parse and forward the hint; ordinary distributed queries
still use their existing behavior. No new standalone search command version
or disk format was added. These compatibility paths do not replace testing
against every historical daemon release.

After a hinted query, `SHOW META LIKE 'index'` reports, for example:

```text
@match:MatchByCandidates (100%), uploaderid:SecondaryIndex (100%)
@match:MatchByCandidatesFallback(candidate_limit) (100%), uploaderid:SecondaryIndex (100%)
```

`@match` percentages describe the fraction of reported matching contexts
that used each active/fallback path. They are neither selectivity nor elapsed
time. The renderer uses an `@match` denominator separate from the existing
index-stat denominator; this prevents multi-chunk results such as `900%`
while leaving older secondary-index entries formatted as before. A disk ID
lookup or columnar analyzer can supply candidates without a secondary-index
entry. Other fallback reasons include `no_iterator`, `no_match`,
`knn_or_join`, `no_native_prefilter`, `unsafe_filter_tree`,
`prefilter_unavailable`, `interrupted`, and `no_work`.

With `SET profiling=1`, `SHOW PROFILE` includes `candidate_collect` for disk
materialization and RAM scans, including attempts that reach the limit and
fall back. It does not represent all candidate matching time; existing
`read_docs`, `read_hits`, and `get_docs` states still account for matching
work. With profiling off, candidate collection does not take timestamps or
record profile state transitions. `SHOW PLAN` renders the transformed match
tree; use `SHOW META` to see whether candidate restriction actually activated
for each search context.

## Performance evidence

The controlled primary fixture had 100,000 documents in one disk chunk, six
attribute candidates, and one true phrase/proximity match. It used common
alphabet terms, numeric highlighting, `ranker=none`, one thread, identical
result options, disabled query cache, and separate copies of the same data
for the saved original and patched binaries. Times below are warm medians
after three warmups, from 21 measured runs:

| Build and execution | Median time |
| --- | ---: |
| Saved unmodified build | 14.313 ms |
| Patched build, no hint | 13.503 ms |
| Patched build, hint | 0.785 ms |

The hint was 17.2x faster than patched unhinted and 18.2x faster than the
saved build in this fixture. Separate ID-predicate measurements (15 warm
runs) gave 13.563 ms unhinted versus 0.769 ms hinted for all six IDs; one
known matching ID gave 1.507 ms versus 0.550 ms. The distinction matters:
one ID is much easier than a complete candidate list.

| Other fixture | Unhinted median | Hinted median | Ratio |
| --- | ---: | ---: | ---: |
| RAM, 25,000 rows | 16.189 ms | 0.799 ms | 20.3x |
| Wildcard, 12,000 rows | 6.606 ms | 3.280 ms | 2.0x |

Instrumented work counts on the disk reproduction fell from 405,612 to
2,720 decoded posting-row deltas and from 28,002 to 3 hitlist seeks. The
small-buffer tuning reduced data returned from the `.spd` file from about
7.86 MB in the first hinted prototype to about 110 KB in the tuned hinted
version. Those are call/reader measurements, not physical disk-page reads
or latency percentages, and instrumentation was excluded from wall-time runs.

One additional, non-repeated query on a larger local dataset returned the
same two rows with no warnings in about 24 ms hinted versus 3.15 s unhinted
(roughly 129x). It used a proximity query with four string-filter values,
`ranker=none`, `threads=1`, and `max_matches=10000`. This observation is
workload-specific and is not a controlled benchmark.

These timings were recorded during patch development before the final parser,
profiling, and metadata review fixes. The final feature commit was validated
for correctness, but this benchmark set was not rerun against that exact
commit.

The bound limits copied candidate memory, **not** collection cost. An
over-limit predicate pays for the attempted collection and then runs the
normal path; no general speedup or fixed fallback penalty is claimed.
RAM still scans live rows to collect candidates, and wildcard corpus setup
can dominate a hinted query. The research measurement artifacts are outside
this repository; the numbers here describe their setup and limits rather
than a maintained performance regression test.

## Validation and review boundaries

The feature extends the existing unit-test source
[`src/gtests/gtests_rtstuff.cpp`](../src/gtests/gtests_rtstuff.cpp) with
cursor, replay, exact/overflow bound, limit snapshot, parser, cancellation,
timeout, old-agent metadata, RAM user-variable, and RAM profiling cases. All
13 focused tests passed in the current built test binary. The existing SQL
fixture [`test/test_461/test.xml`](../test/test_461/test.xml) and its generated
`model.bin` cover RAM and disk chunks, ID and secondary-index paths, ranking
and weights, wildcard/reset behavior, limit fallback, nested agents, and
multi-index metadata. Its `rt_461`, `rtidx_461`, and `col_461` variants passed
in the recorded standard test run.

Important review boundaries remain: checkpoint/reader behavior on other
historical file layouts, concurrent cancellation timing, unusual third-party
plugins, and old/new agent combinations beyond the versions exercised by the
research. The patch also does not remove the existing wildcard statistics
pass or guarantee that an available candidate plan is cheapest for every
selectivity, cutoff, or thread count.

The implementation is grouped in existing files: SQL parsing and query state
in [`sphinxql.l`](../src/sphinxql.l), [`sphinxql.y`](../src/sphinxql.y),
[`searchdsql.cpp`](../src/searchdsql.cpp), and [`sphinx.h`](../src/sphinx.h);
candidate state and posting leaves in [`sphinxsearch.h`](../src/sphinxsearch.h)
and [`searchnode.cpp`](../src/searchnode.cpp); disk and RAM execution in
[`sphinx.cpp`](../src/sphinx.cpp) and [`sphinxrt.cpp`](../src/sphinxrt.cpp);
agent forwarding/aggregation in [`daemon/api_search.cpp`](../src/daemon/api_search.cpp),
[`daemon/search_handler.cpp`](../src/daemon/search_handler.cpp), and
[`daemon/query_log.cpp`](../src/daemon/query_log.cpp); and configuration,
metadata, and profiling in [`searchd.cpp`](../src/searchd.cpp),
[`sphinxutils.cpp`](../src/sphinxutils.cpp), and
[`queryprofile.h`](../src/queryprofile.h).
