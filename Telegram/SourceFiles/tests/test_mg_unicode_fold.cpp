/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/

// Unit tests for the decorated-Unicode name fold. Catch2 is not vendored in
// this repository (only the external desktop-app libraries pull it in), so this
// uses the same tiny self-contained harness as test_secret_chat.
//
// Built only with -DDESKTOP_APP_TEST_APPS=ON; run the produced
// ./test_mg_unicode_fold.

#include "core/mg_unicode_fold.h"

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

void Folds(const QString &decorated, const QString &expected) {
	const auto folded = MG::FoldDecorated(decorated);
	Check(
		folded == expected,
		decorated + " -> \"" + folded + "\", expected \"" + expected + '"');
}

// A name the fold must not touch: no extra index copy, so the empty sentinel.
void Untouched(const QString &text) {
	Folds(text, QString());
}

// The fold always answers in lowercase; PrepareSearchWords lowercases the
// stored name and the query anyway, and both halves of the table are derived
// from data that only knows one case of a letter.
void TestCompatibilityVariants() {
	// The Mathematical Alphanumerics are all above U+FFFF, which is exactly
	// what the upstream per-QChar accent walk cannot see.
	// bold, double-struck, italic and sans-serif bold, one name
	Folds("\U0001D400\U0001D55F\U0001D45B\U0001D5EE", "anna");
	// monospace
	Folds(
		"\U0001D690\U0001D69B\U0001D69E\U0001D699\U0001D699\U0001D698",
		"gruppo");
	Folds("Ｍｅｒｃｕｒｙ", "mercury");
	Folds("ⓒⓗⓐⓣ", "chat");
	Folds("ℰ", "e");
	// A ligature is one codepoint standing for two letters.
	Folds("uﬃci", "uffici");
	// The superscript generator mixes "¹²³" from Latin-1 with "⁰⁴..⁹" from the
	// superscripts block, so a table starting at U+0100 would fold a generated
	// name only halfway. The Spanish and Portuguese ordinals sit in Latin-1
	// next to their superscript twins for the same reason.
	Folds("ᴳʳᵘᵖᵖᵒ ¹²³⁴⁵⁶", "gruppo 123456");
	Folds("1ª 3º", "1a 3o");
	// U+1CCD6 OUTLINED LATIN CAPITAL LETTER A, added in Unicode 16.0: the table
	// is baked, so this folds against every Qt instead of only a recent one.
	Folds("\U0001CCD6ula", "aula");
}

void TestLookalikes() {
	// The confusables filters used to drop each of these.
	Folds("Ɩ", "l"); // below the old cutoff, and RemoveOneAccent has no entry
	Folds("۷", "v"); // the confusables target is an uppercase V
	Folds("ŋ", "n"); // the target is "n" plus a combining mark
	// "ɛ" is resolved to Greek by the confusables data and is not in
	// RemoveOneAccent either, so only a hand-written entry reaches it.
	Folds("ŋơ۷ɛƖ", "novel");
	// "Ɩ" lowercases to "ɩ" U+0269, which the confusables data maps to "i"
	// instead, so this used to read as "iondon". "ɖ" is left to
	// RemoveOneAccent, which runs over the name before this does.
	Folds("ɩơŋɖơŋ", "lonɖon");
	// The Coptic letters a generator borrows: the confusables data resolves
	// "ⲉ" and "ⲋ" to a prototype that is not ASCII either, and has no entry at
	// all for "ⲇ".
	Folds("ⲥⲉⲋⲧⲟ", "cesto");
	// The confusables data lists the Cherokee syllabary only under its
	// capitals, but a generator emits either case and the fold sees the name as
	// it was written, so both have to reach ASCII.
	Folds("ᏟᎻᎪᎢ", "chat");
	Folds("ꮟꭺꮢ", "bar");
	// The whole reported case.
	Folds("ℂᑌℂℐℕᗅ ℐᝨᗅℒℐᗅℕᗅ", "cucina italiana");
}

void TestDecorationBecomesASeparator() {
	// Search only matches a query at the start of a word, so an emoji glued
	// onto the word has to become a space or the word behind it is unreachable.
	Folds("♥️cucina❤️italiana♥️", " cucina italiana ");
	// The variation selector and the zero width joiner of an emoji sequence are
	// dropped, so they cannot sit between the separator and the word either.
	Folds("❤️‍\U0001F525chat", "  chat");
}

void TestRealScriptsAreLeftAlone() {
	// Folding these would break search written in those scripts.
	Untouched("Привет");
	Untouched("中文群");
	Untouched("한국");
	Untouched("がく");
	// NFKD decomposes these, but the base is not ASCII: folding would turn "ё"
	// into "е" and strip the kana voicing mark.
	Untouched("алёна");
	// Only the digits of these scripts are folded, their letters are real text.
	Untouched("քաղաք");
	// And plain text is plain text.
	Untouched("Cucina Italiana 2024");
	Untouched(QString());
}

void TestTableShape() {
	// The table is generated, sorted and bisected, so a botched regen would
	// shift entries silently. Walk what the fold can reach and check that every
	// answer it gives is non-empty ASCII, and that no ASCII input is remapped.
	for (auto ch = char32_t(1); ch != 0x80; ++ch) {
		Untouched(QString::fromUcs4(&ch, 1));
	}
	auto reachable = 0;
	for (auto ch = char32_t(0x80); ch != 0x110000; ++ch) {
		if (QChar::isSurrogate(ch)) {
			continue;
		}
		const auto folded = MG::FoldDecorated(QString::fromUcs4(&ch, 1));
		if (folded.isEmpty()) {
			continue;
		}
		++reachable;
		for (const auto letter : folded) {
			Check(
				letter.unicode() < 0x80,
				QString("U+%1 folds to \"%2\", which is not ASCII")
					.arg(uint(ch), 4, 16, QChar('0'))
					.arg(folded));
		}
	}
	// The table's own entry count, which the generator prints. Everything the
	// fallback classification reaches on its own is on top of it, so this only
	// has to catch a table that lost entries, not pin an exact number.
	Check(
		reachable >= 2542,
		QString("only %1 codepoints fold").arg(reachable));
}

// A lone high surrogate at the end of a string must not be read past.
void TestTruncatedSurrogate() {
	auto truncated = QString("\U0001D400");
	truncated.append(QChar(0xD835));
	Check(
		MG::FoldDecorated(truncated).startsWith('a'),
		"a truncated surrogate pair is survivable");
}

} // namespace

int main(int argc, char *argv[]) {
	TestCompatibilityVariants();
	TestLookalikes();
	TestDecorationBecomesASeparator();
	TestRealScriptsAreLeftAlone();
	TestTableShape();
	TestTruncatedSurrogate();

	std::cout << "\n" << (gChecks - gFailures) << "/" << gChecks
		<< " checks passed" << std::endl;
	if (gFailures > 0) {
		std::cerr << gFailures << " check(s) FAILED" << std::endl;
		return 1;
	}
	std::cout << "OK" << std::endl;
	return 0;
}
