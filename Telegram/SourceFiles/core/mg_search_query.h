/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QString>

#include <functional>

namespace MG {

// Typed operators for the in-chat search field, the same grammar the Android
// client accepts. "from:alice type:photo after:2026-01-01 cake" becomes the
// text "cake" plus the messages.search fields in-chat search never fills in:
// from_id, filter and min_date / max_date. Only what the server can evaluate
// is supported.
//
// Whitespace-separated key:value tokens, keys and keyword values
// case-insensitive, no quoting:
//   from:username or from:@username (only peers already known locally)
//   before:D / after:D / date:D with D one of YYYY-MM-DD, today, yesterday;
//     local midnight, "after" includes the day, "before" excludes it, "date"
//     is the one-day range
//   type:X with X one of photo, video, voice, round, music, gif,
//     file/doc/document, link/url, contact, geo/location, poll, mention,
//     pinned
// The last token of each kind wins. A token that does not parse stays in the
// text verbatim, so a literal "from:x" can still be searched for. Hashtag and
// cashtag queries pass through untouched.
enum class SearchType {
	None,
	Photo,
	Video,
	Voice,
	Round,
	Music,
	Gif,
	Document,
	Link,
	Contact,
	Geo,
	Poll,
	Mention,
	Pinned,
};

struct SearchQuery {
	QString text;
	QString from; // Username without '@', empty when not given.
	SearchType type = SearchType::None;
	int minDate = 0;
	int maxDate = 0;
};

// A from: whose username is not known stays in the text: searching every
// sender instead would silently widen the results.
[[nodiscard]] SearchQuery ParseSearchQuery(
	const QString &query,
	const std::function<bool(const QString &username)> &usernameKnown);

} // namespace MG
