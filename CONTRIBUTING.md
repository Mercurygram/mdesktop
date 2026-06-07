# Contributing to Mercurygram Desktop

Thanks for considering a contribution. Mercurygram Desktop is a rebase fork of
upstream Telegram Desktop. Read [`AGENTS.md`](AGENTS.md) before you start — the
rebase workflow shapes how patches must be structured.

## Translations

Mercurygram-only strings (those introduced by `[MG]` commits) are translated
in-repo and land via GitHub pull requests. There is no Crowdin / Weblate /
Transifex.

All **other** Telegram Desktop strings are translated by Telegram's own cloud
language packs, fetched at runtime — they are not in this repository and cannot
be changed here. Only the Mercurygram-only keys can be.

### What needs translating

The MG-only keys are prefixed `lng_mg_` and their English source values live in
[`Telegram/Resources/langs/lang.strings`](Telegram/Resources/langs/lang.strings)
(search for `lng_mg_`). Because these keys do not exist on Telegram's language
servers, the cloud lang pack never translates them; instead Mercurygram ships
per-locale overlay files in
[`Telegram/Resources/langs/mercurygram/`](Telegram/Resources/langs/mercurygram)
and applies them onto the active language at runtime.

Run the helper to see which keys are still missing in each locale:

```sh
./scripts/check-mg-translations.sh
```

### How to translate

1. Fork the repo, branch off `dev`.
2. Edit `Telegram/Resources/langs/mercurygram/<locale>.strings` (create it if it
   doesn't exist). Add the missing `"lng_mg_…" = "…";` entries — the script
   output lists the key names to copy.
3. Use the English value in `Telegram/Resources/langs/lang.strings` as the
   source of truth.
4. Mind the `.strings` format:
   - One `"key" = "value";` entry per line; no `/* … */` comments (the runtime
     parser does not accept them).
   - Escape inner double quotes as `\"` (e.g.
     `"lng_mg_hide_all_chats" = "Nascondi la scheda \"Tutte\"";`). Native
     typographic quotes (`«…»`, `„…“`) need no escaping.
   - Preserve any `{placeholder}` tags verbatim.
   - Files are UTF-8.
   - Keep proper nouns untranslated: `Mercurygram`.
5. If you add a brand-new locale, also add its file to
   [`Telegram/Resources/qrc/telegram/mercurygram_langs.qrc`](Telegram/Resources/qrc/telegram/mercurygram_langs.qrc)
   so it gets bundled. Use the lowercase Telegram language id as the filename
   (e.g. `pt-br.strings`). Only locales that already exist upstream make sense —
   otherwise the ~10k upstream strings stay English and only the MG screens get
   translated.
6. Re-run `./scripts/check-mg-translations.sh` — your locale should show no
   missing keys.
7. Open a PR titled `[MG] translations(<locale>): <short note>`.

You do **not** need to know which `[MG] …` commit introduced a string. The
maintainer folds translation PRs into the originating commit before the next
upstream rebase, to keep the patch series small (see [`AGENTS.md`](AGENTS.md),
"Keep the commit count low").

Non-git contributors can use the
[translation issue form](.github/ISSUE_TEMPLATE/translation.yml) instead.

### Translation status

Italian (`it`) was reviewed by a native speaker. The other shipped locales
(`de`, `es`, `ko`, `nl`, `pt-br`, `ru`, `uk`, `ar`) were AI-seeded and are
explicitly considered drafts awaiting native-speaker review. Corrections via PR
are very welcome — don't assume anything is locked in.

## Code contributions

For non-translation changes, follow the conventions in [`AGENTS.md`](AGENTS.md) —
tag commits `[MG]`, prefer new files over editing heavily-modified upstream
files, and fold fixes for an existing `[MG]` feature into the introducing commit
rather than adding follow-up commits.
