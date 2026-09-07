/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/timer.h"
#include "core/mg_folder_blob.h"
#include "data/data_types.h"

#include <rpl/lifetime.h>

class HistoryItem;

namespace Data {
class DocumentMedia;
} // namespace Data

namespace Main {
class Session;
} // namespace Main

namespace MG {

// Keeps Mercurygram folders (see core/mg_folders.h) in step across the
// devices of one account through a single JSON document in Saved Messages,
// edited in place: mercurygram-folders.json with the caption
// #mercurygram_folders. Telegram already stores every server folder
// definition, so mirroring this set into the account leaks nothing new, and no
// third-party service is involved. The Android client writes and reads the
// same document.
//
// Flow: the first folder change after launch searches Saved Messages for the
// document and pulls it, so a fresh install adopts the copy in the cloud
// before it could push an empty one; every later change schedules a debounced
// push, skipped when the cloud already holds this set. A message or an edit in
// Saved Messages carrying that file name -- from another device, or our own
// send coming back -- adopts the newest one and pulls. Last writer wins on the
// document's "updated" field, which is kept in the same pref as the folders, so
// a change made offline still pushes after a restart instead of being
// overwritten by the older copy in the cloud.
//
// Known limits, by design: the whole document is replaced, folder contents are
// not merged; the relative order of the folders already present is not
// re-applied; a push that fails on the wire is retried on the next folder
// change or launch.
class MercurygramFolderSync final {
public:
	explicit MercurygramFolderSync(not_null<Main::Session*> session);
	~MercurygramFolderSync();

private:
	void handleFiltersChange();
	void handleMessage(not_null<HistoryItem*> item);
	void handleDestroyed(not_null<HistoryItem*> item);
	void find();
	void adopt(MsgId messageId);
	void pull();
	void apply(const FolderBlob &blob);
	void schedulePush();
	void push();
	void pushDone();
	void pushFailed();

	[[nodiscard]] HistoryItem *message() const;
	[[nodiscard]] bool isOurDocument(not_null<HistoryItem*> item) const;

	const not_null<Main::Session*> _session;

	MsgId _messageId = 0; // The document in Saved Messages, once known.
	bool _started = false; // The cloud was looked at once since launch.
	bool _searching = false;
	bool _pushing = false;

	// The blob the cloud is believed to hold, folders only: the "updated" it
	// carries differs between two devices holding the same set.
	QByteArray _cloudFolders;
	QByteArray _foldersBefore;
	int _updatedBefore = 0;

	std::shared_ptr<Data::DocumentMedia> _view;
	base::Timer _pushTimer;
	base::Timer _pushTimeout;
	rpl::lifetime _lifetime;

};

} // namespace MG
