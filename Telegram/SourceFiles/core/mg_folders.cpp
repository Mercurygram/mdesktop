/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/mg_folders.h"

#include "apiwrap.h"
#include "base/random.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "data/data_chat_filters.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/stickers/data_custom_emoji.h"
#include "history/history.h"
#include "main/main_session.h"
#include "window/window_session_controller.h"

namespace MG {
namespace {

// The blob names the dialogFilter rule flags in TL order, and the bits are
// carried across as they are, so the two orders have to agree.
using Flag = Data::ChatFilter::Flag;
static_assert(int(Flag::Contacts) == (1 << 0));
static_assert(int(Flag::NonContacts) == (1 << 1));
static_assert(int(Flag::Groups) == (1 << 2));
static_assert(int(Flag::Channels) == (1 << 3));
static_assert(int(Flag::Bots) == (1 << 4));
static_assert(int(Flag::NoMuted) == (1 << 5));
static_assert(int(Flag::NoRead) == (1 << 6));
static_assert(int(Flag::NoArchived) == (1 << 7));
static_assert(int(Flag::RulesMask) == ((1 << 8) - 1));

[[nodiscard]] std::string PrefKey(not_null<Main::Session*> session) {
	return "mg-folders-" + std::to_string(session->userId().bare);
}

// The slot each folder sits in, as this device last saw it: server ids, All
// chats and Mercurygram ids alike. The blob has no room for a position -- it
// is shared with the Android client -- and the folder list itself is rebuilt
// from the server on every launch, so without this the restored folders all
// land at the end.
[[nodiscard]] std::string OrderPrefKey(not_null<Main::Session*> session) {
	return PrefKey(session) + "-order";
}

[[nodiscard]] QByteArray SerializeOrder(const std::vector<FilterId> &order) {
	auto result = QByteArray();
	for (const auto id : order) {
		if (!result.isEmpty()) {
			result += ',';
		}
		result += QByteArray::number(id);
	}
	return result;
}

[[nodiscard]] std::vector<FilterId> ParseOrder(const QByteArray &data) {
	auto result = std::vector<FilterId>();
	for (const auto &part : data.split(',')) {
		auto ok = false;
		const auto id = FilterId(part.toInt(&ok));
		if (ok) {
			result.push_back(id);
		}
	}
	return result;
}

[[nodiscard]] std::vector<FilterId> CurrentOrder(
		not_null<Main::Session*> session) {
	return session->data().chatsFilters().list()
		| ranges::views::transform(&Data::ChatFilter::id)
		| ranges::to_vector;
}

void SaveMercurygramFolderOrder(not_null<Main::Session*> session) {
	const auto key = OrderPrefKey(session);
	const auto value = SerializeOrder(CurrentOrder(session));
	auto &settings = Core::App().settings();
	if (settings.readPref<QByteArray>(key) == value) {
		return;
	}
	settings.writePref<QByteArray>(key, value);
	Core::App().saveSettingsDelayed();
}

// A folder title holds custom emojis and nothing else, so only those cross:
// their document ids mean the same emoji on every device, while the offsets of
// any other entity kind would point into a title no reader can rebuild.
[[nodiscard]] std::vector<TitleEntity> TitleEntities(
		const TextWithEntities &title) {
	auto result = std::vector<TitleEntity>();
	for (const auto &entity : title.entities) {
		if (entity.type() != EntityType::CustomEmoji) {
			continue;
		}
		result.push_back({
			.offset = entity.offset(),
			.length = entity.length(),
			.documentId = qint64(Data::ParseCustomEmojiData(entity.data())),
		});
	}
	return result;
}

[[nodiscard]] TextWithEntities TitleText(const FolderBlobEntry &entry) {
	auto result = TextWithEntities{ entry.title };
	for (const auto &entity : entry.titleEntities) {
		result.entities.push_back({
			EntityType::CustomEmoji,
			entity.offset,
			entity.length,
			Data::SerializeCustomEmojiId(entity.documentId),
		});
	}
	return result;
}

[[nodiscard]] qint64 MarkedFromPeerId(PeerId id) {
	using Kind = MarkedPeer::Kind;
	if (peerIsUser(id)) {
		return ToMarkedPeerId({ Kind::User, qint64(peerToUser(id).bare) });
	} else if (peerIsChat(id)) {
		return ToMarkedPeerId({ Kind::Chat, qint64(peerToChat(id).bare) });
	} else if (peerIsChannel(id)) {
		return ToMarkedPeerId({
			Kind::Channel,
			qint64(peerToChannel(id).bare),
		});
	}
	return 0; // Secret chats and anything else never go into the blob.
}

[[nodiscard]] PeerId PeerIdFromMarked(qint64 marked) {
	const auto parsed = FromMarkedPeerId(marked);
	if (!parsed) {
		return PeerId();
	}
	const auto bare = BareId(parsed->bare);
	switch (parsed->kind) {
	case MarkedPeer::Kind::User: return peerFromUser(UserId(bare));
	case MarkedPeer::Kind::Chat: return peerFromChat(ChatId(bare));
	case MarkedPeer::Kind::Channel: return peerFromChannel(ChannelId(bare));
	}
	return PeerId();
}

// Sorted, because a flat_set of History pointers iterates in address order,
// which differs between runs: the blob is compared byte for byte to decide
// whether the copy in Saved Messages is already up to date.
[[nodiscard]] std::vector<qint64> MarkedSorted(
		const base::flat_set<not_null<History*>> &histories) {
	auto result = std::vector<qint64>();
	result.reserve(histories.size());
	for (const auto &history : histories) {
		if (const auto marked = MarkedFromPeerId(history->peer->id)) {
			result.push_back(marked);
		}
	}
	ranges::sort(result);
	return result;
}

} // namespace

int MercurygramFilterCount(const std::vector<Data::ChatFilter> &list) {
	return int(ranges::count_if(list, [](const Data::ChatFilter &filter) {
		return IsMercurygramFolderId(filter.id());
	}));
}

FilterId NewMercurygramFilterId(const std::vector<Data::ChatFilter> &list) {
	while (true) {
		const auto id = FilterId(-2 - int(base::RandomIndex(
			std::numeric_limits<int>::max() - 1)));
		if (!ranges::contains(list, id, &Data::ChatFilter::id)) {
			return id;
		}
	}
}

Data::ChatFilter MoveFolder(
		not_null<Main::Session*> session,
		FilterId oldId,
		Data::ChatFilter updated) {
	const auto filters = &session->data().chatsFilters();
	if (!oldId
		|| (updated.id() == oldId)
		|| !ranges::contains(filters->list(), oldId, &Data::ChatFilter::id)) {
		// Creating a folder, a plain edit, or a row that was never pushed to
		// the list: nothing to move.
		return updated;
	}
	const auto taken = filters->list() | ranges::views::transform(
		&Data::ChatFilter::id
	) | ranges::to_vector;
	const auto newId = updated.id() ? updated.id() : NextServerFolderId(taken);
	updated = updated.withId(newId);
	filters->applyIdChange(oldId, updated);

	// The active folder is held by each window, and nothing else knows the id
	// changed. setActiveChatsFilter() can clear the section stack, which walks
	// the window list, so re-point over a copy of it.
	const auto windows = session->windows();
	for (const auto &window : windows) {
		if (window->activeChatsFilterCurrent() == oldId) {
			window->setActiveChatsFilter(newId);
		}
	}

	const auto api = &session->api();
	using Flag = MTPmessages_UpdateDialogFilter::Flag;
	auto after = mtpRequestId(0);
	if (IsMercurygramFolderId(newId)) {
		// The folder stays on this device from now on: drop the server copy.
		// The order sent below no longer names oldId, so it has to wait for
		// this delete: an order that omits a filter the server still owns
		// comes back disagreeing with the list, and the reload that follows
		// re-inserts the copy that is not deleted yet, next to this one.
		after = api->request(MTPmessages_UpdateDialogFilter(
			MTP_flags(Flag(0)),
			MTP_int(oldId),
			MTPDialogFilter()
		)).send();
	} else {
		after = api->request(MTPmessages_UpdateDialogFilter(
			MTP_flags(Flag::f_filter),
			MTP_int(newId),
			updated.tl()
		)).send();
	}
	// A rejected create leaves the folder listed under a server id it does not
	// own; the next reload() reconciles it away. The two rejections the user
	// can actually hit -- the folder limit and the chats-per-folder limit --
	// are checked before this runs, so a rollback path would only cover errors
	// that already need a reload to be seen.

	// The order is sent for the whole list, Mercurygram ids and All chats
	// included: saveOrder() echoes it on the client before stripping those ids
	// for the wire, and that echo requires every listed folder to be in it.
	const auto order = filters->list() | ranges::views::transform(
		&Data::ChatFilter::id
	) | ranges::to_vector;
	filters->saveOrder(order, after);
	return updated;
}

TakenFilters TakeMercurygramFilters(std::vector<Data::ChatFilter> &list) {
	auto result = TakenFilters();
	for (auto i = 0; i != int(list.size());) {
		if (IsMercurygramFolderId(list[i].id())) {
			// The slot in the list as it came in, not in the shrinking one:
			// PutBackMercurygramFilters inserts in this same order, so by
			// the time this folder goes back every earlier one already sits
			// in place and the original index is the right one to insert at.
			result.push_back({ i + int(result.size()), std::move(list[i]) });
			list.erase(begin(list) + i);
		} else {
			++i;
		}
	}
	return result;
}

void PutBackMercurygramFilters(
		std::vector<Data::ChatFilter> &list,
		TakenFilters taken) {
	for (auto &[position, filter] : taken) {
		const auto at = std::min(position, int(list.size()));
		list.insert(begin(list) + at, std::move(filter));
	}
}

FolderBlob CollectMercurygramFilters(
		not_null<Data::Session*> owner,
		int updated) {
	auto result = FolderBlob();
	result.updated = updated;
	for (const auto &filter : owner->chatsFilters().list()) {
		if (!IsMercurygramFolderId(filter.id())) {
			continue;
		}
		auto entry = FolderBlobEntry();
		entry.id = filter.id();
		entry.title = filter.titleText().text;
		entry.titleEntities = TitleEntities(filter.titleText());
		entry.titleNoanimate = filter.staticTitle();
		entry.emoticon = filter.iconEmoji();
		entry.color = filter.colorIndex().value_or(-1);
		entry.flags = (filter.flags()
			& Data::ChatFilter::Flag::RulesMask).value();
		entry.include = MarkedSorted(filter.always());
		entry.exclude = MarkedSorted(filter.never());
		for (const auto &history : filter.pinned()) {
			if (const auto marked = MarkedFromPeerId(history->peer->id)) {
				entry.pinned.push_back(marked);
			}
		}
		result.folders.push_back(std::move(entry));
	}
	return result;
}

std::vector<Data::ChatFilter> ParseMercurygramFilters(
		not_null<Data::Session*> owner,
		const FolderBlob &blob) {
	using Flags = Data::ChatFilter::Flags;

	// history(), not a lookup: a peer this device has not met yet still belongs
	// in the folder, and dropping it here would push the shortened set back to
	// the other devices.
	const auto history = [&](qint64 marked) -> History* {
		const auto peerId = PeerIdFromMarked(marked);
		return peerId ? owner->history(peerId).get() : nullptr;
	};
	auto result = std::vector<Data::ChatFilter>();
	result.reserve(blob.folders.size());
	for (const auto &entry : blob.folders) {
		auto always = base::flat_set<not_null<History*>>();
		for (const auto marked : entry.include) {
			if (const auto raw = history(marked)) {
				always.emplace(raw);
			}
		}
		auto pinned = std::vector<not_null<History*>>();
		for (const auto marked : entry.pinned) {
			if (const auto raw = history(marked)) {
				pinned.push_back(raw);
				always.emplace(raw);
			}
		}
		auto never = base::flat_set<not_null<History*>>();
		for (const auto marked : entry.exclude) {
			if (const auto raw = history(marked)) {
				never.emplace(raw);
			}
		}
		result.push_back(Data::ChatFilter(
			entry.id,
			{ TitleText(entry), entry.titleNoanimate },
			entry.emoticon,
			((entry.color >= 0)
				? std::make_optional(uint8(entry.color))
				: std::nullopt),
			Flags::from_raw(Flags::Type(entry.flags)),
			std::move(always),
			std::move(pinned),
			std::move(never)));
	}
	return result;
}

FolderBlob SavedMercurygramFolders(not_null<Main::Session*> session) {
	const auto json = Core::App().settings().readPref<QByteArray>(
		PrefKey(session));
	if (json.isEmpty()) {
		return {};
	}
	return ParseFolderBlob(json).value_or(FolderBlob());
}

void SaveMercurygramFolders(not_null<Main::Session*> session, int updated) {
	const auto key = PrefKey(session);
	const auto json = SerializeFolderBlob(
		CollectMercurygramFilters(&session->data(), updated));
	auto &settings = Core::App().settings();
	if (settings.readPref<QByteArray>(key) == json) {
		// Every folder change lands here, most of them touching no
		// Mercurygram folder at all: an unchanged set is not worth a settings
		// write.
		return;
	}
	settings.writePref<QByteArray>(key, json);
	Core::App().saveSettingsDelayed();
}

void SaveMercurygramFolders(not_null<Main::Session*> session) {
	SaveMercurygramFolders(session, SavedMercurygramFolders(session).updated);
}

MercurygramFolders::MercurygramFolders(not_null<Main::Session*> session)
: _session(session) {
	session->data().chatsFilters().changed(
	) | rpl::on_next([=] {
		handleFiltersChange();
	}, _lifetime);
}

MercurygramFolders::~MercurygramFolders() = default;

void MercurygramFolders::handleFiltersChange() {
	const auto filters = &_session->data().chatsFilters();
	if (!filters->loaded()) {
		return;
	} else if (!_restored) {
		// The server answer is in, so a folder missing from the list is
		// genuinely missing rather than not loaded yet. Restoring goes through
		// ChatFilters::set(), which fires this signal again, so it waits for
		// the current one to finish.
		_restored = true;
		crl::on_main(_session, [=] {
			restoreSaved();
		});
		return;
	} else if (!_ready) {
		// Another change reached us before the restore ran: writing the set
		// back now would save it without the folders still to be restored,
		// and the saved copy is the only place they exist.
		return;
	}
	SaveMercurygramFolders(_session);
	SaveMercurygramFolderOrder(_session);
}

void MercurygramFolders::restoreSaved() {
	const auto filters = &_session->data().chatsFilters();
	const auto saved = ParseMercurygramFilters(
		&_session->data(),
		SavedMercurygramFolders(_session));

	// Every set() below reports a folder change, and _ready is still false, so
	// handleFiltersChange leaves the half-restored set alone until this
	// returns: the copy in the settings is the only place those folders exist.
	for (const auto &filter : saved) {
		if (!ranges::contains(
				filters->list(),
				filter.id(),
				&Data::ChatFilter::id)) {
			filters->set(filter);
		}
	}

	// set() appends an unknown id, so every folder restored above sits after
	// all the server ones now: put the list back in the order this device
	// saved. Ids that are gone are dropped, ids that are new keep the slot
	// the server gave them.
	const auto savedOrder = ParseOrder(
		Core::App().settings().readPref<QByteArray>(OrderPrefKey(_session)));
	if (!savedOrder.empty()) {
		auto order = std::vector<FilterId>();
		order.reserve(filters->list().size());
		for (const auto id : savedOrder) {
			if (!ranges::contains(order, id)
				&& ranges::contains(
					filters->list(),
					id,
					&Data::ChatFilter::id)) {
				order.push_back(id);
			}
		}
		for (const auto &filter : filters->list()) {
			if (!ranges::contains(order, filter.id())) {
				order.push_back(filter.id());
			}
		}
		filters->applyMercurygramOrder(order);
	}
	_ready = true;
	_restoredChanges.fire({});
}

} // namespace MG
