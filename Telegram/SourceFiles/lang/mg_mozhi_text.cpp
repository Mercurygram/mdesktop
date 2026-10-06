/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "lang/mg_mozhi_text.h"

#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>

#include <algorithm>

namespace MG {
namespace {

// Mathematical white square brackets: every engine tried (DuckDuckGo,
// Google, Yandex) passes them through, while {{N}} picked up stray braces
// and <bN> tags got spaced apart or dropped.
[[nodiscard]] QString OpenMarker(int index) {
	return QChar(0x27E6) + QString::number(index) + QChar(0x27E7);
}

[[nodiscard]] QString CloseMarker(int index) {
	return QString(QChar(0x27E6)) + '/' + QString::number(index) + QChar(0x27E7);
}

[[nodiscard]] const QRegularExpression &MarkerRegex() {
	static const auto result = QRegularExpression(
		QString(QChar(0x27E6)) + QStringLiteral("(/?)(\\d+)") + QChar(0x27E7));
	return result;
}

[[nodiscard]] QString NewlineMarker() {
	return QString(QChar(0x27E6)) + 'n' + QChar(0x27E7);
}

} // namespace

QStringList SplitMozhiText(const QString &text, int limit) {
	auto result = QStringList();
	auto rest = QStringView(text);
	while (rest.size() > limit) {
		const auto window = rest.left(limit);
		auto cut = window.lastIndexOf(u'\n');
		if (cut <= 0) {
			cut = window.lastIndexOf(u' ');
		}
		const auto size = (cut > 0) ? (cut + 1) : limit;
		result.push_back(rest.left(size).toString());
		rest = rest.mid(size);
	}
	if (!rest.isEmpty() || result.isEmpty()) {
		result.push_back(rest.toString());
	}
	return result;
}

std::optional<QString> ParseMozhiReply(const QByteArray &body) {
	const auto document = QJsonDocument::fromJson(body);
	if (!document.isObject()) {
		return std::nullopt;
	}
	const auto object = document.object();
	auto text = object.value(QStringLiteral("translated-text")).toString();
	if (text.isEmpty()) {
		text = object.value(QStringLiteral("translation")).toString();
	}
	if (text.isEmpty()) {
		return std::nullopt;
	}
	return text;
}

MozhiProtected ProtectMozhiSpans(
		const QString &text,
		std::vector<MozhiSpan> spans) {
	auto result = MozhiProtected{
		.spans = std::move(spans),
	};
	const auto count = int(result.spans.size());
	result.used.assign(count, false);
	const auto valid = [&](const MozhiSpan &span) {
		return (span.offset >= 0)
			&& (span.length > 0)
			&& (span.offset + span.length <= text.size());
	};

	// Atomic spans, outermost first; one nested in another rides along.
	auto atomic = std::vector<int>();
	for (auto i = 0; i != count; ++i) {
		if (result.spans[i].atomic && valid(result.spans[i])) {
			atomic.push_back(i);
		}
	}
	std::sort(begin(atomic), end(atomic), [&](int a, int b) {
		const auto &x = result.spans[a];
		const auto &y = result.spans[b];
		return (x.offset != y.offset)
			? (x.offset < y.offset)
			: (x.length > y.length);
	});
	auto atomicAt = std::vector<int>(text.size() + 1, -1);
	auto covered = std::vector<int>(text.size() + 1, -1);
	auto till = 0;
	for (const auto i : atomic) {
		const auto &span = result.spans[i];
		if (span.offset < till) {
			continue;
		}
		result.used[i] = true;
		atomicAt[span.offset] = i;
		for (auto p = span.offset + 1; p < span.offset + span.length; ++p) {
			covered[p] = i;
		}
		till = span.offset + span.length;
	}

	// A paired span may not start or end strictly inside an atomic one.
	auto opens = std::vector<std::vector<int>>(text.size() + 1);
	auto closes = std::vector<std::vector<int>>(text.size() + 1);
	for (auto i = 0; i != count; ++i) {
		const auto &span = result.spans[i];
		if (span.atomic || !valid(span)) {
			continue;
		}
		const auto end = span.offset + span.length;
		if (covered[span.offset] >= 0 || covered[end] >= 0) {
			continue;
		}
		result.used[i] = true;
		opens[span.offset].push_back(i);
		closes[end].push_back(i);
	}

	auto &out = result.text;
	out.reserve(text.size() + 8 * count);
	for (auto p = 0; p <= text.size();) {
		for (const auto i : closes[p]) {
			out += CloseMarker(i);
		}
		for (const auto i : opens[p]) {
			out += OpenMarker(i);
		}
		if (p == text.size()) {
			break;
		} else if (const auto i = atomicAt[p]; i >= 0) {
			out += OpenMarker(i);
			p += result.spans[i].length;
		} else {
			out += text[p++];
		}
	}

	const auto plain = StripMozhiMarkers(out);
	result.nothingToTranslate = std::none_of(
		plain.begin(),
		plain.end(),
		[](QChar ch) { return ch.isLetter(); });
	return result;
}

std::optional<MozhiRestored> RestoreMozhiSpans(
		const MozhiProtected &prot,
		const QString &source,
		const QString &translated) {
	const auto count = int(prot.spans.size());
	auto result = MozhiRestored();
	result.placed.resize(count);
	auto opened = std::vector<int>(count, -1);
	auto closed = std::vector<int>(count, -1);
	auto &out = result.text;
	auto from = 0;
	auto it = MarkerRegex().globalMatch(translated);
	while (it.hasNext()) {
		const auto match = it.next();
		out += QStringView(translated).mid(from, match.capturedStart() - from);
		from = match.capturedEnd();
		const auto close = !match.capturedView(1).isEmpty();
		auto ok = false;
		const auto i = match.capturedView(2).toInt(&ok);
		if (!ok || i < 0 || i >= count || !prot.used[i]) {
			continue;
		}
		const auto &span = prot.spans[i];
		if (span.atomic) {
			if (close) {
				continue;
			} else if (result.placed[i]) {
				return std::nullopt;
			}
			result.placed[i] = MozhiSpan{
				.offset = int(out.size()),
				.length = span.length,
				.atomic = true,
			};
			out += QStringView(source).mid(span.offset, span.length);
		} else if (close) {
			if (closed[i] < 0) {
				closed[i] = out.size();
			}
		} else if (opened[i] < 0) {
			opened[i] = out.size();
		}
	}
	out += QStringView(translated).mid(from);

	for (auto i = 0; i != count; ++i) {
		if (!prot.used[i]) {
			continue;
		} else if (prot.spans[i].atomic) {
			if (!result.placed[i]) {
				return std::nullopt;
			}
			continue;
		}
		auto start = opened[i];
		auto end = closed[i];
		if (start < 0 || end < start) {
			continue;
		}
		// Engines move the spaces around the markers.
		while (start < end && out[start].isSpace()) {
			++start;
		}
		while (end > start && out[end - 1].isSpace()) {
			--end;
		}
		if (end > start) {
			result.placed[i] = MozhiSpan{
				.offset = start,
				.length = end - start,
			};
		}
	}
	return result;
}

QString StripMozhiMarkers(const QString &translated) {
	auto result = translated;
	return result.remove(MarkerRegex());
}

QString EncodeMozhiNewlines(QString text) {
	return text.replace('\n', NewlineMarker());
}

QString DecodeMozhiNewlines(const QString &translated) {
	static const auto marker = QRegularExpression(
		QStringLiteral("[ \\t]*") + NewlineMarker() + QStringLiteral("[ \\t]*"));
	static const auto spaced = QRegularExpression(
		QStringLiteral("[ \\t]+\\n"));
	auto result = translated;
	result.replace(marker, QStringLiteral("\n"));
	result.replace(spaced, QStringLiteral("\n"));
	return result;
}

} // namespace MG
