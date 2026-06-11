#!/usr/bin/env bash
#
# This file is part of Mercurygram Desktop,
# an unofficial privacy/security focused fork of Telegram Desktop.
#
# Mirror an upstream Telegram Desktop release into a Mercurygram release.
#
# For the latest upstream *stable* release and the latest upstream *pre-release*
# that we have not mirrored yet, this:
#   1. rebases the Mercurygram (MG) commits onto the upstream release tag so they
#      are the last commits on the tree -- linear history, no merge commits;
#   2. adds a "Version X.Y.Z.1[ beta]" commit (set_release_version.py);
#   3. pushes a release/X.Y.Z branch and a vX.Y.Z.1[-beta] tag.
#
# Pushing the tag (with a PAT, not the default GITHUB_TOKEN) triggers the
# existing .github/workflows/release.yml signed-build pipeline.
#
# The MG commit set is exactly `${MG_BASE}..origin/dev`, where
#   MG_BASE = git merge-base origin/dev upstream/dev
# i.e. every commit on dev that is not in upstream's dev branch.
#
# Conflicts:
#  - The MG branding commit renames product strings on lines next to the version
#    numbers upstream bumps every release, so it always conflicts in the five
#    version files. That conflict is resolved deterministically with
#    `git merge-file --theirs` (keep MG branding in the conflicting hunks, keep
#    every non-conflicting upstream change), and the stale version numbers it
#    leaves behind are overwritten by set_release_version.py in step 2.
#  - For C++ sources/headers and the non-C++ files MG patches touch next to
#    upstream churn (build lists, lang strings, .style, CI recipes) a merge
#    driver (configure_merge_driver()) resolves the content merge with git's own
#    three-way `git merge-file` (xdiff) instead of the rebase's default `ort`
#    strategy. This is required, not cosmetic: ort's internal merge is *zealous*
#    -- for an MG hunk that lands next to an upstream change (the common case
#    once upstream relocates the code an MG patch hooks into) ort can silently
#    resolve the whole file to the upstream side, dropping the MG insertion with
#    no conflict markers. `git merge-file` instead keeps
#    both sides' non-overlapping changes (so the MG hook lands on the relocated
#    upstream code) and emits markers only where the two genuinely overlap. A
#    clean driver merge exits 0, so git stages it; an overlap leaves markers and
#    falls through to rerere.
#  - Whatever the driver/ort leaves conflicted falls through to rerere (a
#    persisted resolution cache). If rerere cannot resolve it either, the commit
#    is checked for having been superseded upstream (taking the upstream side of
#    every conflict empties it -> drop it); otherwise the rebase is aborted, a
#    "mirror conflict" issue is opened with the conflicting commit and hunks,
#    and the script fails. While that issue stays open the tag is skipped, so
#    the failure is reported once instead of on every scheduled run; closing
#    the issue (or fixing dev) re-arms the mirror.
#
# (This replaced an earlier mergiraf syntax-aware driver: mergiraf kept the MG
# hooks but reordered class members -- it sank ApiWrap's `using SharedMediaType`
# alias below the structs that use it, producing an uncompilable apiwrap.h.
# `git merge-file` preserves declaration order, needs no external binary, and is
# always present.)
#
# rerere stores a *normalised* preimage, so a recorded seed is independent of the
# merge.conflictstyle and of which merge produced the conflict -- the committed
# seeds replay under this script's diff3 setup unchanged.
#
# Required environment:
#   MIRROR_TOKEN      PAT with contents:write AND workflows:write on GH_REPO
#                     (workflows scope is required because the MG commits touch
#                     .github/workflows/release.yml; without it the tag push is
#                     rejected, and the default GITHUB_TOKEN would not trigger
#                     release.yml anyway).
#   GH_REPO           owner/name of this fork (e.g. Mercurygram/mdesktop).
#   GH_TOKEN          token gh uses to read the upstream releases API and to
#                     manage the mirror-conflict issues (issues:write).
# Optional:
#   UPSTREAM_REPO     default telegramdesktop/tdesktop
#   UPSTREAM_URL      default https://github.com/<UPSTREAM_REPO>.git
#   MIRROR_GIT_NAME   author/committer for the version commit (default below)
#   MIRROR_GIT_EMAIL  ditto
#   FORCE_TAG         re-mirror exactly this upstream tag (e.g. v6.8.5),
#                     bypassing the latest-only and idempotency checks (and any
#                     open conflict issue) and force-replacing its release
#                     branch + tag.
#   CANARY            set to 1 for a dry-run replay of the MG commits onto the
#                     upstream/dev tip: no pushes, no tags; a rolling issue is
#                     opened/updated while the replay conflicts and closed when
#                     it is clean again. Early warning for the conflicts the
#                     next release tag will hit, while they are still cheap to
#                     fix on dev.

set -euo pipefail

UPSTREAM_REPO="${UPSTREAM_REPO:-telegramdesktop/tdesktop}"
UPSTREAM_URL="${UPSTREAM_URL:-https://github.com/${UPSTREAM_REPO}.git}"
MIRROR_GIT_NAME="${MIRROR_GIT_NAME:-Mercurygram CI}"
MIRROR_GIT_EMAIL="${MIRROR_GIT_EMAIL:-ci@mercurygram.local}"
FORCE_TAG="${FORCE_TAG:-}"
CANARY="${CANARY:-}"

: "${GH_REPO:?GH_REPO must be set (owner/name of this fork)}"
: "${MIRROR_TOKEN:?MIRROR_TOKEN must be set (PAT with contents+workflows write)}"

# The five files set_release_version.py rewrites; for these, version-number
# conflicts are harmless and resolved deterministically (see header).
VERSION_FILES="\
Telegram/build/version \
Telegram/SourceFiles/core/version.h \
Telegram/Resources/winrc/Telegram.rc \
Telegram/Resources/winrc/Updater.rc \
Telegram/Resources/uwp/AppX/AppxManifest.xml"

ISSUE_LABEL="mirror-conflict"
CANARY_TITLE="canary: MG commits conflict with upstream/dev"

repo_root="$(git rev-parse --show-toplevel)"
cd "$repo_root"
seed_dir="${repo_root}/.github/rerere-seed"
# Markdown report of the last unresolved conflict, written by drive_rebase and
# posted to the conflict issue by the caller.
conflict_report="$(mktemp)"

log() { printf '>> %s\n' "$*"; }
err() { printf '!! %s\n' "$*" >&2; }

# ---- conflict issues -------------------------------------------------------
# One open issue per stuck target (a mirror tag, or the canary) carries the
# conflicting commit + hunks. While it is open the target is skipped, so a
# stuck tag produces one failed run + one issue instead of a failure email
# every 6 hours. The issue is closed automatically once the target mirrors
# cleanly (i.e. after dev was fixed), or manually to force a retry.

ensure_label() {
	gh label create "$ISSUE_LABEL" \
		--description "upstream mirror rebase conflict" --color B60205 \
		>/dev/null 2>&1 || true
}

open_issue_number() { # <exact title> -> issue number (empty if none)
	gh issue list --label "$ISSUE_LABEL" --state open \
		--json number,title \
		--jq "map(select(.title == \"$1\")) | .[0].number // empty" \
		2>/dev/null || true
}

file_conflict_issue() { # <title> <body-file>
	ensure_label
	local num; num="$(open_issue_number "$1")"
	if [ -n "$num" ]; then
		gh issue comment "$num" --body-file "$2" >/dev/null \
			&& log "updated conflict issue #${num} ($1)" \
			|| err "failed to update conflict issue ($1)"
	else
		gh issue create --label "$ISSUE_LABEL" --title "$1" --body-file "$2" \
			>/dev/null \
			&& log "opened conflict issue ($1)" \
			|| err "failed to open conflict issue ($1)"
	fi
}

close_conflict_issue() { # <title> <comment>
	local num; num="$(open_issue_number "$1")"
	if [ -n "$num" ]; then
		gh issue close "$num" --comment "$2" >/dev/null 2>&1 \
			&& log "closed conflict issue #${num} ($1)" || true
	fi
}

# ---------------------------------------------------------------------------

# Register a merge driver that resolves conflict-prone files with git's own
# three-way `git merge-file` (xdiff) instead of the rebase's default `ort`
# content merge (see the header "Conflicts:" note for why ort is unsafe here:
# it can silently drop an MG insertion that lands next to an upstream change --
# and, worse, it does so with *no conflict markers*, so rerere never sees it and
# the run ships a branding-stripped release. `git merge-file` instead keeps both
# sides or leaves visible markers).
# Strictly additive to the rerere policy:
#   * A file the driver merges cleanly exits 0, so git stages it and rerere
#     records nothing; a file it leaves conflicted (markers) falls through to
#     rerere exactly as before.
#   * `git merge-file` is a git builtin -- always present, no install step, no
#     fail-open path.
# Globs cover the C++ tree plus the non-C++ files MG commits routinely touch
# next to upstream churn (build lists, lang strings, .style, CI recipes). The
# version files (.rc / AppxManifest.xml / version) are deliberately NOT listed:
# the resolve loop re-derives them from the index stages with
# `merge-file --theirs`, independent of whatever the driver did.
# The attributes are repo-scoped and uncommitted (.git/info/attributes, rebuilt
# per run like the rr-cache): the tree tracks upstream, so a committed
# .gitattributes would itself be a rebase-conflict surface.
configure_merge_driver() {
	git config merge.xmerge.name "git merge-file (xdiff three-way)"
	# %A current/ours (edited in place = output), %O base, %B theirs; -L labels
	# match the diff3 marker order; exit code is the conflict count (0 = clean).
	git config merge.xmerge.driver \
		'git merge-file --diff3 -L ours -L base -L theirs %A %O %B'
	local attrs="${repo_root}/.git/info/attributes"
	mkdir -p "${repo_root}/.git/info" || return 0
	if ! grep -q 'merge=xmerge' "$attrs" 2>/dev/null; then
		{
			echo '*.cpp         merge=xmerge'
			echo '*.h           merge=xmerge'
			echo '*.hpp         merge=xmerge'
			echo '*.cc          merge=xmerge'
			echo '*.hh          merge=xmerge'
			echo '*.cxx         merge=xmerge'
			echo '*.hxx         merge=xmerge'
			echo 'CMakeLists.txt merge=xmerge'
			echo '*.cmake       merge=xmerge'
			echo '*.style       merge=xmerge'
			echo '*.strings     merge=xmerge'
			echo '*.yaml        merge=xmerge'
			echo '*.yml         merge=xmerge'
			echo 'Dockerfile    merge=xmerge'
			echo '*.md          merge=xmerge'
		} >> "$attrs"
	fi
	log "registered git merge-file (xdiff) merge driver for C++ and conflict-prone text files"
}

setup() {
	git config user.name "$MIRROR_GIT_NAME"
	git config user.email "$MIRROR_GIT_EMAIL"
	git config rerere.enabled true
	git config rerere.autoUpdate true
	# diff3 conflict style: the merge driver renders its markers with --diff3, so
	# matching the global style keeps conflict output consistent. rerere normalises
	# the preimage, so diff3 does not perturb seed matching.
	git config merge.conflictstyle diff3
	git config advice.detachedHead false
	configure_merge_driver

	# Overlay the committed rerere seed onto the live cache (which the Actions
	# cache restores between runs) so the maintainer's recorded resolutions
	# always replay. Done unconditionally, not just when the cache is cold: a
	# failed run saves a preimage-only entry for the unresolved conflict, and a
	# cold-only seed would then be shadowed by that warm-but-incomplete cache --
	# the seed's postimage must win so the next run resolves instead of aborting.
	if [ -d "$seed_dir" ]; then
		mkdir -p "${repo_root}/.git/rr-cache"
		cp -a "${seed_dir}/." "${repo_root}/.git/rr-cache/" 2>/dev/null || true
		log "seeded rerere cache from .github/rerere-seed"
	fi

	if git remote get-url upstream >/dev/null 2>&1; then
		git remote set-url upstream "$UPSTREAM_URL"
	else
		git remote add upstream "$UPSTREAM_URL"
	fi
	log "fetching upstream ($UPSTREAM_REPO)"
	git fetch --quiet --tags --force upstream
	# Refresh refs/remotes/origin/dev explicitly (a bare `git fetch origin dev`
	# would only move FETCH_HEAD), so the rebase always uses the live dev tip.
	git fetch --quiet --force origin dev:refs/remotes/origin/dev

	# Push over HTTPS with the PAT so the tag push triggers release.yml.
	git remote set-url --push origin \
		"https://x-access-token:${MIRROR_TOKEN}@github.com/${GH_REPO}.git"
}

# prerelease flag (true/false) of an upstream tag, via the Releases API.
upstream_prerelease() {
	gh api "repos/${UPSTREAM_REPO}/releases/tags/$1" --jq '.prerelease' 2>/dev/null
}

# BetaChannel value (0/1) recorded in the upstream tag's tree.
tree_betachannel() {
	git show "$1:Telegram/build/version" 2>/dev/null \
		| awk '/^BetaChannel/{print $2; exit}'
}

rebase_in_progress() {
	[ -d "${repo_root}/.git/rebase-merge" ] || [ -d "${repo_root}/.git/rebase-apply" ]
}

# Write a markdown report of the currently-paused conflict into
# $conflict_report. Must run before any resolution attempt (resolving destroys
# the markers the report quotes).
write_conflict_report() { # <onto-description>
	{
		echo "Rebase of the MG commits onto \`$1\` stopped on:"
		echo
		echo '```'
		git log -1 --format='%h %s' REBASE_HEAD 2>/dev/null || echo '(unknown commit)'
		echo '```'
		echo
		echo 'Conflicted files:'
		git diff --name-only --diff-filter=U | sed 's/^/- `/; s/$/`/'
		echo
		echo '<details><summary>Conflict hunks</summary>'
		echo
		echo '```diff'
		# Combined diff of the unmerged paths; capped so a pathological
		# conflict cannot blow the issue-body size limit.
		git diff --diff-filter=U | head -c 60000
		echo '```'
		echo '</details>'
		echo
		echo "Fix \`dev\` (resolve on a rebase onto the tag, or record a rerere seed per \`.github/MIRROR.md\`), push it, then close this issue to re-arm the mirror."
	} >"$conflict_report"
}

# A commit whose every change already landed upstream (e.g. an upstream-compat
# fix that upstream later fixed themselves) resolves to nothing: during a
# rebase "ours" is the upstream side, so take it for every conflicted path and
# drop the commit iff that leaves the whole commit empty. Anything still
# staged (a hunk that did apply, or a clean file from the same commit) means
# the commit still carries MG content -- not superseded, caller aborts.
try_drop_superseded() {
	git diff --name-only -z --diff-filter=U \
		| xargs -0 -r git checkout --ours -- 2>/dev/null || return 1
	git diff --name-only -z --diff-filter=U \
		| xargs -0 -r git add -- 2>/dev/null || return 1
	[ -z "$(git diff --name-only --diff-filter=U)" ] || return 1
	git diff --cached --quiet || return 1
	log "commit $(git log -1 --format='%h (%s)' REBASE_HEAD 2>/dev/null) is fully contained in upstream; dropping it"
	# --skip exits non-zero whenever the rebase pauses again on a later commit,
	# which is normal here -- the drive loop handles the next stop. A skip that
	# genuinely went nowhere leaves the same conflict in place and the loop's
	# convergence guard aborts it.
	GIT_EDITOR=true git rebase --skip >/dev/null 2>&1 || true
}

# Replay the MG commits onto the upstream target (args: <onto> <mg_base>),
# driving the rebase to completion. Returns 0 on a finished rebase, 1 on abort
# (with $conflict_report describing the conflict).
#
# The loop is driven by "is a rebase still in progress", NOT by the presence of
# unmerged paths: with rerere.autoUpdate a replayed resolution is *staged*, so
# `--diff-filter=U` is empty while the rebase is still paused -- keying on U
# would abort a perfectly resolved rebase. Each iteration:
#   - resolves version-file conflicts deterministically (merge-file --theirs),
#     keeping upstream's non-conflicting changes; their stale numbers are
#     overwritten later by set_release_version.py. NOTE: --theirs takes MG's
#     whole side of each *conflicting hunk*, so an upstream change that lands on
#     a line inside such a hunk (rare for these files) would be dropped;
#     non-conflicting upstream hunks are preserved.
#   - for any other unmerged path (left to rerere; if rerere did not stage it,
#     it shows up here): first checks whether the whole commit was superseded
#     upstream and can be dropped (try_drop_superseded), else aborts -> the
#     caller files a conflict issue and the run fails;
#   - then advances with `git rebase --continue`, skipping a commit only when it
#     has become genuinely empty (nothing unmerged and nothing staged).
drive_rebase() {
	local up="$1" mg_base="$2" tmp f had_bad guard=0
	git rebase --rerere-autoupdate --onto "$up" "$mg_base" >/dev/null 2>&1 || true
	tmp="$(mktemp -d)"
	while rebase_in_progress; do
		guard=$((guard + 1))
		if [ "$guard" -gt 100 ]; then
			err "rebase onto $up did not converge after $guard steps"
			rm -rf "$tmp"; git rebase --abort 2>/dev/null || true; return 1
		fi
		had_bad=0
		while IFS= read -r -d '' f; do
			case " $VERSION_FILES " in
				*" $f "*)
					git show ":1:$f" >"$tmp/base" 2>/dev/null || : >"$tmp/base"
					git show ":2:$f" >"$tmp/ours"
					git show ":3:$f" >"$tmp/theirs"
					git merge-file -p --theirs "$tmp/ours" "$tmp/base" "$tmp/theirs" >"$f"
					git add "$f"
					;;
				*)
					err "unresolved non-version conflict in $f"
					had_bad=1
					;;
			esac
		done < <(git diff --name-only -z --diff-filter=U)
		if [ "$had_bad" = 1 ]; then
			write_conflict_report "$up"
			if try_drop_superseded; then
				continue
			fi
			rm -rf "$tmp"; git rebase --abort 2>/dev/null || true; return 1
		fi
		if ! GIT_EDITOR=true git rebase --continue >/dev/null 2>&1; then
			# --continue refused. If nothing is unmerged and nothing is staged,
			# the commit became empty against the new base -> skip it. Otherwise
			# (new conflicts, or rerere-staged changes) just loop and continue.
			if rebase_in_progress \
				&& [ -z "$(git diff --name-only --diff-filter=U)" ] \
				&& git diff --cached --quiet; then
				GIT_EDITOR=true git rebase --skip >/dev/null 2>&1 || {
					err "rebase onto $up is stuck (cannot continue or skip)"
					rm -rf "$tmp"; git rebase --abort 2>/dev/null || true; return 1
				}
			fi
		fi
	done
	rm -rf "$tmp"
	return 0
}

# mirror_one <upstream-tag> <expect_beta 0|1>
mirror_one() {
	local up="$1" expect_beta="$2"
	[ -n "$up" ] || return 0
	if ! printf '%s' "$up" | grep -Eq '^v[0-9]+\.[0-9]+\.[0-9]+$'; then
		err "skipping non-release tag: $up"
		return 0
	fi

	local suffix="" ; [ "$expect_beta" = 1 ] && suffix="-beta"
	local our_tag="${up}.1${suffix}"
	local branch="release/${up#v}"
	local issue_title="mirror conflict: ${our_tag}"

	if [ -z "$FORCE_TAG" ] \
		&& git ls-remote --tags origin "refs/tags/${our_tag}" | grep -q .; then
		log "already mirrored: $our_tag (skip)"
		return 0
	fi

	# A previous run already failed on this tag and filed the details; do not
	# burn a failure email every 6 hours on the same conflict. Fixing dev and
	# closing the issue (or a force_tag dispatch) re-arms the mirror.
	if [ -z "$FORCE_TAG" ] && [ -n "$(open_issue_number "$issue_title")" ]; then
		log "open conflict issue for ${our_tag}; skipping until it is closed"
		return 0
	fi

	# Guard: the upstream tree's BetaChannel must agree with the API channel.
	local bc; bc="$(tree_betachannel "$up")"
	if [ -n "$bc" ] && [ "$bc" != "$expect_beta" ]; then
		err "channel mismatch for $up (API beta=$expect_beta, tree BetaChannel=$bc)"
		return 1
	fi

	# The MG commit set is everything on dev past its fork point from upstream.
	# This is only correct while dev stays linearly ahead of upstream/dev; a
	# merge commit in the range means upstream was merged (not rebased) into dev
	# and the range would balloon with upstream commits -- refuse rather than
	# replay hundreds of commits onto the tag.
	local mg_base; mg_base="$(git merge-base origin/dev upstream/dev || true)"
	if [ -z "$mg_base" ]; then
		err "no merge-base between origin/dev and upstream/dev"
		return 1
	fi
	if [ -n "$(git rev-list --merges "${mg_base}..origin/dev")" ]; then
		err "dev has merge commits past ${mg_base:0:10}; rebase it on upstream/dev first"
		return 1
	fi
	log "mirroring $up -> $our_tag (MG base ${mg_base:0:10}, $(git rev-list --count "${mg_base}..origin/dev") commits)"

	git checkout -B "$branch" origin/dev

	if ! drive_rebase "$up" "$mg_base"; then
		err "rebase of MG commits onto $up has unresolved conflicts"
		file_conflict_issue "$issue_title" "$conflict_report"
		git checkout -q dev || true
		return 1
	fi

	# Version-bump commit (overwrites the stale numbers left by the resolver).
	# A fresh rebase always changes the version, but fall back to an empty commit
	# so the tag still points at a "Version" commit if it somehow did not.
	python3 Telegram/build/set_release_version.py "$our_tag"
	local msg
	if [ "$expect_beta" = 1 ]; then
		msg="Beta version ${up#v}.1."
	else
		msg="Version ${up#v}.1."
	fi
	git commit --quiet --all --message "$msg" \
		|| git commit --quiet --allow-empty --message "$msg"
	git tag --annotate --force --message "Mercurygram ${our_tag}" "$our_tag"

	# Force-push: the release branch is mirror-managed and re-derived from a
	# fresh rebase each run (so a re-run after a partial failure has new SHAs).
	git push --force origin "$branch"
	git push --force origin "refs/tags/${our_tag}"
	log "pushed $branch and $our_tag (release.yml will build it)"

	close_conflict_issue "mirror conflict: ${our_tag}" \
		"Resolved: ${our_tag} mirrored successfully."

	git checkout -q dev || true
}

# Dry-run replay of the MG set onto the upstream/dev tip -- nothing is pushed
# or tagged. Upstream commits sit on dev for weeks before a release tag points
# at them, so this surfaces the conflicts the next tag will hit while they are
# still cheap to fix on dev. The rolling issue is the signal (opened/updated on
# conflict, closed on a clean replay); the run itself always exits 0 -- a
# canary conflict is advance warning, not a mirror failure.
canary_run() {
	local tip mg_base
	tip="$(git rev-parse --short upstream/dev)"
	mg_base="$(git merge-base origin/dev upstream/dev || true)"
	if [ -z "$mg_base" ]; then
		err "canary: no merge-base between origin/dev and upstream/dev"
		return 0
	fi
	log "canary: replaying $(git rev-list --count "${mg_base}..origin/dev") MG commits onto upstream/dev (${tip})"
	git checkout -q --detach origin/dev
	if drive_rebase "upstream/dev" "$mg_base"; then
		log "canary: clean replay onto upstream/dev (${tip})"
		close_conflict_issue "$CANARY_TITLE" \
			"Clean replay onto upstream/dev (\`${tip}\`)."
	else
		err "canary: replay onto upstream/dev (${tip}) has conflicts"
		sed -i "1s/^/(canary, upstream\\/dev at \`${tip}\`)\n\n/" "$conflict_report"
		file_conflict_issue "$CANARY_TITLE" "$conflict_report"
	fi
	git checkout -q dev || true
	return 0
}

main() {
	setup

	if [ "$CANARY" = 1 ]; then
		canary_run
		return $?
	fi

	local rc=0
	if [ -n "$FORCE_TAG" ]; then
		local pr beta=0
		pr="$(upstream_prerelease "$FORCE_TAG" || true)"
		if [ -n "$pr" ]; then
			[ "$pr" = true ] && beta=1
		else
			# No GitHub Release for the tag -> fall back to the tree's flag.
			[ "$(tree_betachannel "$FORCE_TAG")" = 1 ] && beta=1
		fi
		log "force-mirroring $FORCE_TAG (prerelease=${pr:-unknown}, beta=$beta)"
		mirror_one "$FORCE_TAG" "$beta" || rc=1
	else
		local releases latest_stable latest_beta
		releases="$(gh api "repos/${UPSTREAM_REPO}/releases?per_page=100")"
		latest_stable="$(printf '%s' "$releases" \
			| jq -r 'map(select(.draft==false and .prerelease==false))|.[0].tag_name // empty')"
		latest_beta="$(printf '%s' "$releases" \
			| jq -r 'map(select(.draft==false and .prerelease==true))|.[0].tag_name // empty')"
		log "latest upstream stable=${latest_stable:-none} beta=${latest_beta:-none}"
		mirror_one "$latest_stable" 0 || rc=1
		mirror_one "$latest_beta"   1 || rc=1
	fi
	return $rc
}

main "$@"
