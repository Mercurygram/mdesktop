/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/mg_search_query.h"

#include <QtCore/QDate>
#include <QtCore/QDateTime>
#include <QtCore/QRegularExpression>
#include <QtCore/QStringList>

#include <limits>

namespace MG {
namespace {

[[nodiscard]] QDate ParseDay(const QString &value) {
	if (value == QStringLiteral("today")) {
		return QDate::currentDate();
	} else if (value == QStringLiteral("yesterday")) {
		return QDate::currentDate().addDays(-1);
	}
	return QDate::fromString(value, QStringLiteral("yyyy-MM-dd"));
}

// Local midnight as unix seconds, or 0 when it does not fit a TL date.
[[nodiscard]] int StartOfDay(QDate day) {
	const auto seconds = day.startOfDay().toSecsSinceEpoch();
	return (seconds > 0 && seconds <= std::numeric_limits<int>::max())
		? int(seconds)
		: 0;
}

[[nodiscard]] SearchType ParseType(const QString &value) {
	static const auto map = std::initializer_list<
		std::pair<QStringView, SearchType>>{
		{ u"photo", SearchType::Photo },
		{ u"video", SearchType::Video },
		{ u"voice", SearchType::Voice },
		{ u"round", SearchType::Round },
		{ u"music", SearchType::Music },
		{ u"gif", SearchType::Gif },
		{ u"file", SearchType::Document },
		{ u"doc", SearchType::Document },
		{ u"document", SearchType::Document },
		{ u"link", SearchType::Link },
		{ u"url", SearchType::Link },
		{ u"contact", SearchType::Contact },
		{ u"geo", SearchType::Geo },
		{ u"location", SearchType::Geo },
		{ u"poll", SearchType::Poll },
		{ u"mention", SearchType::Mention },
		{ u"pinned", SearchType::Pinned },
	};
	for (const auto &[name, type] : map) {
		if (value == name) {
			return type;
		}
	}
	return SearchType::None;
}

// True when the token was an operator and has been applied to the result.
bool ParseToken(
		SearchQuery &result,
		const QString &token,
		const std::function<bool(const QString &)> &usernameKnown) {
	const auto colon = token.indexOf(u':');
	if (colon <= 0 || colon == token.size() - 1) {
		return false;
	}
	const auto key = token.left(colon).toLower();
	const auto value = token.mid(colon + 1);
	if (key == QStringLiteral("from")) {
		static const auto username = QRegularExpression(
			QStringLiteral("^[A-Za-z0-9_]{1,32}$"));
		const auto name = value.startsWith(u'@') ? value.mid(1) : value;
		if (!username.match(name).hasMatch()
			|| !usernameKnown
			|| !usernameKnown(name)) {
			return false;
		}
		result.from = name;
		return true;
	} else if (key == QStringLiteral("before")
		|| key == QStringLiteral("after")
		|| key == QStringLiteral("date")) {
		const auto day = ParseDay(value.toLower());
		if (!day.isValid()) {
			return false;
		}
		const auto start = StartOfDay(day);
		const auto end = StartOfDay(day.addDays(1));
		if (!start || !end) {
			return false;
		}
		if (key == QStringLiteral("after")) {
			result.minDate = start;
		} else if (key == QStringLiteral("before")) {
			result.maxDate = start;
		} else {
			result.minDate = start;
			result.maxDate = end;
		}
		return true;
	} else if (key == QStringLiteral("type")) {
		const auto type = ParseType(value.toLower());
		if (type == SearchType::None) {
			return false;
		}
		result.type = type;
		return true;
	}
	return false;
}

} // namespace

SearchQuery ParseSearchQuery(
		const QString &query,
		const std::function<bool(const QString &)> &usernameKnown) {
	auto result = SearchQuery{ .text = query };
	const auto trimmed = query.trimmed();
	if (trimmed.startsWith(u'#') || trimmed.startsWith(u'$')) {
		return result;
	}
	auto rest = QStringList();
	for (const auto &token : trimmed.split(
			QRegularExpression(QStringLiteral("\\s+")),
			Qt::SkipEmptyParts)) {
		if (!ParseToken(result, token, usernameKnown)) {
			rest.push_back(token);
		}
	}
	result.text = rest.join(u' ');
	return result;
}

} // namespace MG
