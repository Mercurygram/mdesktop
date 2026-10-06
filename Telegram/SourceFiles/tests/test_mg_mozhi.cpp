/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/

// Unit tests for the Mozhi translation text helpers: chunk splitting, reply
// parsing and the entity markers. Same tiny self-contained harness as test_mg_unicode_fold.
//
// Built only with -DDESKTOP_APP_TEST_APPS=ON; run the produced
// ./test_mg_mozhi.

#include "lang/mg_mozhi_text.h"

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

void TestSplit() {
	const auto shortText = QStringLiteral("hello world");
	Check(
		MG::SplitMozhiText(shortText) == QStringList{ shortText },
		"short text is one chunk");
	Check(
		MG::SplitMozhiText(QString()) == QStringList{ QString() },
		"empty text is one empty chunk");

	const auto lines = QStringLiteral("aaaa\nbbbb cccc");
	const auto byLine = MG::SplitMozhiText(lines, 10);
	Check(
		byLine == (QStringList{ QStringLiteral("aaaa\n"), QStringLiteral("bbbb cccc") }),
		"cut after newline");

	const auto words = QStringLiteral("aaa bbb ccc");
	const auto bySpace = MG::SplitMozhiText(words, 9);
	Check(
		bySpace == (QStringList{ QStringLiteral("aaa bbb "), QStringLiteral("ccc") }),
		"cut after last space");

	const auto solid = QString(25, QChar('x'));
	const auto hard = MG::SplitMozhiText(solid, 10);
	Check(hard.size() == 3 && hard[0].size() == 10, "hard cut");
	Check(hard.join(QString()) == solid, "hard cut joins back");

	auto big = QString();
	for (auto i = 0; i != 500; ++i) {
		big += QStringLiteral("word%1 ").arg(i);
	}
	const auto chunks = MG::SplitMozhiText(big);
	auto fits = true;
	for (const auto &chunk : chunks) {
		fits = fits && (chunk.size() <= MG::kMozhiChunkLimit);
	}
	Check(chunks.size() > 1 && fits, "default limit respected");
	Check(chunks.join(QString()) == big, "default split joins back");
}

void TestParse() {
	Check(
		MG::ParseMozhiReply(R"({"translated-text":"ciao"})")
			== QStringLiteral("ciao"),
		"translated-text");
	Check(
		MG::ParseMozhiReply(R"({"translation":"hallo"})")
			== QStringLiteral("hallo"),
		"translation fallback");
	Check(
		!MG::ParseMozhiReply(R"({"translated-text":""})"),
		"empty is failure");
	Check(!MG::ParseMozhiReply("[1,2]"), "array is failure");
	Check(!MG::ParseMozhiReply("<html>"), "garbage is failure");
}

QString O(int i) {
	return QChar(0x27E6) + QString::number(i) + QChar(0x27E7);
}

QString C(int i) {
	return QString(QChar(0x27E6)) + '/' + QString::number(i) + QChar(0x27E7);
}

bool Placed(
		const MG::MozhiRestored &r,
		int i,
		int offset,
		int length) {
	return r.placed[i]
		&& r.placed[i]->offset == offset
		&& r.placed[i]->length == length;
}

void TestMarkers() {
	// "see example.com, very important" with a url and a bold run.
	const auto source = QStringLiteral("see example.com, very important");
	const auto prot = MG::ProtectMozhiSpans(source, {
		{ .offset = 4, .length = 11, .atomic = true },
		{ .offset = 17, .length = 14 },
	});
	Check(
		prot.text == "see " + O(0) + ", " + O(1) + "very important" + C(1),
		"protect url + bold");
	Check(!prot.nothingToTranslate, "has words");

	// Engine moved the url and put spaces inside the bold markers.
	const auto translated = "guarda " + O(0) + ", " + O(1) + " molto importante " + C(1);
	const auto r = MG::RestoreMozhiSpans(prot, source, translated);
	Check(r.has_value(), "restore ok");
	if (r) {
		Check(
			r->text == QStringLiteral("guarda example.com,  molto importante "),
			"restored text");
		Check(Placed(*r, 0, 7, 11), "url re-anchored");
		Check(Placed(*r, 1, 21, 16), "bold trimmed");
	}

	const auto lost = MG::RestoreMozhiSpans(prot, source, QStringLiteral("guarda"));
	Check(!lost, "lost atomic fails");
	Check(
		MG::StripMozhiMarkers(translated)
			== QStringLiteral("guarda ,  molto importante "),
		"strip markers");

	const auto dup = MG::RestoreMozhiSpans(prot, source, O(0) + O(0) + " ciao");
	Check(!dup, "duplicated atomic fails");

	const auto mangled = MG::RestoreMozhiSpans(
		prot,
		source,
		"guarda " + O(0) + " molto " + C(1) + " importante");
	Check(mangled && !mangled->placed[1], "lost open drops bold only");
	Check(mangled && mangled->placed[0], "url kept when bold lost");

	const auto linkOnly = MG::ProtectMozhiSpans(QStringLiteral("example.com"), {
		{ .offset = 0, .length = 11, .atomic = true },
	});
	Check(linkOnly.nothingToTranslate, "link only");

	// Bold nested in a link rides along, bold crossing a link is dropped.
	const auto nested = MG::ProtectMozhiSpans(QStringLiteral("go to example.com now"), {
		{ .offset = 6, .length = 11, .atomic = true },
		{ .offset = 6, .length = 7 },
		{ .offset = 0, .length = 9 },
	});
	Check(!nested.used[1] && !nested.used[2], "overlaps skipped");
	Check(nested.text == "go to " + O(0) + " now", "only link marked");

	// Wrapping exactly a link keeps both.
	const auto wrap = MG::ProtectMozhiSpans(QStringLiteral("a b.com"), {
		{ .offset = 2, .length = 5, .atomic = true },
		{ .offset = 2, .length = 5 },
	});
	Check(wrap.text == "a " + O(1) + O(0) + C(1), "bold around link");
}

void TestNewlines() {
	const auto source = QStringLiteral("Lista:\n@a\n\nfin");
	const auto encoded = MG::EncodeMozhiNewlines(source);
	Check(!encoded.contains('\n'), "no raw newline sent");
	Check(MG::DecodeMozhiNewlines(encoded) == source, "newline round trip");

	const auto n = QString(QChar(0x27E6)) + 'n' + QChar(0x27E7);
	Check(
		MG::DecodeMozhiNewlines("Hello. " + n + " How? " + n + " " + n + " Ok.")
			== QStringLiteral("Hello.\nHow?\n\nOk."),
		"spaces around marker dropped");
	Check(
		MG::DecodeMozhiNewlines(QStringLiteral("Hi. \nThere \n\nend"))
			== QStringLiteral("Hi.\nThere\n\nend"),
		"space before newline dropped");
	Check(
		MG::StripMozhiMarkers(n + "x" + O(0)) == n + "x",
		"newline marker is not an entity marker");
}

} // namespace

int main() {
	TestSplit();
	TestParse();
	TestMarkers();
	TestNewlines();
	std::cout << (gChecks - gFailures) << "/" << gChecks
		<< " checks passed" << std::endl;
	return gFailures ? 1 : 0;
}
