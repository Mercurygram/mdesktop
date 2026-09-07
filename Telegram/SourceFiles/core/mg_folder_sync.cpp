/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/mg_folder_sync.h"

#include "api/api_common.h"
#include "apiwrap.h"
#include "base/unixtime.h"
#include "core/mg_folders.h"
#include "data/data_changes.h"
#include "data/data_chat_filters.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_file_origin.h"
#include "data/data_media_types.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "storage/localimageloader.h"
#include "ui/chat/attach/attach_prepare.h"

namespace MG {
namespace {

// The document every Mercurygram client looks for in Saved Messages. Both are
// part of the wire format (MercurygramFolders.md in the Android repository):
// the file name is what the other devices match on, and the caption tag is
// what the search asks for, so neither may vary.
const auto kFileName = u"mercurygram-folders.json"_q;
const auto kCaptionTag = u"#mercurygram_folders"_q;

constexpr auto kSearchLimit = 5;
constexpr auto kPushDelay = crl::time(3000);
constexpr auto kPushTimeout = crl::time(60000);
constexpr auto kCaptionLimit = 1024;

// Anything this big is not a folder blob: any account can hold a file with that
// name in Saved Messages, and it is not downloaded before it is refused.
constexpr auto kMaxBlobSize = int64(4 * 1024 * 1024);

// By id, so the key does not depend on the order the folders sit in: a pull
// leaves the order of the folders already present alone, and two devices that
// disagree on it would otherwise push at each other forever. The "updated" is
// dropped as well: two devices holding the same set disagree on it.
[[nodiscard]] QByteArray FoldersKey(FolderBlob blob) {
	blob.updated = 0;
	ranges::sort(blob.folders, ranges::greater(), &FolderBlobEntry::id);
	return SerializeFolderBlob(blob);
}

[[nodiscard]] QByteArray FoldersKey(not_null<Main::Session*> session) {
	return FoldersKey(CollectMercurygramFilters(&session->data(), 0));
}

// What the cloud holds, in this device's own wording so the two keys can be
// compared: the folders the document listed, and only those. A device that has
// never synced keeps folders the document cannot know about, and counting them
// here would mean never pushing them.
[[nodiscard]] QByteArray CloudKey(
		not_null<Main::Session*> session,
		const FolderBlob &cloud) {
	auto blob = CollectMercurygramFilters(&session->data(), 0);
	blob.folders.erase(
		ranges::remove_if(blob.folders, [&](const FolderBlobEntry &entry) {
			return !ranges::contains(
				cloud.folders,
				entry.id,
				&FolderBlobEntry::id);
		}),
		end(blob.folders));
	return FoldersKey(std::move(blob));
}

[[nodiscard]] QString Caption(not_null<Main::Session*> session) {
	auto result = kCaptionTag;
	for (const auto &filter : session->data().chatsFilters().list()) {
		if (!IsMercurygramFolderId(filter.id())) {
			continue;
		}
		const auto line = '\n'
			+ QString::fromUtf8("\xF0\x9F\x93\x81 ")
			+ filter.titleText().text
			+ u" ("_q
			+ tr::lng_filters_chats_count(
				tr::now,
				lt_count_short,
				filter.always().size())
			+ ')';
		if (result.size() + line.size() > kCaptionLimit) {
			break;
		}
		result += line;
	}
	return result;
}

// Sent from memory rather than through a file: folder names and chat ids never
// touch the disk outside the encrypted tdata storage, and there is nothing to
// clean up after the upload. The file name is what the other devices match on,
// so displayName carries it into documentAttributeFilename.
[[nodiscard]] Ui::PreparedList BlobAsFile(const QByteArray &json) {
	auto file = Ui::PreparedFile(QString());
	file.content = json;
	file.displayName = kFileName;
	file.size = json.size();
	file.information = std::make_unique<Ui::PreparedFileInformation>();
	file.information->filemime = u"application/json"_q;
	auto result = Ui::PreparedList();
	result.files.push_back(std::move(file));
	return result;
}

} // namespace

MercurygramFolderSync::MercurygramFolderSync(not_null<Main::Session*> session)
: _session(session)
, _pushTimer([=] { push(); })
, _pushTimeout([=] { pushFailed(); }) {
	session->data().chatsFilters().changed(
	) | rpl::on_next([=] {
		handleFiltersChange();
	}, _lifetime);

	using Flag = Data::MessageUpdate::Flag;
	session->changes().messageUpdates(
		Flag::NewAdded | Flag::Edited
	) | rpl::on_next([=](const Data::MessageUpdate &update) {
		handleMessage(update.item);
	}, _lifetime);

	session->changes().messageUpdates(
		Flag::Destroyed
	) | rpl::on_next([=](const Data::MessageUpdate &update) {
		handleDestroyed(update.item);
	}, _lifetime);

	// Our own send lands under a client-side id first, one the other devices
	// would never see; this is where it becomes the id to edit next time.
	session->data().itemIdChanged(
	) | rpl::on_next([=](const Data::Session::IdChange &change) {
		if (const auto item = session->data().message(change.newId)) {
			handleMessage(item);
		}
	}, _lifetime);

	session->downloaderTaskFinished(
	) | rpl::on_next([=] {
		if (_view) {
			pull();
		}
	}, _lifetime);
}

MercurygramFolderSync::~MercurygramFolderSync() = default;

void MercurygramFolderSync::handleFiltersChange() {
	if (!_session->data().chatsFilters().loaded()) {
		return;
	} else if (!_started) {
		// The cloud is read before the first push, so a device that has just
		// been set up adopts the copy in the cloud instead of replacing it.
		find();
	} else {
		schedulePush();
	}
}

void MercurygramFolderSync::handleMessage(not_null<HistoryItem*> item) {
	if (!item->history()->peer->isSelf()
		|| !item->isRegular() // Our own send, before the server confirmed it.
		|| !isOurDocument(item)) {
		return;
	} else if (_pushing) {
		pushDone();
	}
	adopt(item->id);
}

void MercurygramFolderSync::handleDestroyed(not_null<HistoryItem*> item) {
	if (!_messageId
		|| (item->id != _messageId)
		|| !item->history()->peer->isSelf()) {
		return;
	}
	// Reached when the history is only unloaded from memory as well, so the
	// search decides: it adopts the document again if the server still has it.
	_messageId = 0;
	_started = false;
	find();
}

bool MercurygramFolderSync::isOurDocument(not_null<HistoryItem*> item) const {
	const auto media = item->media();
	const auto document = media ? media->document() : nullptr;
	return document && (document->filename() == kFileName);
}

HistoryItem *MercurygramFolderSync::message() const {
	return _messageId
		? _session->data().message(_session->userPeerId(), _messageId)
		: nullptr;
}

void MercurygramFolderSync::find() {
	if (_searching) {
		return;
	}
	_started = _searching = true;
	_session->api().request(MTPmessages_Search(
		MTP_flags(0),
		MTP_inputPeerSelf(),
		MTP_string(kCaptionTag),
		MTP_inputPeerEmpty(), // from_id
		MTPInputPeer(), // saved_peer_id
		MTPVector<MTPReaction>(), // saved_reaction
		MTPint(), // top_msg_id
		MTP_inputMessagesFilterDocument(),
		MTP_int(0), // min_date
		MTP_int(0), // max_date
		MTP_int(0), // offset_id
		MTP_int(0), // add_offset
		MTP_int(kSearchLimit),
		MTP_int(0), // max_id
		MTP_int(0), // min_id
		MTP_long(0) // hash
	)).done([=](const MTPmessages_Messages &result) {
		_searching = false;
		auto found = MsgId();
		result.match([](const MTPDmessages_messagesNotModified &) {
		}, [&](const auto &data) {
			_session->data().processUsers(data.vusers());
			_session->data().processChats(data.vchats());
			_session->data().processMessages(
				data.vmessages(),
				NewMessageType::Existing);
			for (const auto &message : data.vmessages().v) {
				const auto id = IdFromMessage(message);
				const auto item = _session->data().message(
					_session->userPeerId(),
					id);
				if (item && isOurDocument(item) && (id > found)) {
					found = id;
				}
			}
		});
		if (found) {
			adopt(found);
		} else {
			// Nothing in the cloud: the push guard must not believe it
			// already holds this set, or nothing would ever be sent.
			_cloudFolders = QByteArray();
			schedulePush();
		}
	}).fail([=] {
		_searching = false;
		_started = false; // Retried on the next folder change.
	}).send();
}

void MercurygramFolderSync::adopt(MsgId messageId) {
	if (_messageId && (messageId < _messageId)) {
		return; // Several documents: the newest one is the live copy.
	}
	_messageId = messageId;
	pull();
}

void MercurygramFolderSync::pull() {
	const auto item = message();
	const auto media = item ? item->media() : nullptr;
	const auto document = media ? media->document() : nullptr;
	if (!document || (document->size > kMaxBlobSize)) {
		// Dropped, or every later download in the app would come back here
		// through downloaderTaskFinished and find the same nothing.
		_view = nullptr;
		return;
	}
	_view = document->createMediaView();
	if (!_view->loaded()) {
		document->forceToCache(true);
		document->save(Data::FileOriginMessage(item->fullId()), QString());
		return; // Continued from downloaderTaskFinished.
	}
	const auto parsed = ParseFolderBlob(_view->bytes());
	_view = nullptr;
	if (parsed) {
		apply(*parsed);
	}
}

void MercurygramFolderSync::apply(const FolderBlob &blob) {
	const auto owner = &_session->data();
	const auto filters = &owner->chatsFilters();
	const auto stored = SavedMercurygramFolders(_session).updated;
	if (blob.updated > stored) {
		// A device that has never synced holds folders the copy in the cloud
		// cannot know about, because they predate the sync: those are kept and
		// pushed below, instead of reading their absence as a deletion.
		const auto firstSync = !stored;
		if (!firstSync) {
			auto obsolete = std::vector<FilterId>();
			for (const auto &filter : filters->list()) {
				const auto id = filter.id();
				if (IsMercurygramFolderId(id)
					&& !ranges::contains(
						blob.folders,
						id,
						&FolderBlobEntry::id)) {
					obsolete.push_back(id);
				}
			}
			for (const auto id : obsolete) {
				filters->remove(id);
			}
		}
		for (auto &filter : ParseMercurygramFilters(owner, blob)) {
			const auto &list = filters->list();
			const auto i = ranges::find(
				list,
				filter.id(),
				&Data::ChatFilter::id);
			if (i != end(list)) {
				// The peers the blob cannot name -- a secret chat lives on
				// this device alone and has no Bot API id -- are carried over
				// by hand, or a pull from another device would quietly drop
				// them out of the folder here.
				auto always = filter.always();
				auto never = filter.never();
				for (const auto &history : i->always()) {
					if (!SyncablePeer(history->peer->id)) {
						always.emplace(history);
					}
				}
				for (const auto &history : i->never()) {
					if (!SyncablePeer(history->peer->id)) {
						never.emplace(history);
					}
				}
				filter = Data::ChatFilter(
					filter.id(),
					filter.title(),
					filter.iconEmoji(),
					filter.colorIndex(),
					filter.flags(),
					std::move(always),
					filter.pinned(),
					std::move(never));
			}
			filters->set(filter);
		}
		SaveMercurygramFolders(_session, blob.updated);
		_cloudFolders = CloudKey(_session, blob);
	} else if (blob.updated == stored) {
		_cloudFolders = CloudKey(_session, blob);
	}
	schedulePush();
}

void MercurygramFolderSync::schedulePush() {
	_pushTimer.callOnce(kPushDelay);
}

void MercurygramFolderSync::push() {
	if (!_started || _searching) {
		// Pushing while the search is in flight would send a second document
		// instead of editing the one the search is about to return; the search
		// schedules a push itself when it lands.
		return;
	} else if (_pushing) {
		schedulePush();
		return;
	}
	const auto stored = SavedMercurygramFolders(_session).updated;
	const auto folders = FoldersKey(_session);
	if (folders == _cloudFolders) {
		return;
	} else if (!stored && !MercurygramFilterCount(
			_session->data().chatsFilters().list())) {
		return; // A device with nothing yet never clears the cloud copy.
	}
	const auto updated = std::max(base::unixtime::now(), stored + 1);
	auto list = BlobAsFile(SerializeFolderBlob(
		CollectMercurygramFilters(&_session->data(), updated)));
	auto caption = TextWithTags{ Caption(_session) };
	auto action = Api::SendAction(
		_session->data().history(_session->userPeerId()));
	action.clearDraft = false;

	_pushing = true;
	_foldersBefore = _cloudFolders;
	_updatedBefore = stored;
	_cloudFolders = folders;
	SaveMercurygramFolders(_session, updated);
	_pushTimeout.callOnce(kPushTimeout);

	if (const auto item = message()) {
		action.replaceMediaOf = item->id;
		_session->api().editMedia(
			std::move(list),
			SendMediaType::File,
			std::move(caption),
			action);
	} else {
		list.files.front().caption = std::move(caption);
		_session->api().sendFiles(
			std::move(list),
			SendMediaType::File,
			nullptr,
			action);
	}
}

void MercurygramFolderSync::pushDone() {
	_pushing = false;
	_pushTimeout.cancel();
}

void MercurygramFolderSync::pushFailed() {
	// Give back what the push claimed. Keeping it would have this session
	// believe the cloud holds this set, and ignore a copy from another device
	// whose "updated" is below the value the push bumped it to.
	_pushing = false;
	_cloudFolders = _foldersBefore;
	SaveMercurygramFolders(_session, _updatedBefore);
	// An edit fails exactly like this when the document was deleted from Saved
	// Messages, so the next folder change searches again and sends a new one.
	_messageId = 0;
	_started = false;
}

} // namespace MG
