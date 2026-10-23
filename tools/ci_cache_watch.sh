#!/usr/bin/env bash
# Daily Actions-cache upkeep (.github/workflows/cache-watch.yml): cleans what it can, and exits 1 only for what
# it could not, which fails the scheduled run and so notifies the repo owner.
#
# Why: sccache entries written by a PR run live under refs/pull/<n>/merge and nothing can read them after
# the PR closes; on 2026-10-15 three merged PRs held 8.1 GB of a 10.9 GB cache (limit 10 GB) and main's
# live entries were being evicted, so builds ran 20-33 min instead of ~2. cache-cleanup.yml removes a PR's
# entries on close; this catches whatever that misses, and prunes outdated entries before the limit is hit.
#
# Steps (thresholds are env-overridable):
#   1. entries under refs/pull/* of a CLOSED PR (a leak): deleted, then reported
#   2. entries not read for STALE_DAYS (default 3): deleted oldest-read first, at most MAX_DELETES (default 800)
#      per run. They are objects of source that has since changed (on 2026-10-20, 3.0 GB of 8.2 GB). Not 7 days:
#      GitHub deletes entries unread for 7 days itself, so they would already be gone. One API call per entry,
#      and the job token gets 1000 calls an hour, hence the per-run cap; a larger backlog clears over a few days.
#   3. total size still > WARN_TOTAL_GB (default 8, i.e. 80% of the 10 GB limit) after 1-2: reported
set -euo pipefail
repo="${GITHUB_REPOSITORY:?}"
warn_total_gb="${WARN_TOTAL_GB:-8}"; stale_days="${STALE_DAYS:-3}"; max_deletes="${MAX_DELETES:-800}"
fail=0
say() { echo "::warning::$*"; fail=1; }
list() {
  gh api --paginate "repos/$repo/actions/caches?per_page=100" \
    --jq '.actions_caches[]|[.id,.ref,.size_in_bytes,.last_accessed_at]|@tsv'
}
summary() { awk -F'\t' '{s+=$3; n++} END{printf "%d %.2f", n, s/1e9}'; }

rows=$(list)
set -- $(echo "$rows" | summary)
echo "cache: $1 entries, $2 GB (limit 10)"

for n in $(echo "$rows" | awk -F'\t' '$2 ~ /^refs\/pull\//{split($2,a,"/"); print a[3]}' | sort -un); do
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
stale=$(echo "$rows" | awk -F'\t' -v c="$cutoff" '$2 ~ /^refs\/heads\// && $4 < c' | sort -t$'\t' -k4,4)
set -- $(echo "$stale" | grep . | summary || echo "0 0.00")
echo "not read for $stale_days days: $1 entries, $2 GB; deleting up to $max_deletes"
deleted=0
for id in $(echo "$stale" | grep . | cut -f1 | head -n "$max_deletes"); do
  gh api -X DELETE "repos/$repo/actions/caches/$id" >/dev/null 2>&1 && deleted=$((deleted + 1)) || true
done
echo "deleted $deleted outdated entries"

set -- $(list | summary)
echo "cache now: $1 entries, $2 GB"
awk "BEGIN{exit !($2 > $warn_total_gb)}" && say "cache still holds $2 GB after cleanup, over the $warn_total_gb GB line (limit 10 GB): builds will start missing"
exit $fail
