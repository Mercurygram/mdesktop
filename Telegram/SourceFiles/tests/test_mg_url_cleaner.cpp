/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/

// Unit tests for the tracking-parameter stripper. Catch2 is not vendored in
// this repository, so this uses the same tiny self-contained harness as
// test_mg_unicode_fold.
//
// Built only with -DDESKTOP_APP_TEST_APPS=ON; run the produced
// ./test_mg_url_cleaner.

#include "core/mg_url_cleaner.h"

#include <rpl/never.h>
#include <rpl/producer.h>

#include <QtCore/QString>

#include <iostream>
#include <utility>

// lib_ui comes in for the entity parser that finds the links inside a block of
// text, and its animation and toast code calls back into this hook, which the
// application normally provides. A headless test runs no animation, so a
// producer that never fires is enough to link.
namespace crl {

rpl::producer<> on_main_update_requests() {
	return rpl::never<>();
}

} // namespace crl

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

void Cleans(const QString &url, const QString &expected) {
	const auto cleaned = MG::StripTrackingFromUrl(url);
	Check(
		cleaned == expected,
		url + " -> \"" + cleaned + "\", expected \"" + expected + '"');
}

void Keeps(const QString &url) {
	Cleans(url, url);
}

void TestGlobalParameters() {
	Cleans(
		"https://example.com/a?utm_source=news&id=7",
		"https://example.com/a?id=7");
	Cleans(
		"https://example.com/a?fbclid=abc",
		"https://example.com/a");
	Cleans(
		"https://example.com/a?gclid=x&utm_medium=y&q=z",
		"https://example.com/a?q=z");
	Keeps("https://example.com/a?id=7");
	Keeps("https://example.com/a");
}

void TestHostScopedParameters() {
	Cleans(
		"https://youtu.be/dQw4w9WgXcQ?si=abcd",
		"https://youtu.be/dQw4w9WgXcQ");
	Cleans(
		"https://www.youtube.com/watch?v=dQw4w9WgXcQ&si=abcd",
		"https://www.youtube.com/watch?v=dQw4w9WgXcQ");
	Cleans(
		"https://www.amazon.co.uk/dp/B01?tag=aff-21&th=1",
		"https://www.amazon.co.uk/dp/B01?th=1");
	Cleans(
		"https://www.google.de/search?q=cats&ved=2ahUKEwi",
		"https://www.google.de/search?q=cats");
	// si is a real parameter on sites that are not YouTube or Spotify.
	Keeps("https://example.com/map?si=42");
}

void TestMeaningfulParametersStay() {
	// A carousel index picks which image opens.
	Cleans(
		"https://www.instagram.com/p/abc/?img_index=2&stkn=zzz",
		"https://www.instagram.com/p/abc/?img_index=2");
}

void TestQueryEncodingIsPreserved() {
	// Decoding and re-encoding would turn the plus into %2B, which the server
	// reads as a literal plus rather than a space.
	Cleans(
		"https://example.com/s?q=hello+world&utm_source=x",
		"https://example.com/s?q=hello+world");
	Cleans(
		"https://example.com/s?q=a%26b&fbclid=x",
		"https://example.com/s?q=a%26b");
}

void TestNonHttpIsLeftAlone() {
	Keeps("tg://resolve?domain=durov&utm_source=x");
	Keeps("mailto:someone@example.com?utm_source=x");
}

} // namespace

int main(int argc, char *argv[]) {
	TestGlobalParameters();
	TestHostScopedParameters();
	TestMeaningfulParametersStay();
	TestQueryEncodingIsPreserved();
	TestNonHttpIsLeftAlone();

	std::cout << "\n" << (gChecks - gFailures) << "/" << gChecks
		<< " checks passed" << std::endl;
	if (gFailures > 0) {
		std::cerr << gFailures << " check(s) FAILED" << std::endl;
		return 1;
	}
	std::cout << "OK" << std::endl;
	return 0;
}
