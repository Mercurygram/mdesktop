/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/mg_pins.h"

#include "data/data_premium_limits.h"
#include "data/data_session.h"
#include "dialogs/dialogs_key.h"
#include "dialogs/dialogs_main_list.h"
#include "dialogs/dialogs_pinned_list.h"
#include "history/history.h"
#include "main/main_session.h"
#include "storage/storage_account.h"

namespace MG {
namespace {

constexpr auto kPrefKey = "mg-main-pins";

[[nodiscard]] std::vector<PeerId> Read(not_null<Data::Session*> owner) {
	const auto raw = owner->session().local().readPref<QByteArray>(kPrefKey);
	auto result = std::vector<PeerId>();
	for (const auto &part : raw.split(',')) {
		auto ok = false;
		const auto value = part.toULongLong(&ok);
		if (ok && value) {
			result.push_back(PeerId(PeerIdHelper(value)));
		}
	}
	return result;
}

// A chat this list can pin right now: loaded and in "All chats", not in
// the archive.
[[nodiscard]] History *Pinnable(
		not_null<Data::Session*> owner,
		PeerId id) {
	const auto history = owner->historyLoaded(id);
	return (history
		&& history->folderKnown()
		&& !history->folder()
		&& history->inChatList())
		? history
		: nullptr;
}

} // namespace

int ServerMainPinsLimit(not_null<Data::Session*> owner) {
	return Data::PremiumLimits(&owner->session()).dialogsPinnedCurrent();
}

void SaveMainPins(not_null<Data::Session*> owner) {
	auto ids = std::vector<PeerId>();
	for (const auto &key : owner->pinnedChatsOrder(nullptr)) {
		if (const auto history = key.history()) {
			ids.push_back(history->peer->id);
		}
	}
	// A stored chat that is not loaded yet, or sits in the archive, did
	// not get unpinned here: keep it, it is pinned again once it shows up.
	for (const auto id : Read(owner)) {
		if (!Pinnable(owner, id) && !ranges::contains(ids, id)) {
			ids.push_back(id);
		}
	}
	auto raw = QByteArray();
	for (const auto id : ids) {
		if (!raw.isEmpty()) {
			raw += ',';
		}
		raw += QByteArray::number(quint64(id.value));
	}
	owner->session().local().writePref<QByteArray>(kPrefKey, raw);
}

void ApplyMainPins(not_null<Data::Session*> owner) {
	const auto stored = Read(owner);
	if (stored.empty()) {
		return;
	}
	const auto pinned = owner->chatsList(nullptr)->pinned();
	const auto was = pinned->order();

	// Pinned elsewhere and not known here (another device, the archive
	// row): those stay on top, in the order the server gave.
	auto order = std::vector<Dialogs::Key>();
	for (const auto &key : was) {
		const auto history = key.history();
		if (!history || !ranges::contains(stored, history->peer->id)) {
			order.push_back(key);
		}
	}
	for (const auto id : stored) {
		if (const auto history = Pinnable(owner, id)) {
			order.push_back(history);
		}
	}
	if (order == was) {
		return;
	}
	// setPinned(true) moves to the front, so walk from the bottom up.
	for (auto i = order.rbegin(); i != order.rend(); ++i) {
		pinned->setPinned(*i, true);
	}
	owner->notifyPinnedDialogsOrderUpdated();
}

} // namespace MG
