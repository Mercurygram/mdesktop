# Upstream release mirroring

`upstream-mirror.yml` watches [telegramdesktop/tdesktop][up] and turns its
releases into Mercurygram releases automatically.

| upstream release      | what we push          | triggers      |
| --------------------- | --------------------- | ------------- |
| stable `vX.Y.Z`       | tag `vX.Y.Z.1`        | `release.yml` |
| pre-release `vX.Y.Z`  | tag `vX.Y.Z.1-beta`   | `release.yml` |

Each run mirrors only the **latest** unmirrored stable and the **latest**
unmirrored pre-release (older ones are not backfilled). Stable vs pre-release is
read from the GitHub Releases `prerelease` flag and cross-checked against
`BetaChannel` in the tag's `Telegram/build/version`.

## What a mirror produces

For upstream `vX.Y.Z` it creates a `release/X.Y.Z` branch shaped like:

```
<upstream vX.Y.Z>
  + [MG] Mercurygram branding
  + [MG] Autoupdate from GitHub Releases
  + [MG] CI: GitHub Releases pipeline, ...
  + [MG] Add secret chat ...
  + export: avoid -Wtype-limits ...
  + Version X.Y.Z.1[ beta]        <- set_release_version.py, tag points here
```

Linear, **no merge commits**, MG commits last. The MG commit set is exactly
`git merge-base origin/dev upstream/dev .. origin/dev` (every commit on `dev`
not in upstream's `dev`). Keep `dev` rebased on upstream so that stays true.

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
   changed). For C++ sources/headers the mirror script registers a merge driver
   that resolves the content merge with git's own three-way `git merge-file`
   (xdiff) instead of the rebase's default `ort` strategy. This is required, not
   cosmetic: ort's internal merge is *zealous* and, for an MG hunk that lands next
   to an upstream change (the usual case once upstream relocates the code an MG
   patch hooks into), can silently resolve the whole file to the upstream side —
   dropping the MG insertion with **no conflict markers**. `git merge-file` keeps
   both sides' non-overlapping changes (so the MG hook lands on the relocated
   upstream code) and emits markers only where the two genuinely overlap. A clean
   driver merge exits 0 and git stages it; an overlap leaves markers and falls
   through to **rerere**. If rerere has no recorded resolution either, the rebase
   is aborted, nothing is pushed, and the job fails — you get the standard Actions
   failure email. Resolve it once and teach rerere (below); the next upstream bump
   replays it.

   > An earlier revision used the **mergiraf** syntax-aware driver here. It kept
   > the MG hooks but reordered class members — it sank `ApiWrap`'s
   > `using SharedMediaType` alias below the structs that use it, producing an
   > uncompilable `apiwrap.h`. `git merge-file` preserves declaration order, needs
   > no external binary, and is always present.

### Seeding / updating the rerere cache

The cache is persisted between runs via the Actions cache, and the committed
`.github/rerere-seed/` is overlaid onto it at job start (unconditionally, so a
recorded resolution wins even over a warm cache that a previous failed run left
holding only the unresolved preimage).

rerere stores a *normalised* preimage, so a seed is independent of
`merge.conflictstyle` and of which merge produced the conflict. Record under the
same setup the mirror uses — git's `merge-file` driver for C++ and
`merge.conflictStyle = diff3` — so the recorded preimage matches the CI run. The
helper below reproduces that setup; CMake/`.txt`/non-C++ files have no driver and
go through git's own merge on both sides, so their seeds need nothing special.

To record a resolution durably:

```sh
git config rerere.enabled true
git config merge.conflictStyle diff3
# Drive C++ conflicts through git merge-file (xdiff), matching the mirror:
git config merge.xmerge.driver 'git merge-file --diff3 -L ours -L base -L theirs %A %O %B'
printf '*.cpp merge=xmerge\n*.h merge=xmerge\n*.hpp merge=xmerge\n*.cc merge=xmerge\n*.hh merge=xmerge\n*.cxx merge=xmerge\n*.hxx merge=xmerge\n' >> .git/info/attributes
git checkout -B tmp origin/dev
git rebase --onto vX.Y.Z "$(git merge-base origin/dev upstream/dev)"
# resolve the conflict, git add, then:
git rebase --continue
cp -a .git/rr-cache/. .github/rerere-seed/      # commit this on dev
git checkout dev && git branch -D tmp
```

## Manual / forced re-mirror

Run the **Upstream mirror** workflow via *Run workflow* with `force_tag` set to an
upstream tag (e.g. `v6.8.5`). This bypasses the latest-only and idempotency
checks and force-replaces that release's branch and tag.

## Disabling upstream workflows

A mirror pulls in upstream's ~18 workflow files. `disable-upstream-workflows.sh`
runs before and after the push and disables every workflow whose file is not in
the keep-list (`release.yml`, `upstream-mirror.yml`) via `gh workflow disable` —
a settings change, so it persists and never edits the workflow files. Add a new
Mercurygram-owned workflow? Add its filename to `KEEP` in that script.

[up]: https://github.com/telegramdesktop/tdesktop
