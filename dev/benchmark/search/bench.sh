#!/bin/bash
# chdb_search (pg_chdb) against ParadeDB's pg_search on the Hacker News
# dataset, in one PostgreSQL cluster. Each phase appends to $RESULTS
# (default ./results):
#
#   env               hardware, versions and settings            env.txt
#   load              load.sql: hn and hn_pdb from the Parquet   load.csv
#   build             both index builds: time, CPU, WAL, size    build.csv
#   queries L [E...] [-- Q...]
#                     queries.sql, COLD + RUNS runs per query    queries_raw.csv
#   phrase_index      the chdb index with token positions        build.csv
#   writes            single-row and batched inserts             writes.csv
#   maintenance       REINDEX, DELETE 1%, VACUUM                 maintenance.csv
#   reindex_parallel  ParadeDB's REINDEX with more workers       maintenance.csv
#   time_in_index     both indexes with time in them             build.csv
#   summary           median and p99 per query                   queries.csv
#   all               every phase, in the order of REPORT.md
#
# The cluster must have pg_search in shared_preload_libraries and a
# database $PGDATABASE (default bench) with chdb, chdb_search and pg_search
# created. Connection settings come from the usual PG* variables, and
# PGDATA must name the cluster's directory. PG_RESTART is the command that
# restarts the cluster before each engine's queries (default pg_ctl
# restart); run the cluster in a cgroup of its own (systemd-run) for the
# CPU seconds to be its alone.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
R=${RESULTS:-$HERE/results}
export PGDATABASE=${PGDATABASE:-bench}
COLD=${COLD:-3}
RUNS=${RUNS:-20}
PG_RESTART=${PG_RESTART:-"pg_ctl -D ${PGDATA:-} -m fast -w restart"}
SRC=${SRC:-https://datasets-documentation.s3.eu-west-3.amazonaws.com/hackernews/hacknernews.parquet}
CHDB_IDX=hn_text_chdb
PDB_IDX=hn_pdb_text
mkdir -p "$R"/raw "$R"/plans "$R"/sql

psql_() { psql -X -q -At -v ON_ERROR_STOP=1 "$@"; }
now() { date +%s.%N; }
secs() { awk -v a="$1" -v b="$2" 'BEGIN { printf "%.3f", b - a }'; }
lsn() { psql_ -c 'SELECT pg_current_wal_lsn()'; }
wal() { psql_ -c "SELECT '$2'::pg_lsn - '$1'::pg_lsn"; }
load_avg() { cut -d' ' -f1 /proc/loadavg; }
# CPU seconds used by the cluster's cgroup (every backend, worker and engine),
# meaningful when the cluster runs in a cgroup of its own, as under
# systemd-run; empty where the cgroup cannot be read.
cpu() {
	local cg
	cg=$(cut -d: -f3 /proc/"$(head -1 "${PGDATA:-}"/postmaster.pid 2>/dev/null)"/cgroup 2>/dev/null) &&
		awk '/^usage_usec/ { printf "%.3f", $2 / 1e6 }' /sys/fs/cgroup"$cg"/cpu.stat 2>/dev/null
}
cpu_delta() { [ -n "$1" ] && [ -n "$2" ] && awk -v a="$1" -v b="$2" 'BEGIN { printf "%.1f", b - a }'; }
# Waits until the cluster writes under 1 MB of WAL in 10 s, so that one
# engine's background merges (chdb parts, ParadeDB segments) or autovacuum do
# not land in the next measurement; gives up after 20 minutes.
quiesce() {
	local a b i
	for i in $(seq 1 120); do
		a=$(lsn); sleep 10; b=$(lsn)
		[ "$(wal $a $b)" -lt 1048576 ] && return 0
	done
	echo "quiesce: still writing WAL after 20 minutes" >&2
}
restart() { eval "$PG_RESTART" > /dev/null; psql_ -c 'SELECT 1' > /dev/null; }

# The chdb store's active parts and the ParadeDB index's visible segments.
parts() {
	psql_ -c "SELECT n FROM chdb.chdb_search_query(
	  (SELECT format('SELECT count() FROM system.parts WHERE active AND database = %L AND table = %L',
	                 split_part(t, '.', 1), split_part(t, '.', 2))
	     FROM chdb.chdb_search_store_table('$CHDB_IDX') t)) AS (n bigint)"
}
segments() { psql_ -c "SELECT count(*) FROM paradedb.index_info('$PDB_IDX')"; }

table_of() { [ "$1" = chdb ] && echo hn || echo hn_pdb; }
index_of() { [ "$1" = chdb ] && echo $CHDB_IDX || echo $PDB_IDX; }
units_of() { [ "$1" = chdb ] && parts || segments; }

# Runs one statement, timed, with the cluster's CPU seconds and the WAL it
# wrote; prints "secs,cpu_secs,wal".
timed() {
	local l0 l1 t0 t1 c0 c1
	l0=$(lsn); c0=$(cpu); t0=$(now)
	psql_ -c "$1" > /dev/null || return 1
	t1=$(now); c1=$(cpu); l1=$(lsn)
	echo "$(secs $t0 $t1),$(cpu_delta "$c0" "$c1"),$(wal $l0 $l1)"
}

phase_env() {
	{
		echo "# date"; date -Is
		echo "# uname"; uname -a
		echo "# lscpu"; lscpu
		echo "# memory"; free -g
		echo "# disk under PGDATA"; df -h "${PGDATA:-.}" | tail -1; findmnt -T "${PGDATA:-.}" -no SOURCE,FSTYPE
		echo "# postgres"; psql_ -c 'SELECT version()'
		echo "# extensions"; psql_ -F ' ' -c "SELECT extname, extversion FROM pg_extension ORDER BY 1"
		echo "# chdb_search library"; psql_ -c 'SELECT chdb.chdb_search_version()'
		echo "# libchdb in the engine"; psql_ -c "SELECT v FROM chdb.chdb_search_query('SELECT version()') AS (v text)"
		echo "# libchdb.so mapped by the engine"
		f=$(awk '/libchdb/ { print $6; exit }' /proc/"$(psql_ -c 'SELECT chdb.chdb_search_engine_pid()')"/maps)
		echo "$f"; sha256sum "$f"
		echo "# pg_search build"; psql_ -F ' ' -c 'SELECT * FROM paradedb.version_info()' 2>&1
		echo "# settings"
		psql_ -F ' = ' -c "SELECT name, current_setting(name) FROM pg_settings
		  WHERE source NOT IN ('default', 'override') OR name ~ '^(chdb|chdb_search|paradedb)\.'
		     OR name IN ('maintenance_work_mem', 'max_parallel_workers_per_gather',
		                 'max_parallel_maintenance_workers', 'max_worker_processes', 'jit',
		                 'synchronous_commit', 'autovacuum', 'effective_cache_size')
		  ORDER BY 1"
	} > "$R"/env.txt 2>&1
	echo "env -> $R/env.txt"
}

phase_load() {
	local t0 t1 extra=""
	[ -n "${LIMIT:-}" ] && extra="-v limit=$LIMIT"
	t0=$(now)
	psql -X -v ON_ERROR_STOP=1 -v src="$SRC" $extra -f "$HERE"/load.sql > "$R"/load.log 2>&1 || { tail "$R"/load.log; return 1; }
	t1=$(now)
	echo "step,seconds" > "$R"/load.csv
	grep -B1 '^Time:' "$R"/load.log | awk '/^Time:/ { print prev "," $2 / 1000 } { prev = $0 }' | sed 's/^--$//' >> "$R"/load.csv
	echo "total,$(secs $t0 $t1)" >> "$R"/load.csv
	cat "$R"/load.log | tail -4
}

phase_build() {
	local r
	[ -f "$R"/build.csv ] || echo "engine,step,seconds,cpu_seconds,wal_bytes,index_bytes,units,load_avg" > "$R"/build.csv
	r=$(timed "CREATE INDEX $CHDB_IDX ON hn USING chdb (text)") || return 1
	echo "chdb,create index,$r,$(psql_ -c "SELECT pg_relation_size('$CHDB_IDX')"),$(parts),$(load_avg)" >> "$R"/build.csv
	r=$(timed "CREATE INDEX $PDB_IDX ON hn_pdb USING paradedb (id, text) WITH (key_field = 'id')") || return 1
	echo "pdb,create index,$r,$(psql_ -c "SELECT pg_relation_size('$PDB_IDX')"),$(segments),$(load_avg)" >> "$R"/build.csv
	cat "$R"/build.csv
}

# The chdb index again with token positions (support_phrase_search), which
# @@~ uses where the default index checks each candidate row's text, timed
# with the phrase and term queries; then the default index once more.
phase_phrase_index() {
	local r
	psql_ -c "DROP INDEX IF EXISTS $CHDB_IDX" || return 1
	r=$(timed "CREATE INDEX $CHDB_IDX ON hn USING chdb (text chdb.text_ops (support_phrase_search = true))") || return 1
	echo "chdb,create index (support_phrase_search),$r,$(psql_ -c "SELECT pg_relation_size('$CHDB_IDX')"),$(parts),$(load_avg)" >> "$R"/build.csv
	phase_queries phrase_idx chdb -- phrase term_common
	psql_ -c "DROP INDEX IF EXISTS $CHDB_IDX" || return 1
	r=$(timed "CREATE INDEX $CHDB_IDX ON hn USING chdb (text)") || return 1
	echo "chdb,create index (again),$r,$(psql_ -c "SELECT pg_relation_size('$CHDB_IDX')"),$(parts),$(load_avg)" >> "$R"/build.csv
	tail -2 "$R"/build.csv
}

# Supplementary: both indexes rebuilt with time as a column of their own
# (chdb columnar_ops, a ParadeDB fast field), so the time filter of
# top10_score_time is the index's rather than the heap's; timed as builds,
# then that query and the plain top-10 again.
phase_time_in_index() {
	local r
	psql_ -c "DROP INDEX IF EXISTS $CHDB_IDX" -c "DROP INDEX IF EXISTS $PDB_IDX" || return 1
	quiesce
	r=$(timed "CREATE INDEX $CHDB_IDX ON hn USING chdb (text, time)") || return 1
	echo "chdb,create index (text; time columnar_ops),$r,$(psql_ -c "SELECT pg_relation_size('$CHDB_IDX')"),$(parts),$(load_avg)" >> "$R"/build.csv
	quiesce
	r=$(timed "CREATE INDEX $PDB_IDX ON hn_pdb USING paradedb (id, text, time) WITH (key_field = 'id')") || return 1
	echo "pdb,create index (id; text; time),$r,$(psql_ -c "SELECT pg_relation_size('$PDB_IDX')"),$(segments),$(load_avg)" >> "$R"/build.csv
	quiesce
	phase_queries time_in_index chdb pdb -- top10_score_time top10_score
	tail -2 "$R"/build.csv
}

# Splits queries.sql at its "-- q: name engine" markers.
split_queries() {
	awk -v dir="$R/sql" '
		/^-- q: / { out = dir "/" $3 "_" $4 ".sql"; printf "" > out; next }
		out && /^(--|$)/ { next }
		out { print >> out }' "$HERE"/queries.sql
}

query_names() { grep '^-- q: ' "$HERE"/queries.sql | awk '{ print $3 }' | uniq; }

# bench.sh queries <label> [engine ...] [-- query ...]
phase_queries() {
	local label=$1 engines="" names="" e q f opts n
	shift
	while [ $# -gt 0 ] && [ "$1" != "--" ]; do engines="$engines $1"; shift; done
	[ "${1:-}" = "--" ] && { shift; names="$*"; }
	engines=${engines:-chdb pdb}
	names=${names:-$(query_names)}
	split_queries
	[ -f "$R"/queries_raw.csv ] || echo "label,engine,query,run,kind,ms,load_avg" > "$R"/queries_raw.csv
	[ -f "$R"/queries_meta.csv ] || echo "label,engine,query,rows,status" > "$R"/queries_meta.csv
	for e in $engines; do
		restart
		opts="-c search_path=public"
		[ "$e" = chdb ] && opts="-c search_path=chdb,public"
		for q in $names; do
			f="$R/sql/${q}_$e.sql"
			rm -f "$R/raw/${label}_${q}_$e".*
			if PGOPTIONS="$opts" pgbench -n -c 1 -j 1 -t $((COLD + RUNS)) -f "$f" -l \
			      --log-prefix="$R/raw/${label}_${q}_$e" > "$R/raw/${label}_${q}_$e.out" 2>&1 &&
			   ! grep -q 'aborted' "$R/raw/${label}_${q}_$e.out"; then
				awk -v l="$label" -v e="$e" -v q="$q" -v c="$COLD" -v la="$(load_avg)" '
					{ n++; printf "%s,%s,%s,%d,%s,%.3f,%s\n", l, e, q, n, n <= c ? "cold" : "warm", $3 / 1000, la }' \
					"$R/raw/${label}_${q}_$e".[0-9]* >> "$R"/queries_raw.csv
				PGOPTIONS="$opts" psql -X -At -c "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF) $(cat "$f")" \
					> "$R/plans/${label}_${q}_$e.txt" 2>&1
				n=$(head -1 "$R/plans/${label}_${q}_$e.txt" | sed -n 's/.*actual rows=\([0-9.]*\).*/\1/p')
				case $q in count*|top10*|limit*)
					PGOPTIONS="$opts" psql -X -At -F ' ' -f "$f" > "$R/plans/${label}_${q}_$e.out" 2>&1 ;;
				esac
				echo "$label,$e,$q,${n%.00},ok" >> "$R"/queries_meta.csv
				echo "$label $e $q: rows ${n%.00}"
			else
				n=$(grep -m1 -o 'ERROR: .*' "$R/raw/${label}_${q}_$e.out" | tr ',' ';')
				echo "$label,$e,$q,,${n:-failed}" >> "$R"/queries_meta.csv
				echo "$label $e $q: ${n:-failed}"
			fi
		done
	done
}

# 10,000 single-row commits and 100 commits of 1,000 rows, the same rows into
# each table (and into an empty reference copy without a search index), with
# the WAL each wrote and the parts or segments the index has after.
phase_writes() {
	local e t base w name commits per rows s b u0 u1 u2 l0 l1 c0 c1 t0 t1
	base=$(psql_ -c "SELECT max(id) FROM hn")
	psql_ -c "DROP TABLE IF EXISTS hn_src, hn_ref" \
	      -c "CREATE TABLE hn_src AS
	            SELECT row_number() OVER (ORDER BY id) AS n, \"by\", time, type, text, parent, score
	              FROM (SELECT * FROM hn WHERE type = 'comment' AND text <> '' ORDER BY id DESC LIMIT 110000) s" \
	      -c "CREATE UNIQUE INDEX ON hn_src (n)" \
	      -c "CREATE TABLE hn_ref (LIKE hn INCLUDING ALL EXCLUDING INDEXES)" \
	      -c "ALTER TABLE hn_ref ADD PRIMARY KEY (id)" \
	      -c "VACUUM ANALYZE hn_src" || return 1
	[ -f "$R"/writes.csv ] || echo "engine,test,commits,rows,seconds,cpu_seconds,rows_per_s,wal_bytes,wal_per_row,units_before,units_after,units_after_60s,load_avg" > "$R"/writes.csv
	for e in ref chdb pdb; do
		case $e in ref) t=hn_ref;; chdb) t=hn;; pdb) t=hn_pdb;; esac
		psql_ -c "DROP SEQUENCE IF EXISTS new_id_$t" -c "CREATE SEQUENCE new_id_$t START $((base + 1))" || return 1
		cat > "$R"/sql/write_single_$e.sql <<-EOF
			INSERT INTO $t (id, "by", time, type, text, parent, score)
			SELECT s.id, src."by", src.time, src.type, src.text, src.parent, src.score
			  FROM (SELECT nextval('new_id_$t') AS id) s JOIN hn_src src ON src.n = s.id - $base;
		EOF
		cat > "$R"/sql/write_batch_$e.sql <<-EOF
			INSERT INTO $t (id, "by", time, type, text, parent, score)
			SELECT s.id, src."by", src.time, src.type, src.text, src.parent, src.score
			  FROM (SELECT nextval('new_id_$t') AS id FROM generate_series(1, 1000)) s
			  JOIN hn_src src ON src.n = s.id - $base;
		EOF
		for w in single:10000:1 batch:100:1000; do
			IFS=: read -r name commits per <<< "$w"
			quiesce
			u0=""; [ $e != ref ] && u0=$(units_of $e)
			l0=$(lsn); c0=$(cpu); t0=$(now)
			pgbench -n -c 1 -j 1 -t $commits -f "$R"/sql/write_${name}_$e.sql > "$R"/raw/write_${name}_$e.out 2>&1 ||
				{ echo "$e $name failed"; tail -3 "$R"/raw/write_${name}_$e.out; return 1; }
			t1=$(now); c1=$(cpu); l1=$(lsn)
			u1=""; u2=""
			if [ $e != ref ]; then u1=$(units_of $e); sleep 60; u2=$(units_of $e); fi
			rows=$((commits * per)); s=$(secs $t0 $t1); b=$(wal $l0 $l1)
			echo "$e,$name,$commits,$rows,$s,$(cpu_delta "$c0" "$c1"),$(awk -v r=$rows -v s=$s 'BEGIN { printf "%.0f", r / s }'),$b,$((b / rows)),$u0,$u1,$u2,$(load_avg)" >> "$R"/writes.csv
			tail -1 "$R"/writes.csv
		done
	done
}

# REINDEX, then DELETE 1% of the original rows and VACUUM, per engine.
# REINDEX=0 skips the rebuild; DELETE_MOD picks another 1% (id % 100 = it).
phase_maintenance() {
	local e t i r n m=${DELETE_MOD:-0}
	[ -f "$R"/maintenance.csv ] || echo "engine,step,seconds,cpu_seconds,wal_bytes,index_bytes,units,load_avg" > "$R"/maintenance.csv
	for e in chdb pdb; do
		t=$(table_of $e); i=$(index_of $e)
		if [ "${REINDEX:-1}" = 1 ]; then
			quiesce; r=$(timed "REINDEX INDEX $i") || return 1
			echo "$e,reindex,$r,$(psql_ -c "SELECT pg_relation_size('$i')"),$(units_of $e),$(load_avg)" >> "$R"/maintenance.csv
		fi
		n=$(psql_ -c "SELECT count(*) FROM $t WHERE id % 100 = $m")
		quiesce; r=$(timed "DELETE FROM $t WHERE id % 100 = $m") || return 1
		echo "$e,delete $n rows (id % 100 = $m),$r,$(psql_ -c "SELECT pg_relation_size('$i')"),$(units_of $e),$(load_avg)" >> "$R"/maintenance.csv
		quiesce; r=$(timed "VACUUM $t") || return 1
		echo "$e,vacuum,$r,$(psql_ -c "SELECT pg_relation_size('$i')"),$(units_of $e),$(load_avg)" >> "$R"/maintenance.csv
	done
	cat "$R"/maintenance.csv
}

# Supplementary: ParadeDB's REINDEX with more parallel maintenance workers
# than the default two, which its build warning suggests (WORKERS, default
# 6), and the maintenance_work_mem it then requires (MWM, default 1GB).
phase_reindex_parallel() {
	local r w=${WORKERS:-6} m=${MWM:-1GB}
	quiesce
	r=$(PGOPTIONS="-c max_parallel_maintenance_workers=$w -c maintenance_work_mem=$m" timed "REINDEX INDEX $PDB_IDX") || return 1
	echo "pdb,reindex (max_parallel_maintenance_workers = $w; maintenance_work_mem = $m),$r,$(psql_ -c "SELECT pg_relation_size('$PDB_IDX')"),$(segments),$(load_avg)" >> "$R"/maintenance.csv
	tail -1 "$R"/maintenance.csv
}

# Median and nearest-rank p99 of the warm runs; with 20 runs the p99 is the
# slowest of them.
phase_summary() {
	python3 - "$R" <<-'EOF'
		import csv, math, statistics, sys
		r = sys.argv[1]
		runs, meta = {}, {}
		for row in csv.DictReader(open(f"{r}/queries_raw.csv")):
		    runs.setdefault((row["label"], row["query"], row["engine"]), []).append(row)
		for row in csv.DictReader(open(f"{r}/queries_meta.csv")):
		    meta[(row["label"], row["query"], row["engine"])] = row
		with open(f"{r}/queries.csv", "w", newline="") as f:
		    w = csv.writer(f)
		    w.writerow(["label", "query", "engine", "rows", "status", "first_ms", "cold_median_ms", "warm_median_ms", "warm_p99_ms", "warm_runs"])
		    for k, m in meta.items():
		        rs = runs.get(k, [])
		        cold = [float(x["ms"]) for x in rs if x["kind"] == "cold"]
		        warm = sorted(float(x["ms"]) for x in rs if x["kind"] == "warm")
		        p99 = warm[math.ceil(0.99 * len(warm)) - 1] if warm else ""
		        w.writerow([k[0], k[1], k[2], m["rows"], m["status"],
		                    f"{cold[0]:.2f}" if cold else "",
		                    f"{statistics.median(cold):.2f}" if cold else "",
		                    f"{statistics.median(warm):.2f}" if warm else "",
		                    f"{p99:.2f}" if warm else "", len(warm)])
		print(open(f"{r}/queries.csv").read())
	EOF
}

case "${1:-}" in
env) phase_env ;;
load) phase_load ;;
build) phase_build ;;
phrase_index) phase_phrase_index ;;
time_in_index) phase_time_in_index ;;
queries) shift; phase_queries "$@" ;;
writes) phase_writes ;;
maintenance) phase_maintenance ;;
reindex_parallel) phase_reindex_parallel ;;
summary) phase_summary ;;
all) phase_load && phase_env && phase_build &&
     phase_queries base chdb pdb && phase_queries base_rev pdb chdb &&
     phase_phrase_index && phase_writes && phase_queries after_writes chdb pdb &&
     phase_maintenance && REINDEX=0 DELETE_MOD=1 phase_maintenance &&
     REINDEX=0 DELETE_MOD=2 phase_maintenance && phase_reindex_parallel &&
     phase_time_in_index && phase_summary ;;
*) sed -n '2,24p' "$0"; exit 2 ;;
esac
