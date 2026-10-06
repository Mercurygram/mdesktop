/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "lang/mg_mozhi_provider.h"

#include "base/weak_ptr.h"
#include "core/mg_settings.h"
#include "lang/mg_mozhi_text.h"

#include <QtCore/QUrl>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

namespace MG {
namespace {

constexpr auto kTimeout = crl::time(15000);
constexpr auto kBanDuration = crl::time(60000);

const auto kDefaultInstances = std::array{
	u"https://mozhi.aryak.me"_q,
	u"https://mozhi.pussthecat.org"_q,
	u"https://mozhi.catsarch.com"_q,
	u"https://translate.projectsegfau.lt"_q,
	u"https://mozhi.ducks.party"_q,
};

// Shared by every provider: one is created per translate box or chat bar,
// and a dead instance should stay skipped across them.
int PreferredInstance = 0;
base::flat_map<QString, crl::time> BannedUntil;

[[nodiscard]] QStringList ActiveInstances() {
	const auto custom = MozhiInstance();
	if (!custom.isEmpty()) {
		return { custom };
	}
	return { begin(kDefaultInstances), end(kDefaultInstances) };
}

// A rejected request (too long, malformed) is our fault, not the instance's.
[[nodiscard]] bool RequestRejected(int status) {
	return (status == 400)
		|| (status == 413)
		|| (status == 414)
		|| (status == 431);
}

// Whether an entity's text is kept as is (true), translated with the
// entity re-applied (false), or the entity dropped (nothing).
[[nodiscard]] std::optional<bool> IsAtomic(EntityType type) {
	switch (type) {
	case EntityType::Url:
	case EntityType::Email:
	case EntityType::Hashtag:
	case EntityType::Cashtag:
	case EntityType::Mention:
	case EntityType::MentionName:
	case EntityType::CustomEmoji:
	case EntityType::BotCommand:
	case EntityType::MediaTimestamp:
	case EntityType::Phone:
	case EntityType::BankCard:
	case EntityType::Code:
	case EntityType::Pre:
	case EntityType::FormattedDate:
		return true;
	case EntityType::CustomUrl:
	case EntityType::Bold:
	case EntityType::Semibold:
	case EntityType::Italic:
	case EntityType::Underline:
	case EntityType::StrikeOut:
	case EntityType::Blockquote:
	case EntityType::Spoiler:
	case EntityType::Subscript:
	case EntityType::Superscript:
	case EntityType::Marked:
		return false;
	case EntityType::Invalid:
	case EntityType::Colorized:
		return std::nullopt;
	}
	return std::nullopt;
}

struct Protected {
	MozhiProtected spans;
	std::vector<EntityInText> entities; // One per span.
};

[[nodiscard]] Protected Protect(const TextWithEntities &source) {
	auto result = Protected();
	auto spans = std::vector<MozhiSpan>();
	for (const auto &entity : source.entities) {
		if (const auto atomic = IsAtomic(entity.type())) {
			spans.push_back({
				.offset = entity.offset(),
				.length = entity.length(),
				.atomic = *atomic,
			});
			result.entities.push_back(entity);
		}
	}
	result.spans = ProtectMozhiSpans(source.text, std::move(spans));
	return result;
}

[[nodiscard]] TextWithEntities Restore(
		const Protected &prot,
		const QString &source,
		const QString &translated) {
	const auto restored = RestoreMozhiSpans(prot.spans, source, translated);
	if (!restored) {
		return { StripMozhiMarkers(translated) };
	}
	const auto &spans = prot.spans.spans;
	const auto &placed = restored->placed;
	auto result = TextWithEntities{ restored->text };
	for (auto i = 0; i != int(spans.size()); ++i) {
		const auto &entity = prot.entities[i];
		if (placed[i]) {
			result.entities.push_back(EntityInText(
				entity.type(),
				placed[i]->offset,
				placed[i]->length,
				entity.data()));
			continue;
		}
		// Not marked itself: carried inside the atomic span holding it.
		const auto &span = spans[i];
		for (auto j = 0; j != int(spans.size()); ++j) {
			const auto &outer = spans[j];
			if (outer.atomic
				&& placed[j]
				&& outer.offset <= span.offset
				&& span.offset + span.length <= outer.offset + outer.length) {
				result.entities.push_back(EntityInText(
					entity.type(),
					placed[j]->offset + (span.offset - outer.offset),
					span.length,
					entity.data()));
				break;
			}
		}
	}
	ranges::sort(result.entities, std::less<>(), &EntityInText::offset);
	return result;
}

[[nodiscard]] QByteArray FormValue(const QString &value) {
	return QUrl::toPercentEncoding(value);
}

class MozhiProvider final
	: public Ui::TranslateProvider
	, public base::has_weak_ptr {
public:
	[[nodiscard]] bool supportsMessageId() const override {
		return false;
	}

	void request(
			Ui::TranslateProviderRequest request,
			LanguageId to,
			Fn<void(Ui::TranslateProviderResult)> done) override {
		if (request.text.text.trimmed().isEmpty()) {
			done({ .error = Ui::TranslateProviderError::Unknown });
			return;
		}
		auto prot = std::make_shared<Protected>(Protect(request.text));
		if (prot->spans.nothingToTranslate) {
			done({ .text = std::move(request.text) });
			return;
		}
		auto code = to.twoLetterCode();
		if (code == u"nb"_q) {
			code = u"no"_q;
		}
		const auto source = request.text.text;
		translateChunks(
			SplitMozhiText(prot->spans.text),
			0,
			code,
			QString(),
			[=](std::optional<QString> translated) {
				if (!translated) {
					done({ .error = Ui::TranslateProviderError::Unknown });
				} else {
					done({ .text = Restore(*prot, source, *translated) });
				}
			});
	}

private:
	void translateChunks(
			QStringList chunks,
			int index,
			QString to,
			QString translated,
			Fn<void(std::optional<QString>)> done) {
		if (index == chunks.size()) {
			done(std::move(translated));
			return;
		}
		const auto chunk = chunks[index];
		translateChunk(chunk, to, ActiveInstances(), 0, crl::guard(this, [=](
				std::optional<QString> result) mutable {
			if (!result) {
				done(std::nullopt);
				return;
			}
			// Engines drop a leading newline, which splits paragraphs
			// at chunk boundaries.
			if (chunk.startsWith('\n') && !result->startsWith('\n')) {
				result->prepend('\n');
			}
			translateChunks(
				chunks,
				index + 1,
				to,
				translated + *result,
				done);
		}));
	}

	void translateChunk(
			QString text,
			QString to,
			QStringList instances,
			int attempt,
			Fn<void(std::optional<QString>)> done) {
		const auto count = int(instances.size());
		const auto now = crl::now();
		auto index = -1;
		for (; attempt != count; ++attempt) {
			const auto i = (PreferredInstance + attempt) % count;
			const auto ban = BannedUntil.find(instances[i]);
			if (ban == end(BannedUntil) || ban->second <= now) {
				index = i;
				break;
			}
		}
		if (index < 0) {
			done(std::nullopt);
			return;
		}
		const auto instance = instances[index];
		auto request = QNetworkRequest(
			QUrl(instance + u"/api/translate"_q));
		request.setHeader(
			QNetworkRequest::ContentTypeHeader,
			u"application/x-www-form-urlencoded; charset=UTF-8"_q);
		request.setRawHeader("Accept", "application/json");
		request.setTransferTimeout(kTimeout);

		// Text goes in the body, not the query: keeps it out of access logs
		// and clear of URL length limits.
		const auto engines = MozhiEngineNames();
		const auto engine = engines.value(MozhiEngine(), engines.front());
		const auto send = (engine == u"DuckDuckGo"_q)
			? EncodeMozhiNewlines(text)
			: text;
		const auto body = "engine=" + FormValue(engine.toLower())
			+ "&from=auto"
			+ "&to=" + FormValue(to)
			+ "&text=" + FormValue(send);
		const auto reply = _network.post(request, body);
		QObject::connect(reply, &QNetworkReply::finished, crl::guard(this, [=] {
			reply->deleteLater();
			const auto status = reply->attribute(
				QNetworkRequest::HttpStatusCodeAttribute).toInt();
			auto result = (reply->error() == QNetworkReply::NoError)
				? ParseMozhiReply(reply->readAll())
				: std::nullopt;
			if (result) {
				result = DecodeMozhiNewlines(*result);
			}
			if (result) {
				if (instances.size() > 1) {
					PreferredInstance = index;
				}
				done(result);
			} else if (RequestRejected(status)) {
				done(std::nullopt);
			} else {
				BannedUntil[instance] = crl::now() + kBanDuration;
				translateChunk(text, to, instances, attempt + 1, done);
			}
		}));
	}

	QNetworkAccessManager _network;

};

} // namespace

QStringList MozhiEngineNames() {
	return {
		u"DuckDuckGo"_q,
		u"Google"_q,
		u"Yandex"_q,
	};
}

std::unique_ptr<Ui::TranslateProvider> CreateMozhiProvider() {
	return std::make_unique<MozhiProvider>();
}

} // namespace MG
