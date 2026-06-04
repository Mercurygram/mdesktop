#!/usr/bin/env bash
#
# This file is part of Mercurygram Desktop,
# an unofficial privacy/security focused fork of Telegram Desktop.
#
# Disable every GitHub Actions workflow that is not Mercurygram's own.
#
# Rebasing the MG commits onto an upstream tag pulls in upstream's ~18 workflow
# files (linux.yml, win.yml, mac.yml, snap.yml, docker.yml, winget.yml,
# master_updater.yml, the issue/stale bots, ...). Pushing a release/X.Y.Z branch
# would fire their `on: push` builds, and winget/master_updater fire on the
# `release` event our tag creates. None of those are wanted here.
#
# This sweeps `gh workflow list` and disables anything whose file basename is not
# in the keep-list below. Disabling is a persistent repository setting, so once a
# workflow is off it stays off across future pushes; this only needs to catch
# newly appeared ones. It edits no workflow files (consistent with "only add
# files, never touch existing workflows").
#
# Needs a token with actions:write (the default GITHUB_TOKEN is enough; no PAT).
#
# Optional environment:
#   RELEASE_BRANCH   if set (e.g. release/6.8.5), also cancel any runs an
#                    upstream workflow already started on that branch.

set -euo pipefail

# The only Mercurygram-owned workflows. Everything else is upstream -> disabled.
KEEP="release.yml upstream-mirror.yml"
# Display names of the kept workflows (for the run-cancel guard).
KEEP_NAMES='["Release.","Upstream mirror"]'

log() { printf '>> %s\n' "$*"; }

# One jq pass: emit "active<TAB>name<TAB>basename" rows. `gh workflow disable`
# accepts the workflow file basename, so we never need the numeric id.
gh workflow list --all --json name,path,state \
		--jq '.[] | [.state, .name, (.path | split("/") | last)] | @tsv' \
	| while IFS=$'\t' read -r state name base; do
	case " $KEEP " in
		*" $base "*) continue ;;
	esac
	[ "$state" = active ] || continue
	log "disabling upstream workflow: $name ($base)"
	gh workflow disable "$base" || true
done

if [ -n "${RELEASE_BRANCH:-}" ]; then
	for run in $(gh run list --branch "$RELEASE_BRANCH" --limit 50 \
			--json databaseId,workflowName \
			--jq ".[] | select(.workflowName as \$n | ${KEEP_NAMES} | index(\$n) | not) | .databaseId" \
			2>/dev/null || true); do
		log "cancelling stray run $run on $RELEASE_BRANCH"
		gh run cancel "$run" || true
	done
fi
