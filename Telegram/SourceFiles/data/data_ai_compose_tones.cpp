/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "data/data_ai_compose_tones.h"

#include "main/main_session.h"
#include "apiwrap.h"

namespace Data {
namespace {

constexpr auto kRefreshInterval = 3600 * crl::time(1000);

} // namespace

AiComposeTones::AiComposeTones(not_null<Main::Session*> session)
: _session(session)
, _refreshTimer([=] { refresh(); }) {
	refresh();
	_refreshTimer.callEach(kRefreshInterval);
}

void AiComposeTones::refresh() {
	if (_refreshRequestId) {
		return;
	}
	_refreshRequestId = _session->api().request(MTPaicompose_GetTones(
		MTP_long(_hash)
	)).done([=](const MTPaicompose_Tones &result) {
		_refreshRequestId = 0;
		result.match([&](const MTPDaicompose_tones &data) {
			_hash = data.vhash().v;
			parseTones(data.vtones().v);
			_updates.fire({});
		}, [](const MTPDaicompose_tonesNotModified &) {
		});
	}).fail([=] {
		_refreshRequestId = 0;
	}).send();
}

void AiComposeTones::parseTones(const QVector<MTPAiComposeTone> &list) {
	_list.clear();
	_list.reserve(list.size());
	for (const auto &tone : list) {
		_list.push_back(parseTone(tone));
	}
}

AiComposeTone AiComposeTones::parseTone(
		const MTPAiComposeTone &tone) const {
	return tone.match([&](const MTPDaiComposeTone &data) {
		return AiComposeTone{
			.id = data.vid().v,
			.accessHash = data.vaccess_hash().v,
			.slug = qs(data.vslug()),
			.title = qs(data.vtitle()),
			.emojiId = data.vemoji_id().value_or_empty(),
			.prompt = qs(data.vprompt()),
			.installsCount = data.vinstalls_count().value_or_empty(),
			.authorId = data.vauthor_id()
				? UserId(data.vauthor_id()->v)
				: UserId(0),
			.creator = data.is_creator(),
		};
	}, [&](const MTPDaiComposeToneDefault &data) {
		return AiComposeTone{
			.title = qs(data.vtitle()),
			.emojiId = data.vemoji_id().v,
			.isDefault = true,
			.defaultType = qs(data.vtone()),
		};
	});
}

void AiComposeTones::create(
		const QString &title,
		const QString &prompt,
		DocumentId emojiId,
		bool displayAuthor,
		Fn<void(AiComposeTone)> done,
		Fn<void(const MTP::Error &)> fail) {
	using Flag = MTPaicompose_CreateTone::Flag;
	auto flags = MTPaicompose_CreateTone::Flags(0);
	if (displayAuthor) {
		flags |= Flag::f_display_author;
	}
	if (emojiId) {
		flags |= Flag::f_emoji_id;
	}
	_session->api().request(MTPaicompose_CreateTone(
		MTP_flags(flags),
		MTP_long(emojiId),
		MTP_string(title),
		MTP_string(prompt)
	)).done([=](const MTPAiComposeTone &result) {
		auto parsed = parseTone(result);
		if (done) {
			done(parsed);
		}
		refresh();
	}).fail([=](const MTP::Error &error) {
		if (fail) {
			fail(error);
		}
	}).send();
}

void AiComposeTones::update(
		const AiComposeTone &tone,
		std::optional<QString> title,
		std::optional<QString> prompt,
		std::optional<DocumentId> emojiId,
		std::optional<bool> displayAuthor,
		Fn<void(AiComposeTone)> done,
		Fn<void(const MTP::Error &)> fail) {
	using Flag = MTPaicompose_UpdateTone::Flag;
	auto flags = MTPaicompose_UpdateTone::Flags(0);
	if (displayAuthor) {
		flags |= Flag::f_display_author;
	}
	if (emojiId) {
		flags |= Flag::f_emoji_id;
	}
	if (title) {
		flags |= Flag::f_title;
	}
	if (prompt) {
		flags |= Flag::f_prompt;
	}
	_session->api().request(MTPaicompose_UpdateTone(
		MTP_flags(flags),
		toneToMTP(tone),
		displayAuthor
			? (*displayAuthor ? MTP_boolTrue() : MTP_boolFalse())
			: MTPBool(),
		MTP_long(emojiId.value_or(0)),
		MTP_string(title.value_or(QString())),
		MTP_string(prompt.value_or(QString()))
	)).done([=](const MTPAiComposeTone &result) {
		auto parsed = parseTone(result);
		if (done) {
			done(parsed);
		}
		refresh();
	}).fail([=](const MTP::Error &error) {
		if (fail) {
			fail(error);
		}
	}).send();
}

void AiComposeTones::save(
		const AiComposeTone &tone,
		bool unsave,
		Fn<void()> done) {
	_session->api().request(MTPaicompose_SaveTone(
		toneToMTP(tone),
		unsave ? MTP_boolTrue() : MTP_boolFalse()
	)).done([=] {
		if (done) {
			done();
		}
		refresh();
	}).fail([=] {
	}).send();
}

void AiComposeTones::remove(
		const AiComposeTone &tone,
		Fn<void()> done) {
	_session->api().request(MTPaicompose_DeleteTone(
		toneToMTP(tone)
	)).done([=] {
		if (done) {
			done();
		}
		refresh();
	}).fail([=] {
	}).send();
}

void AiComposeTones::resolve(
		const QString &slug,
		Fn<void(AiComposeTone)> done,
		Fn<void(const MTP::Error &)> fail) {
	_session->api().request(MTPaicompose_GetTone(
		MTP_inputAiComposeToneSlug(MTP_string(slug))
	)).done([=](const MTPAiComposeTone &result) {
		if (done) {
			done(parseTone(result));
		}
	}).fail([=](const MTP::Error &error) {
		if (fail) {
			fail(error);
		}
	}).send();
}

void AiComposeTones::applyUpdate() {
	refresh();
}

MTPInputAiComposeTone AiComposeTones::toneToMTP(
		const AiComposeTone &tone) const {
	return tone.isDefault
		? MTP_inputAiComposeToneDefault(MTP_string(tone.defaultType))
		: MTP_inputAiComposeToneID(
			MTP_long(tone.id),
			MTP_long(tone.accessHash));
}

const std::vector<AiComposeTone> &AiComposeTones::list() const {
	return _list;
}

rpl::producer<> AiComposeTones::updated() const {
	return _updates.events();
}

} // namespace Data
