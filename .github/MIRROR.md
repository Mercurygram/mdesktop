# Upstream release mirroring

`upstream-mirror.yml` watches [telegramdesktop/tdesktop][up] and turns its
releases into Mercurygram releases automatically.

| upstream release      | what we push          | triggers      |
| --------------------- | --------------------- | ------------- |
| stable `vX.Y.Z`       | tag `vX.Y.Z.1`        | `release.yml` |
| pre-release `vX.Y.Z`  | tag `vX.Y.Z.1-beta`   | `release.yml` |

`dev` is the only branch the mirror pushes; there is no per-release branch. The
cut off the **newest** upstream release of either channel is force-pushed to
`dev`, so `dev` always sits on the newest upstream base with its release tag on
the tip and `git describe --tags` resolves to the real Mercurygram version --
what distro `-git` packaging needs. The other channel's cut is tagged with no
branch pointing at it; its commits stay reachable through the tag alone.

Each run mirrors only the **latest** unmirrored stable and the **latest**
unmirrored pre-release (older ones are not backfilled). Stable vs pre-release is
read from the GitHub Releases `prerelease` flag and cross-checked against
`BetaChannel` in the tag's `Telegram/build/version`.

## What a mirror produces

For upstream `vX.Y.Z` it produces a history shaped like:

```
<upstream vX.Y.Z>
  + [MG] Mercurygram branding
  + [MG] Autoupdate from GitHub Releases
  + [MG] CI: GitHub Releases pipeline, ...
  + [MG] Add secret chat ...
  + export: avoid -Wtype-limits ...
  + Version X.Y.Z.1[ beta]        <- set_release_version.py, tag points here
```

Linear, **no merge commits**, MG commits last. For the newest upstream release
this *is* `dev`, tip included; for the other channel it is tagged and nothing
else.

The MG commit set is `${MG_BASE}..${MG_SRC}`, resolved once per run:

- `MG_SRC` is `origin/dev` with the version commit(s) the previous mirror left
  on its tip stripped off. `dev` carries its release's `Version X.Y.Z.1` commit
  as its tip, so without the strip they would stack up one per release. The
  pattern matched is the 4-part fork grammar (`Version 7.2.8.1.`); upstream's
  own `Version 7.2.8: <summary>` commits cannot match it.
- `MG_BASE` is `git merge-base "$MG_SRC" upstream/dev` -- every commit on `dev`
  that is not in upstream's `dev` and is not a version bump.

Resolving both once matters: as soon as one leg force-pushes `dev`,
`refs/remotes/origin/dev` is stale for the next leg, and both legs must replay
the same commits anyway.

Keep `dev` rebased on upstream so that stays true.

## One-time setup

Add in the repo's **Settings → Secrets and variables → Actions**:

- Secret **`MIRROR_TOKEN`** — a PAT (classic: `repo` + `workflow` scopes; or a
  fine-grained token with `contents: write` **and** `workflows: write`). Required
  because the MG commits modify `.github/workflows/release.yml`, and because a
  tag pushed by the default `GITHUB_TOKEN` does **not** trigger `release.yml`.
- Variables **`MIRROR_GIT_NAME`** / **`MIRROR_GIT_EMAIL`** (optional) — author of
  the `Version X.Y.Z.1` commit.

## Conflict handling

Rebasing the MG commits onto a newer upstream tag conflicts in two ways:

1. **Version files** (`version`, `version.h`, the two `.rc`, `AppxManifest.xml`).
   The branding commit renames product strings on lines next to the version
   numbers upstream bumps, so they always collide. Resolved automatically with
   `git merge-file --theirs` (keep branding, keep upstream's other changes); the
   stale numbers are overwritten by `set_release_version.py`. No action needed.

2. **Anything else** (e.g. a source file the secret-chat commit and upstream both
   changed). Left to **rerere**. If rerere has no recorded resolution, the rebase
   is aborted, nothing is pushed, and the job fails — you get the standard Actions
   failure email. Resolve it once and teach rerere (below); the next upstream bump
   replays it.

### Seeding / updating the rerere cache

The cache is persisted between runs via the Actions cache, and seeded at job
start from the committed `.github/rerere-seed/` if present. To record a
resolution durably:

```sh
git config rerere.enabled true
# dev's tip is its release's version commit; drop it, the mirror re-cuts its own.
src=origin/dev
git log -1 --format=%s $src | grep -Eq '^(Beta version|Version) [0-9]+(\.[0-9]+){3}\.$' && src=origin/dev^
git checkout -B tmp $src
git rebase --onto vX.Y.Z "$(git merge-base $src upstream/dev)"
# resolve the conflict, git add, then:
git rebase --continue
cp -a .git/rr-cache/. .github/rerere-seed/      # commit this on dev
git checkout dev && git branch -D tmp
```

## Manual / forced re-mirror

Run the **Upstream mirror** workflow via *Run workflow* with `force_tag` set to an
upstream tag (e.g. `v6.8.5`). This bypasses the latest-only and idempotency
checks and force-replaces that release's tag. The cut is re-derived from
scratch: the mirror replays the MG commits onto the upstream tag, so a re-cut
picks up whatever is on `dev` at that moment.

`dev` moves only when `force_tag` is itself the newest upstream release, so
re-cutting an older base never drags `dev` backwards.

## Disabling upstream workflows

A mirror pulls in upstream's ~18 workflow files. `disable-upstream-workflows.sh`
runs before and after the push and disables every workflow whose file is not in
the keep-list (`release.yml`, `upstream-mirror.yml`) via `gh workflow disable` —
a settings change, so it persists and never edits the workflow files. Add a new
Mercurygram-owned workflow? Add its filename to `KEEP` in that script.

[up]: https://github.com/telegramdesktop/tdesktop
