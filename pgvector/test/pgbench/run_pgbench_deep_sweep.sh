#!/bin/bash
# Concurrency sweep at fixed ef_search=100, matching the PhD student's table format.
# Adapted from run_pgbench_deep.sh: ptandra paths, port 15432, db testdb.

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PGBENCH=/ssd_root/ptandra/pg_install/bin/pgbench
PGB_ARGS="-n -h /tmp -p 15432 -U ptandra -d testdb"
QUERY_SQL="$SCRIPT_DIR/deep_query_scripts/deep_queries_random_100.sql"
WARMUP_T=120
RUN_T=60
CONCURRENCIES=(1 2 4 8 16 32 64)
OUT_DIR=/tmp/pgbench_results
mkdir -p "$OUT_DIR"

echo "Warmup: c=1 T=${WARMUP_T}s ..."
"$PGBENCH" $PGB_ARGS -f "$QUERY_SQL" -c 1 -j 1 -T "$WARMUP_T" 2>&1 | tee "$OUT_DIR/warmup.log" | tail -10

for c in "${CONCURRENCIES[@]}"; do
    echo
    echo "============================================================"
    echo "Run: ef_search=100, concurrency=${c}, T=${RUN_T}s"
    echo "============================================================"
    "$PGBENCH" $PGB_ARGS -f "$QUERY_SQL" -c "$c" -j "$c" -T "$RUN_T" 2>&1 \
        | tee "$OUT_DIR/c_${c}.log" | tail -12
done

echo
echo "All done $(date)"
