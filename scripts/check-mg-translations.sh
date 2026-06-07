#!/usr/bin/env bash
# List Mercurygram-only string keys (lng_mg_*) that are missing from each
# shipped locale overlay relative to the English base (lang.strings).
# Exit 0 always — informational only.

set -eu

base=Telegram/Resources/langs/lang.strings
overlay_dir=Telegram/Resources/langs/mercurygram
if [ ! -f "$base" ]; then
    echo "error: $base not found (run from repo root)" >&2
    exit 2
fi

keys=$(grep -oE '"lng_mg_[^"]*"' "$base" | sort -u)
total=$(printf '%s\n' "$keys" | grep -c . || true)
missing_total=0

printf 'MG keys in base: %d\n' "$total"

for d in "$overlay_dir"/*.strings; do
    [ -e "$d" ] || continue
    locale=$(basename "$d" .strings)
    have=$(grep -oE '"lng_mg_[^"]*"' "$d" 2>/dev/null | sort -u || true)
    missing=$(comm -23 <(printf '%s\n' "$keys") <(printf '%s\n' "$have"))
    extra=$(comm -13 <(printf '%s\n' "$keys") <(printf '%s\n' "$have"))
    if [ -n "$missing" ] || [ -n "$extra" ]; then
        n=$(printf '%s\n' "$missing" | grep -c . || true)
        missing_total=$((missing_total + n))
        printf '\n## %s (%d missing)\n' "$locale" "$n"
        [ -n "$missing" ] && printf '%s\n' "$missing"
        [ -n "$extra" ] && printf 'unknown keys (not in base):\n%s\n' "$extra"
    fi
done

printf '\n---\nTotal missing across locales: %d\n' "$missing_total"
