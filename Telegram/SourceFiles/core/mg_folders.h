/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "core/mg_folder_blob.h"
#include "data/data_types.h"

#include <rpl/event_stream.h>
#include <rpl/lifetime.h>

namespace Data {
class ChatFilter;
class Session;
} // namespace Data

namespace Main {
class Session;
} // namespace Main

namespace MG {

// Mercurygram folders: ordinary chat folders whose definition never reaches
// Telegram. Folder membership is computed on the client anyway -- the server
// only stores the definition -- so a folder kept out of
// messages.updateDialogFilter works exactly like a server one, minus the
// folder-count and chats-per-folder limits. A negative id is the whole
// discriminator (Telegram hands out 0 and >= 2), so never give a Mercurygram
// folder a server-range id.
//
// Telegram Desktop keeps no on-disk copy of the folder list -- it is rebuilt
// from messages.getDialogFilters on every launch -- so this device's set is
// persisted as one blob in the settings pref store, in the same format as the
// copy kept in Saved Messages (see core/mg_folder_blob.h).

[[nodiscard]] int MercurygramFilterCount(
	const std::vector<Data::ChatFilter> &list);

// Random over the negative range rather than counting down from -1: the id has
// to be unique across every device on the account, and this device only knows
// its own folders, so two devices that each create their first Mercurygram
// folder before they sync would both pick -1 and the pull would then read the
// two folders as one. Never -1 itself: the format reserves it, because the UI
// code in both clients uses -1 as a "no folder" placeholder.
[[nodiscard]] FilterId NewMercurygramFilterId(
	const std::vector<Data::ChatFilter> &list);

// Moves an existing folder between the Mercurygram and the server id ranges:
// the same folder under a new id, keeping its slot, its chats and its pins. A
// zero id in `updated` means "give it the next free server one". Returns the
// filter with the id it now has; a no-op for a folder the list does not hold
// yet.
[[nodiscard]] Data::ChatFilter MoveFolder(
	not_null<Main::Session*> session,
	FilterId oldId,
	Data::ChatFilter updated);

// Mercurygram folders taken out of the list before it is reconciled with the
// server answer, each remembering the slot it sat in. Every path that compares
// the list against messages.getDialogFilters would otherwise drop them.
using TakenFilters = std::vector<std::pair<int, Data::ChatFilter>>;

[[nodiscard]] TakenFilters TakeMercurygramFilters(
	std::vector<Data::ChatFilter> &list);
void PutBackMercurygramFilters(
	std::vector<Data::ChatFilter> &list,
	TakenFilters taken);

// The blob <-> Data::ChatFilter bridge.
[[nodiscard]] FolderBlob CollectMercurygramFilters(
	not_null<Data::Session*> owner,
	int updated);
[[nodiscard]] std::vector<Data::ChatFilter> ParseMercurygramFilters(
	not_null<Data::Session*> owner,
	const FolderBlob &blob);

// This device's own copy, in the settings pref store, keyed by account.
[[nodiscard]] FolderBlob SavedMercurygramFolders(
	not_null<Main::Session*> session);
void SaveMercurygramFolders(not_null<Main::Session*> session, int updated);
void SaveMercurygramFolders(not_null<Main::Session*> session);

// One per account: restores the saved set once the server list has arrived,
// and writes it back on every later folder change.
class MercurygramFolders final {
public:
	explicit MercurygramFolders(not_null<Main::Session*> session);
	~MercurygramFolders();

	// The saved set is in the list: until then a Mercurygram folder simply
	// does not exist yet, so a lookup by id can only report it as gone.
	[[nodiscard]] bool restored() const {
		return _ready;
	}

	// Fires once, right after that became true. The restore itself reports
	// folder changes while restored() is still false, so a listener that needs
	// the complete list has nothing else to wait for.
	[[nodiscard]] rpl::producer<> restoredChanges() const {
		return _restoredChanges.events();
	}

private:
	void handleFiltersChange();
	void restoreSaved();

	const not_null<Main::Session*> _session;
	bool _restored = false; // The restore was scheduled.
	bool _ready = false; // The restore ran: the list is complete, save it.
	rpl::event_stream<> _restoredChanges;
	rpl::lifetime _lifetime;

};

} // namespace MG
