/*
This file is part of Mercurygram Desktop,
a privacy and security focused fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Lang {

// Returns the bundled Mercurygram (lng_mg_*) translation file for the given
// language id, in the desktop .strings format ("key" = "value";), or an empty
// QByteArray when no translation ships for it.
//
// These keys do not exist on Telegram's language-pack servers, so they are
// never delivered by the cloud lang pack and would otherwise stay English.
// We ship them in-repo (Resources/langs/mercurygram/<id>.strings) and overlay
// them onto the active language at runtime.
//
// `id` is tried first, then `baseId` (e.g. "pt-br" then "pt").
[[nodiscard]] QByteArray MercurygramOverlay(
	const QString &id,
	const QString &baseId);

} // namespace Lang
