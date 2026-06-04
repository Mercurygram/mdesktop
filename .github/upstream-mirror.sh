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
#  - Any *other* conflict is left to rerere (a persisted resolution cache). If
#    rerere cannot resolve it, the rebase is aborted and the script fails, so the
#    Actions run emails the maintainer instead of pushing a broken tag.
#
# Required environment:
#   MIRROR_TOKEN      PAT with contents:write AND workflows:write on GH_REPO
#                     (workflows scope is required because the MG commits touch
#                     .github/workflows/release.yml; without it the tag push is
#                     rejected, and the default GITHUB_TOKEN would not trigger
#                     release.yml anyway).
#   GH_REPO           owner/name of this fork (e.g. Mercurygram/mdesktop).
#   GH_TOKEN          token gh uses to read the upstream releases API.
# Optional:
#   UPSTREAM_REPO     default telegramdesktop/tdesktop
#   UPSTREAM_URL      default https://github.com/<UPSTREAM_REPO>.git
#   MIRROR_GIT_NAME   author/committer for the version commit (default below)
#   MIRROR_GIT_EMAIL  ditto
#   FORCE_TAG         re-mirror exactly this upstream tag (e.g. v6.8.5),
#                     bypassing the latest-only and idempotency checks and
#                     force-replacing its release branch + tag.

set -euo pipefail

UPSTREAM_REPO="${UPSTREAM_REPO:-telegramdesktop/tdesktop}"
UPSTREAM_URL="${UPSTREAM_URL:-https://github.com/${UPSTREAM_REPO}.git}"
MIRROR_GIT_NAME="${MIRROR_GIT_NAME:-Mercurygram CI}"
MIRROR_GIT_EMAIL="${MIRROR_GIT_EMAIL:-ci@mercurygram.local}"
FORCE_TAG="${FORCE_TAG:-}"

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

repo_root="$(git rev-parse --show-toplevel)"
cd "$repo_root"
seed_dir="${repo_root}/.github/rerere-seed"

log() { printf '>> %s\n' "$*"; }
err() { printf '!! %s\n' "$*" >&2; }

setup() {
	git config user.name "$MIRROR_GIT_NAME"
	git config user.email "$MIRROR_GIT_EMAIL"
	git config rerere.enabled true
	git config rerere.autoUpdate true
	git config advice.detachedHead false

	# Seed the rerere cache from the committed seed if the live cache is cold,
	# so resolutions the maintainer recorded earlier replay automatically.
	if [ -d "$seed_dir" ] && [ -z "$(ls -A "${repo_root}/.git/rr-cache" 2>/dev/null || true)" ]; then
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

# Replay the MG commits onto the upstream tag (args: <tag> <mg_base>), driving
# the rebase to completion. Returns 0 on a finished rebase, 1 on abort.
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
#   - bails on any other unmerged path (left to rerere; if rerere did not stage
#     it, it shows up here and we abort -> the run fails and emails the maintainer);
#   - then advances with `git rebase --continue`, skipping a commit only when it
#     has become genuinely empty (nothing unmerged AND nothing staged).
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

	if [ -z "$FORCE_TAG" ] \
		&& git ls-remote --tags origin "refs/tags/${our_tag}" | grep -q .; then
		log "already mirrored: $our_tag (skip)"
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

	git checkout -q dev || true
}

main() {
	setup

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
