#!/bin/sh
# backup.sh <db-file> <backup-dir> — consistent SQLite snapshot via .backup
# (safe against a live WAL writer), date-stamped, keeps the newest 14.
set -eu

DB="$1"
OUT="$2"
STAMP=$(date +%Y%m%d-%H%M%S)

mkdir -p "$OUT"
sqlite3 "$DB" ".backup '$OUT/solsrv-$STAMP.db'"
ls -1t "$OUT"/solsrv-*.db | tail -n +15 | xargs -r rm --
echo "backup: $OUT/solsrv-$STAMP.db"
