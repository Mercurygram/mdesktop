/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/

// Unit tests for the in-chat search operators, the same cases the Android
// client checks. Same tiny self-contained harness as test_mg_unicode_fold.
//
// Built only with -DDESKTOP_APP_TEST_APPS=ON; run the produced
// ./test_mg_search_query.

#include "core/mg_search_query.h"

#include <QtCore/QDate>
#include <QtCore/QDateTime>
#include <QtCore/QString>

#include <iostream>

namespace {

int gChecks = 0;
int gFailures = 0;

void Check(bool condition, const QString &what) {
	++gChecks;
	if (!condition) {
		++gFailures;
		std::cerr << "  FAIL: " << what.toStdString() << std::endl;
	}
}

MG::SearchQuery Parse(const QString &query) {
	// "alice" and "alice_bob" are the only usernames this session knows.
	return MG::ParseSearchQuery(query, [](const QString &username) {
		return username.compare(QStringLiteral("alice"), Qt::CaseInsensitive) == 0
			|| username == QStringLiteral("alice_bob");
	});
}

int Midnight(int year, int month, int day) {
	return int(QDate(year, month, day).startOfDay().toSecsSinceEpoch());
}

void TestPlainText() {
	const auto parsed = Parse(QStringLiteral("just some words"));
	Check(parsed.text == QStringLiteral("just some words"), "plain text");
	Check(parsed.type == MG::SearchType::None, "plain no type");
	Check(parsed.from.isEmpty(), "plain no from");
	Check(!parsed.minDate && !parsed.maxDate, "plain no dates");
}

void TestTypes() {
	const auto is = [](const char *query, MG::SearchType type) {
		Check(
			Parse(QString::fromLatin1(query)).type == type,
			QString::fromLatin1(query));
	};
	using Type = MG::SearchType;
	is("type:photo", Type::Photo);
	is("type:video", Type::Video);
	is("type:voice", Type::Voice);
	is("type:round", Type::Round);
	is("type:music", Type::Music);
	is("type:gif", Type::Gif);
	is("type:file", Type::Document);
	is("type:doc", Type::Document);
	is("type:document", Type::Document);
	is("type:link", Type::Link);
	is("type:url", Type::Link);
	is("type:contact", Type::Contact);
	is("type:geo", Type::Geo);
	is("type:location", Type::Geo);
	is("type:poll", Type::Poll);
	is("type:mention", Type::Mention);
	is("type:pinned", Type::Pinned);
}

void TestOperatorsStripped() {
	const auto parsed = Parse(
		QStringLiteral("cake from:alice type:photo after:2026-01-01"));
	Check(parsed.text == QStringLiteral("cake"), "combo text");
	Check(parsed.from == QStringLiteral("alice"), "combo from");
	Check(parsed.type == MG::SearchType::Photo, "combo type");
	Check(parsed.minDate == Midnight(2026, 1, 1), "combo after");
}

void TestFrom() {
	Check(
		Parse(QStringLiteral("from:@alice_bob")).from
			== QStringLiteral("alice_bob"),
		"from strips @");
	const auto unknown = Parse(QStringLiteral("from:carol"));
	Check(unknown.from.isEmpty(), "unknown from not applied");
	Check(
		unknown.text == QStringLiteral("from:carol"),
		"unknown from stays literal");
}

void TestCaseInsensitive() {
	const auto parsed = Parse(QStringLiteral("FROM:Alice TYPE:Photo"));
	Check(parsed.from == QStringLiteral("Alice"), "FROM key");
	Check(parsed.type == MG::SearchType::Photo, "TYPE key");
}

void TestDates() {
	const auto range = Parse(
		QStringLiteral("after:2026-01-01 before:2026-06-15"));
	Check(range.minDate == Midnight(2026, 1, 1), "after");
	Check(range.maxDate == Midnight(2026, 6, 15), "before");

	const auto day = Parse(QStringLiteral("date:2026-03-10"));
	Check(day.minDate == Midnight(2026, 3, 10), "date min");
	Check(day.maxDate == Midnight(2026, 3, 11), "date max");

	const auto today = QDate::currentDate();
	const auto todayStart = int(today.startOfDay().toSecsSinceEpoch());
	Check(
		Parse(QStringLiteral("after:today")).minDate == todayStart,
		"today");
	const auto yesterday = Parse(QStringLiteral("date:yesterday"));
	Check(
		yesterday.minDate
			== int(today.addDays(-1).startOfDay().toSecsSinceEpoch()),
		"yesterday min");
	Check(yesterday.maxDate == todayStart, "yesterday max");
}

void TestInvalidStaysLiteral() {
	const auto same = [](const char *query) {
		const auto text = QString::fromLatin1(query);
		Check(Parse(text).text == text, text + " literal");
	};
	same("type:banana");
	same("before:2024-13-99");
	same("from:@ from:a..b");
	same("size:>5MB");
	same("from: :x");
	Check(
		Parse(QStringLiteral("type:banana")).type == MG::SearchType::None,
		"banana no type");
}

void TestLastWins() {
	const auto parsed = Parse(QStringLiteral("type:photo type:video"));
	Check(parsed.type == MG::SearchType::Video, "last type wins");
	Check(parsed.text.isEmpty(), "operators only, empty text");
}

void TestHashtagPassthrough() {
	const auto hashtag = Parse(QStringLiteral("#tag from:alice"));
	Check(hashtag.text == QStringLiteral("#tag from:alice"), "hashtag text");
	Check(hashtag.from.isEmpty(), "hashtag no from");
	const auto cashtag = Parse(QStringLiteral("$TON type:photo"));
	Check(cashtag.text == QStringLiteral("$TON type:photo"), "cashtag text");
	Check(cashtag.type == MG::SearchType::None, "cashtag no type");
}

void TestWhitespaceCollapses() {
	Check(
		Parse(QStringLiteral("  two   type:photo   words  ")).text
			== QStringLiteral("two words"),
		"whitespace");
}

} // namespace

int main(int argc, char *argv[]) {
	TestPlainText();
	TestTypes();
	TestOperatorsStripped();
	TestFrom();
	TestCaseInsensitive();
	TestDates();
	TestInvalidStaysLiteral();
	TestLastWins();
	TestHashtagPassthrough();
	TestWhitespaceCollapses();

	std::cout << "\n" << (gChecks - gFailures) << "/" << gChecks
		<< " checks passed" << std::endl;
	if (gFailures > 0) {
		std::cerr << gFailures << " check(s) FAILED" << std::endl;
		return 1;
	}
	std::cout << "OK" << std::endl;
	return 0;
}
