#!/usr/bin/env bash
# Weekly Actions-cache health check (.github/workflows/cache-watch.yml). Exit 1 = a threshold was crossed,
# which fails the scheduled run and so notifies the repo owner.
#
# Why: sccache entries written by a PR run live under refs/pull/<n>/merge and nothing can read them after
# the PR closes; on 2026-10-15 three merged PRs held 8.1 GB of a 10.9 GB cache (limit 10 GB) and main's
# live entries were being evicted, so builds ran 20-33 min instead of ~2. cache-cleanup.yml removes a PR's
# entries on close; this catches whatever that misses, and a cache growing toward the limit.
#
# Checks (thresholds are env-overridable):
#   1. total size > WARN_TOTAL_GB (default 8, i.e. 80% of the 10 GB limit)
#   2. entries under refs/pull/* of a CLOSED PR (a leak): deleted here, then reported
#   3. entries not read for STALE_DAYS (default 7) totalling more than STALE_GB (default 1)
set -euo pipefail
repo="${GITHUB_REPOSITORY:?}"
warn_total_gb="${WARN_TOTAL_GB:-8}"; stale_days="${STALE_DAYS:-7}"; stale_gb="${STALE_GB:-1}"
fail=0
say() { echo "::warning::$*"; fail=1; }

rows=$(gh api --paginate "repos/$repo/actions/caches?per_page=100" \
  --jq '.actions_caches[]|[.ref,.size_in_bytes,.last_accessed_at]|@tsv')
total=$(echo "$rows" | awk -F'\t' '{s+=$2} END{printf "%.2f", s/1e9}')
count=$(echo "$rows" | grep -c . || true)
echo "cache: $count entries, $total GB (limit 10)"

awk "BEGIN{exit !($total > $warn_total_gb)}" && say "cache holds $total GB, over the $warn_total_gb GB warning line (limit 10 GB): builds will start missing"

for n in $(echo "$rows" | awk -F'\t' '$1 ~ /^refs\/pull\//{split($1,a,"/"); print a[3]}' | sort -un); do
  state=$(gh pr view "$n" --repo "$repo" --json state --jq .state 2>/dev/null || echo UNKNOWN)
  if [ "$state" != "OPEN" ] && [ "$state" != "UNKNOWN" ]; then
    say "PR #$n is $state but still has cache entries (cache-cleanup.yml missed it); deleting"
    while :; do
      c=$(gh api -X DELETE "repos/$repo/actions/caches?key=sccache&ref=refs/pull/$n/merge" --jq .total_count) || c=0
      [ "$c" -gt 0 ] || break
    done
  fi
done

cutoff=$(date -u -d "$stale_days days ago" +%Y-%m-%dT%H:%M:%S)
stale=$(echo "$rows" | awk -F'\t' -v c="$cutoff" '$3 < c {s+=$2; n++} END{printf "%d %.2f", n, s/1e9}')
set -- $stale
echo "not read for $stale_days days: $1 entries, $2 GB"
awk "BEGIN{exit !($2 > $stale_gb)}" && say "$1 entries ($2 GB) were not read in $stale_days days: outdated cache, consider clearing key=sccache on main"
exit $fail
