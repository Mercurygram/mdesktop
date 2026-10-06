/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <optional>
#include <vector>

namespace MG {

// DuckDuckGo behind Mozhi answers 500 above about 1000 characters.
inline constexpr auto kMozhiChunkLimit = 900;

// Splits text into pieces of at most limit characters, cutting after the
// last newline, else after the last space, else hard. Joining the pieces
// gives back the input.
[[nodiscard]] QStringList SplitMozhiText(
	const QString &text,
	int limit = kMozhiChunkLimit);

// The translation from a Mozhi /api/translate reply: "translated-text", or
// "translation" as some forks name it. Nothing on bad JSON or empty text.
[[nodiscard]] std::optional<QString> ParseMozhiReply(const QByteArray &body);

// Engines take and return plain text, so entities are carried across as
// markers the engines leave alone. An atomic span (link, mention, code,
// custom emoji) becomes one marker and comes back with its source text
// untouched; a paired span (bold, italic, hidden link label) wraps its text
// in an open and a close marker, so that text is translated in place.
struct MozhiSpan {
	int offset = 0;
	int length = 0;
	bool atomic = false;
};

struct MozhiProtected {
	QString text;
	std::vector<MozhiSpan> spans;
	// Spans that got markers. A skipped one overlaps an atomic span: nested
	// fully inside it, it is still carried along with that span's text.
	std::vector<bool> used;
	// Nothing but markers and punctuation left, e.g. a message that is just
	// a link: engines fail on that, keep the source instead.
	bool nothingToTranslate = false;
};

[[nodiscard]] MozhiProtected ProtectMozhiSpans(
	const QString &text,
	std::vector<MozhiSpan> spans);

struct MozhiRestored {
	QString text;
	// Per input span: where it landed, or nothing if it was dropped.
	std::vector<std::optional<MozhiSpan>> placed;
};

// Nothing when an atomic marker went missing or was duplicated: the caller
// then falls back to StripMozhiMarkers(translated) without entities. A
// mangled paired marker only drops that span.
[[nodiscard]] std::optional<MozhiRestored> RestoreMozhiSpans(
	const MozhiProtected &prot,
	const QString &source,
	const QString &translated);

[[nodiscard]] QString StripMozhiMarkers(const QString &translated);

// DuckDuckGo joins all lines into one, so newlines go to it as markers.
// Decoding also drops the spaces engines put around a newline.
[[nodiscard]] QString EncodeMozhiNewlines(QString text);
[[nodiscard]] QString DecodeMozhiNewlines(const QString &translated);

} // namespace MG
