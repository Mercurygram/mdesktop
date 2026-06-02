/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "api/api_encrypted_chats.h"

#include "apiwrap.h"
#include "main/main_session.h"
#include "data/data_session.h"
#include "data/data_changes.h"
#include "data/data_user.h"
#include "data/data_secret_chat.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_photo.h"
#include "data/stickers/data_stickers.h"
#include "data/stickers/data_stickers_set.h"
#include "data/data_send_action.h"
#include "data/data_media_types.h"
#include "data/data_file_origin.h"
#include "ui/image/image.h"
#include "ui/image/image_location_factory.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_helpers.h"
#include "mtproto/mtproto_dh_utils.h"
#include "mtproto/mtproto_auth_key.h"
#include "mtproto/facade.h"
#include "mtproto/secret_chat/secret_chat_dh.h"
#include "mtproto/secret_chat/secret_chat_encryption.h"
#include "base/random.h"
#include "base/unixtime.h"
#include "core/file_location.h"
#include "core/mime_type.h"
#include "lang/lang_keys.h"
#include "lang/lang_hardcoded.h"
#include "window/window_session_controller.h"
#include "storage/storage_account.h"
#include "storage/storage_encrypted_file.h"
#include "storage/storage_encryption.h"
#include "storage/file_download.h"
#include "storage/file_upload.h"
#include "storage/localimageloader.h"
#include "storage/download_manager_mtproto.h"
#include "settings.h"

#include "secret_scheme.h"

#include <QtCore/QBuffer>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtGui/QImage>

namespace Api {
namespace {

constexpr auto kRandomPowerSize = 256;
constexpr auto kDhRandomLength = 256;

// Perfect-Forward-Secrecy rekey triggers: a key is rotated after it has
// encrypted/decrypted this many messages, or after this much time, whichever
// comes first (per core.telegram.org/api/end-to-end/pfs). The out/in thresholds
// are asymmetric to match the mobile clients (SecretChatHelper.java:627,818):
// each side rotates after sending 100 messages; the 120-received fallback only
// fires if the peer failed to initiate, so the sender's own trigger wins first.
constexpr auto kRekeyEveryOut = 100;
constexpr auto kRekeyEveryIn = 120;
constexpr auto kRekeyAfter = 7 * 24 * 60 * 60; // one week, in seconds.

// PFS rekeying was introduced in secret-chat layer 23; older peers cannot
// negotiate a new key, so we never initiate a rekey below this layer.
constexpr auto kMinRekeyLayer = 23;

// Sequence-number reliability (mirrors Android SecretChatHelper). On an incoming
// gap we buffer the newer messages until the missing ones are resent; once this
// many holes pile up the stream is treated as unrecoverable and the chat is
// discarded, matching the mobile clients.
constexpr auto kMaxHoles = 4;

// How many recently sent decrypted-layer payloads we keep per chat to answer a
// peer's decryptedMessageActionResend. Older entries are dropped; a request for
// a dropped (or never-cached, e.g. file) seq is satisfied with a delete
// tombstone so the peer's sequence stays intact.
constexpr auto kSentLayerCacheSize = 256;

// Defensive cap on a resend range so a malformed request cannot make us loop or
// flood the peer (Android caps the gap similarly).
constexpr auto kMaxResendRange = 2000;

// Defensive cap on an incoming encrypted file: its size arrives straight from
// the wire and the whole ciphertext is accumulated in a single QByteArray before
// decrypt, so an oversized (malicious or corrupt) value must be rejected up front
// rather than allowed to grow an unbounded buffer. Kept well below INT_MAX so the
// accumulator stays valid on Qt 5 / 32-bit builds (QByteArray caps at INT_MAX)
// and the peak allocation stays sane on 64-bit Qt 6.
constexpr auto kMaxIncomingFileSize = int64(2000) * 1024 * 1024;

[[nodiscard]] bytes::vector RandomPower() {
	auto result = bytes::vector(kRandomPowerSize);
	bytes::set_random(result);
	return result;
}

// A file extension (".mp4") for a mime type, so a rebuilt local video/audio
// document keeps a name type the renderer accepts (see DocumentData::setattributes
// name-type enforcement). Empty when the mime is unknown.
[[nodiscard]] QString ExtensionForMime(const QString &mime) {
	const auto globs = Core::MimeTypeForName(mime).globPatterns();
	if (globs.isEmpty()) {
		return QString();
	}
	auto pattern = globs.front(); // e.g. "*.mp4"
	if (pattern.startsWith('*')) {
		pattern = pattern.mid(1);
	}
	return pattern.startsWith('.') ? pattern : QString();
}

// Largest side of the small inline preview thumbnail carried (unencrypted) in
// decryptedMessageMediaPhoto/Video, matching what official clients send.
constexpr auto kThumbSide = 90;

// Build a small JPEG preview from a full image: scale so the longest side is at
// most kThumbSide, encode as JPEG. Returns the bytes and the scaled dimensions.
struct PreparedThumb {
	QByteArray bytes;
	int width = 0;
	int height = 0;
};
[[nodiscard]] PreparedThumb MakePhotoThumb(const QImage &full) {
	auto result = PreparedThumb();
	if (full.isNull()) {
		return result;
	}
	auto scaled = (full.width() > kThumbSide || full.height() > kThumbSide)
		? full.scaled(
			kThumbSide,
			kThumbSide,
			Qt::KeepAspectRatio,
			Qt::SmoothTransformation)
		: full;
	auto buffer = QBuffer(&result.bytes);
	buffer.open(QIODevice::WriteOnly);
	scaled.save(&buffer, "JPG", 87);
	buffer.close();
	result.width = scaled.width();
	result.height = scaled.height();
	return result;
}

// Copy a decrypted-scheme bytes span into a QByteArray (the underlying storage
// type varies, so go through the raw bytes).
[[nodiscard]] QByteArray BytesToQ(bytes::const_span span) {
	return QByteArray(
		reinterpret_cast<const char*>(span.data()),
		int(span.size()));
}

// Wrap the inline preview thumbnail carried with an incoming media message as a
// document thumbs list (a single photoCachedSize holding the JPEG bytes) so the
// rebuilt local document shows the preview. Empty list when no thumb is present.
[[nodiscard]] MTPVector<MTPPhotoSize> LocalDocumentThumbs(
		const QByteArray &thumb,
		int width,
		int height) {
	if (thumb.isEmpty()) {
		return MTPVector<MTPPhotoSize>();
	}
	auto list = QVector<MTPPhotoSize>();
	list.push_back(MTP_photoCachedSize(
		MTP_string("s"),
		MTP_int(width),
		MTP_int(height),
		MTP_bytes(thumb)));
	return MTP_vector<MTPPhotoSize>(list);
}

// Build the thumb:PhotoSize for an external document (sticker). Mobile only
// treats application/x-tgsticker as an animated sticker when the document's
// thumbs are non-empty -- a photoSizeEmpty does NOT satisfy that (the sticker
// then shows as a "File"). So we always emit a non-empty PhotoSize: a
// photoCachedSize carrying a small JPEG preview when one is loaded, else a
// photoSizeEmpty (type "s"). The receiver still adds an empty size to the
// document's thumbs (so the thumbs list is non-empty -- the sticker-recognition
// gate), but unlike a photoSize with a real-looking location it does NOT try to
// download a thumb that isn't there (that attempt briefly renders the message
// as a "File" before the sticker resolves). This mirrors the Android client.
[[nodiscard]] decrypted::MTPPhotoSize ExternalDocumentThumb(
		not_null<DocumentData*> document) {
	auto image = QImage();
	const auto view = document->createMediaView();
	// Prefer the inline (stripped) preview that ships WITH the sticker: it decodes
	// synchronously with no download, exactly like the photoStrippedSize the
	// Android client embeds (SendMessagesHelper.getThumbForSecretChat). So the peer
	// can display it immediately and never shows a "File" during its sticker-set
	// verification window. Only fall back to an already-decoded raster if there is
	// no inline preview -- never to a still-loading image (its original() is empty,
	// which would yield an empty thumb -> the "File" flash).
	if (const auto inlined = view->thumbnailInline()) {
		image = inlined->original();
	}
	if (image.isNull()) {
		if (const auto thumb = view->thumbnail()) {
			image = thumb->original();
		} else if (const auto large = view->getStickerLarge()) {
			image = large->original();
		} else if (const auto small = view->getStickerSmall()) {
			image = small->original();
		}
	}
	const auto prepared = MakePhotoThumb(image);
	if (!prepared.bytes.isEmpty()) {
		return decrypted::MTP_photoCachedSize(
			MTP_string("s"),
			decrypted::MTP_fileLocationUnavailable(
				MTP_long(0),
				MTP_int(0),
				MTP_long(0)),
			MTP_int(prepared.width),
			MTP_int(prepared.height),
			MTP_bytes(prepared.bytes));
	}
	return decrypted::MTP_photoSizeEmpty(MTP_string("s"));
}

// Translate a decrypted document's attributes into the api-scheme attributes
// processDocument expects, so a document sent as a video/animation/voice/image
// renders inline rather than as a generic file row. Unknown kinds are dropped.
[[nodiscard]] QVector<MTPDocumentAttribute> ConvertDecryptedAttributes(
		const decrypted::MTPVector<decrypted::MTPDocumentAttribute> &list) {
	auto result = QVector<MTPDocumentAttribute>();
	for (const auto &attribute : list.v) {
		attribute.match([&](
				const decrypted::MTPDdocumentAttributeImageSize &d) {
			result.push_back(MTP_documentAttributeImageSize(d.vw(), d.vh()));
		}, [&](const decrypted::MTPDdocumentAttributeAnimated &) {
			result.push_back(MTP_documentAttributeAnimated());
		}, [&](const decrypted::MTPDdocumentAttributeSticker &d) {
			// Render as a sticker using the document's own image; the original
			// sticker set isn't carried across (empty set), and alt/emoji is
			// cosmetic (secret-scheme strings are byte-backed, skip it).
			result.push_back(MTP_documentAttributeSticker(
				MTP_flags(0),
				MTP_string(),
				MTP_inputStickerSetEmpty(),
				MTPMaskCoords()));
		}, [&](const decrypted::MTPDdocumentAttributeVideo &d) {
			using Flag = MTPDdocumentAttributeVideo::Flag;
			result.push_back(MTP_documentAttributeVideo(
				MTP_flags(d.is_round_message() ? Flag::f_round_message : Flag()),
				MTP_double(d.vduration().v),
				d.vw(),
				d.vh(),
				MTPint(), // preload_prefix_size
				MTPdouble(), // video_start_ts
				MTPstring())); // video_codec
		}, [&](const decrypted::MTPDdocumentAttributeAudio &d) {
			// Only the voice flag + duration drive rendering; title/performer are
			// cosmetic for secret-chat audio, so they are left empty.
			using Flag = MTPDdocumentAttributeAudio::Flag;
			const auto flags = d.is_voice() ? Flag::f_voice : Flag();
			result.push_back(MTP_documentAttributeAudio(
				MTP_flags(flags),
				d.vduration(),
				MTPstring(),
				MTPstring(),
				MTPbytes()));
		}, [&](const decrypted::MTPDdocumentAttributeFilename &d) {
			result.push_back(MTP_documentAttributeFilename(d.vfile_name()));
		}, [&](const auto &) {
		});
	}
	return result;
}

// Pull the file name out of a decrypted document's attributes, if present.
[[nodiscard]] QString DocumentName(
		const decrypted::MTPDdecryptedMessageMediaDocument &doc) {
	for (const auto &attribute : doc.vattributes().v) {
		auto result = QString();
		attribute.match([&](
				const decrypted::MTPDdocumentAttributeFilename &data) {
			result = qs(data.vfile_name());
		}, [&](const auto &) {
		});
		if (!result.isEmpty()) {
			return result;
		}
	}
	return QString();
}

} // namespace

EncryptedChats::EncryptedChats(not_null<ApiWrap*> api)
: _api(api)
, _session(&api->session())
, _mtp(&api->instance()) {
}

EncryptedChats::~EncryptedChats() = default;

bool EncryptedChats::dhConfigReady() const {
	return (_dhConfig.version >= 0)
		&& (_dhConfig.g != 0)
		&& !_dhConfig.p.empty();
}

void EncryptedChats::ensureDhConfig(Fn<void()> done) {
	if (dhConfigReady()) {
		done();
		return;
	}
	_mtp.request(MTPmessages_GetDhConfig(
		MTP_int(_dhConfig.version),
		MTP_int(kDhRandomLength)
	)).done([=](const MTPmessages_DhConfig &result) {
		result.match([&](const MTPDmessages_dhConfig &data) {
			const auto prime = bytes::make_span(data.vp().v);
			if (!MTP::SecretChat::ValidateDhConfig(data.vg().v, prime)) {
				LOG(("Secret Chat Error: Bad DH config from server."));
				return;
			}
			_dhConfig.version = data.vversion().v;
			_dhConfig.g = data.vg().v;
			_dhConfig.p = bytes::make_vector(prime);
		}, [&](const MTPDmessages_dhConfigNotModified &) {
		});
		if (dhConfigReady()) {
			done();
		} else {
			// The server returned successfully but left us without a usable
			// config (bad prime, or dhConfigNotModified while we have none
			// cached). Don't silently swallow it: the pending operation is now
			// stranded, so at least make the dead end diagnosable.
			LOG(("Secret Chat Error: DH config unavailable, request dropped."));
		}
	}).fail([=] {
		LOG(("Secret Chat Error: Could not get DH config."));
	}).send();
}

void EncryptedChats::create(not_null<UserData*> user) {
	ensureDhConfig([=] {
		const auto power = RandomPower();
		const auto first = MTP::CreateModExp(_dhConfig.g, _dhConfig.p, power);
		if (first.modexp.empty()) {
			LOG(("Secret Chat Error: Could not create g_a."));
			return;
		}
		const auto randomId = base::RandomValue<int32>();
		const auto randomPower = first.randomPower;
		_mtp.request(MTPmessages_RequestEncryption(
			user->inputUser(),
			MTP_int(randomId),
			MTP_bytes(first.modexp)
		)).done([=](const MTPEncryptedChat &result) {
			if (const auto chat = applyUpdateChat(result)) {
				// A discard could already have landed for this id (e.g. an
				// immediate close from the other side): don't (re)open a dead chat.
				if (chat->state() == SecretChatState::Discarded) {
					return;
				}
				_pending[chat->secretChatId()] = Pending{
					.randomPower = randomPower,
				};
				chat->setUser(user);
				DEBUG_LOG(("Secret Chat: requested chat %1, awaiting accept."
					).arg(chat->secretChatId()));
				// Open the new chat right away (the "waiting for the other
				// party" screen), as the mobile clients do, instead of leaving
				// the user to find it in the dialog list. Mirrors the window
				// resolution used by applyRequested().
				if (const auto window = _session->tryResolveWindow()) {
					window->showPeerHistory(chat);
				}
			}
		}).fail([=](const MTP::Error &error) {
			LOG(("Secret Chat Error: requestEncryption failed: %1"
				).arg(error.type()));
		}).send();
	});
}

void EncryptedChats::accept(not_null<SecretChatData*> chat) {
	if (chat->state() != SecretChatState::Waiting) {
		return;
	}
	const auto id = chat->secretChatId();
	const auto i = _pending.find(id);
	if (i == _pending.end() || i->second.gA.empty()) {
		LOG(("Secret Chat Error: No g_a to accept chat %1.").arg(id));
		return;
	}
	const auto gA = i->second.gA;
	ensureDhConfig([=] {
		const auto power = RandomPower();
		const auto first = MTP::CreateModExp(_dhConfig.g, _dhConfig.p, power);
		if (first.modexp.empty()) {
			return;
		}
		const auto computed = MTP::SecretChat::ComputeKey(
			gA,
			first.randomPower,
			_dhConfig.p);
		if (!computed) {
			LOG(("Secret Chat Error: Bad g_a, cannot accept."));
			return;
		}
		_mtp.request(MTPmessages_AcceptEncryption(
			inputChat(chat),
			MTP_bytes(first.modexp),
			MTP_long(computed->fingerprint)
		)).done([=](const MTPEncryptedChat &result) {
			// The partner may have discarded the chat while our acceptEncryption
			// was in flight (the discard update wins): don't resurrect a dead chat.
			if (chat->state() == SecretChatState::Discarded) {
				return;
			}
			chat->setKey(computed->key, computed->fingerprint);
			chat->setState(SecretChatState::Ready);
			applyUpdateChat(result);
			sendNotifyLayer(chat);
			flushPendingSends(chat);
			writeLocal();
			DEBUG_LOG(("Secret Chat: accepted chat %1, Ready, fingerprint %2."
				).arg(chat->secretChatId()
				).arg(QString::number(computed->fingerprint, 16)));
		}).fail([=](const MTP::Error &error) {
			LOG(("Secret Chat Error: acceptEncryption failed: %1"
				).arg(error.type()));
		}).send();
	});
}

void EncryptedChats::discard(
		not_null<SecretChatData*> chat,
		bool deleteHistory) {
	using Flag = MTPmessages_DiscardEncryption::Flag;
	auto flags = MTPmessages_DiscardEncryption::Flags();
	if (deleteHistory) {
		flags |= Flag::f_delete_history;
	}
	_mtp.request(MTPmessages_DiscardEncryption(
		MTP_flags(flags),
		MTP_int(chat->secretChatId())
	)).fail([=](const MTP::Error &error) {
		LOG(("Secret Chat Error: discardEncryption failed: %1"
			).arg(error.type()));
	}).send();
	// The user closed the chat: always wipe the local history too (the dead,
	// keyless row must not linger with its messages).
	clearLocalState(chat, true);
}

void EncryptedChats::clearLocalState(
		not_null<SecretChatData*> chat,
		bool clearHistory) {
	chat->setState(SecretChatState::Discarded);
	// Drop the closed chat out of the dialog list (shouldBeInChatList now
	// returns false for a Discarded secret chat), optionally clearing its local
	// history.
	const auto history = _session->data().history(chat->id);
	if (clearHistory) {
		history->clear(History::ClearType::ClearHistory);
	}
	history->updateChatListExistence();
	clearChatRandomIds(chat->id);
	_messageTtls.remove(chat->id);
	_ttlNotices.remove(chat->id);
	// Drop the seq-no reliability state: a discarded chat can neither buffer
	// out-of-order messages nor answer resend requests.
	_holes.remove(chat->secretChatId());
	_sentLayers.remove(chat->secretChatId());
	// Drop the remaining per-chat state. A discarded chat never reaches Ready, so
	// queued sends would never flush (and _pendingSends can hold large prepared
	// upload payloads); the handshake/rekey maps would otherwise keep DH private
	// exponents alive for the rest of the session. The rpl::lifetime in
	// _ttlNoticeNameWatch is torn down by removing its entry.
	_pendingSends.remove(chat->secretChatId());
	_rekeys.remove(chat->secretChatId());
	_pending.remove(chat->secretChatId());
	_readTillDates.remove(chat->secretChatId());
	_sendAfter.remove(chat->secretChatId());
	_ttlNoticeNameWatch.remove(chat->secretChatId());
	writeLocal();
	writeMessagesLocal();
}

void EncryptedChats::reportSpam(not_null<SecretChatData*> chat) {
	_mtp.request(MTPmessages_ReportEncryptedSpam(
		inputChat(chat)
	)).send();
	// Reporting spam in a secret chat closes it (and clears the history), the
	// same as the official clients.
	discard(chat, true);
}

void EncryptedChats::processUpdate(const MTPEncryptedChat &chat) {
	applyUpdateChat(chat);
}

void EncryptedChats::writeLocal() {
	_session->local().writeSecretChats();
}

void EncryptedChats::setQts(int32 qts) {
	if (qts <= _qts) {
		return;
	}
	_qts = qts;
	// Persist the new checkpoint (rides the secret-chats blob) so a relaunch
	// can pull anything that arrived while we were offline...
	writeLocal();
	// ...and ACK the server so it stops re-delivering the encrypted updates we
	// have now processed (mirrors Android's messages.receivedQueue(max_qts)).
	_mtp.request(MTPmessages_ReceivedQueue(
		MTP_int(_qts)
	)).send();
}

namespace {

constexpr auto kSecretMessagesVersion = qint32(1);
enum class StoredKind : qint32 {
	Text = 0,
	Document = 1,
	Photo = 2,
	// A public/server document referenced by id+access_hash+dc (stickers and
	// other external docs). Unlike a local E2E file it has a remote location and
	// no on-disk blob; restored by rebuilding that remote-location document so
	// the standard loader re-downloads it.
	External = 3,
	// A self-destruct timer change notice (service message): persisted with the
	// ttl seconds so it re-renders as a centered service notice, not a text bubble.
	ServiceTtl = 4,
};
// Media shape persisted alongside a document, so a restored video/voice
// rebuilds the attributes that drive inline rendering. Photos render inline from
// mime+filename alone, but are tagged too for correct dimensions.
enum class StoredMedia : qint32 {
	File = 0,
	Image = 1,
	Video = 2,
	Voice = 3,
	// A GIF/animation: restored with documentAttributeVideo + documentAttribute
	// Animated so it renders (and auto-plays) as a GIF, not a plain video.
	Gif = 4,
	// A round video note: restored with documentAttributeVideo(round_message) so
	// it renders as a round video, not a GIF. NB DocumentData::isAnimation() is
	// true for round videos, so this MUST be checked before the Gif branch.
	Round = 5,
};

// Human-readable self-destruct duration for the in-chat TTL-change notice. Mirrors
// the picker labels (window_peer_menu.cpp) so short presets read as seconds/minutes,
// not "0 hours" (Ui::FormatTTL collapses anything sub-day to hours).
} // namespace

QString SecretChatTtlDuration(int seconds) {
	if (seconds < 60) {
		return tr::lng_seconds(tr::now, lt_count, seconds);
	} else if (seconds < 3600) {
		return tr::lng_minutes(tr::now, lt_count, seconds / 60);
	} else if (seconds < 86400) {
		return tr::lng_hours(tr::now, lt_count, seconds / 3600);
	} else if (seconds < 7 * 86400) {
		return tr::lng_days(tr::now, lt_count, seconds / 86400);
	}
	return tr::lng_weeks(tr::now, lt_count, seconds / (7 * 86400));
}

QByteArray EncryptedChats::serializeMessages() const {
	// Collect, per chat, the persistable messages in id order (the flat_map is
	// already sorted ascending, i.e. oldest first). Every text/file message is
	// registered in _messageRandomIds, so that index is our message list; the
	// live HistoryItem carries the content we serialize.
	struct Chat {
		qint32 secretChatId = 0;
		PeerId peerId = 0;
		SecretChatData *data = nullptr;
		const base::flat_map<MsgId, uint64> *messages = nullptr;
	};
	auto chats = std::vector<Chat>();
	_session->data().enumerateSecretChats([&](
			not_null<SecretChatData*> chat) {
		const auto i = _messageRandomIds.find(chat->id);
		if (i != _messageRandomIds.end() && !i->second.empty()) {
			chats.push_back({ chat->secretChatId(), chat->id, chat, &i->second });
		}
	});
	if (chats.empty()) {
		return QByteArray();
	}

	auto result = QByteArray();
	auto stream = QDataStream(&result, QIODevice::WriteOnly);
	stream.setVersion(QDataStream::Qt_5_1);
	stream << kSecretMessagesVersion << qint32(chats.size());
	for (const auto &chat : chats) {
		// Stream into a temporary so we can prefix the real persisted count
		// (items destroyed since registration are skipped).
		auto body = QByteArray();
		auto bodyStream = QDataStream(&body, QIODevice::WriteOnly);
		bodyStream.setVersion(QDataStream::Qt_5_1);
		auto count = qint32(0);
		for (const auto &[msgId, randomId] : *chat.messages) {
			const auto item = _session->data().message(
				FullMsgId(chat.peerId, msgId));
			if (!item) {
				continue;
			}
			// A self-destruct timer change notice (centered service message). Its
			// persisted ttl fields are forced to 0 (a notice must never self-destruct).
			const auto notice = [&]() -> std::optional<qint32> {
				const auto n = _ttlNotices.find(chat.peerId);
				if (n == _ttlNotices.end()) {
					return std::nullopt;
				}
				const auto m = n->second.find(msgId);
				return (m != n->second.end())
					? std::make_optional(qint32(m->second))
					: std::nullopt;
			}();
			const auto media = item->media();
			const auto document = media ? media->document() : nullptr;
			const auto photo = (media && !document) ? media->photo() : nullptr;
			// External/public documents (stickers) reference a server file by
			// id/access_hash/dc and carry no on-disk blob -> persisted as a
			// StoredKind::External record (rebuilt with that remote location).
			// (Local E2E files have no remote location.)
			const auto external = document && document->hasRemoteLocation();
			// Persist outgoing read-state so restored bubbles keep ✓✓. Local
			// MsgIds are reassigned on restore, so the outbox-read-till can't
			// be carried verbatim; instead we flag each read outgoing message
			// and rebuild the till in the new id space (see restoreMessages).
			const auto read = item->out() && !item->unread(item->history());
			// Self-destruct (TTL), Android model: persist the PENDING ttl seconds of
			// an UNREAD message (no countdown yet) so it survives restart and arms on
			// the next read, AND the ABSOLUTE ttlDestroyAt of a READ message so its
			// countdown resumes from the remaining time (never restarts) -- the two are
			// mutually exclusive (startSelfDestructTimers erases the pending entry when
			// it arms). A notice has no timer -> force 0 (it must never self-destruct).
			auto ttlSeconds = qint32(0);
			if (!notice) {
				if (const auto k = _messageTtls.find(chat.peerId);
						k != _messageTtls.end()) {
					if (const auto m = k->second.find(msgId);
							m != k->second.end()) {
						ttlSeconds = m->second;
					}
				}
			}
			++count;
			bodyStream
				<< qint64(msgId.bare)
				<< quint64(randomId)
				<< qint32(item->date())
				<< qint32(item->out() ? 1 : 0)
				<< qint32(read ? 1 : 0)
				<< ttlSeconds
				<< qint32(notice ? 0 : item->ttlDestroyAt());
			if (notice) {
				// Persist the rendered text (name resolved now, while the partner
				// user is loaded) so the restored notice keeps the real name even if
				// that minimal partner never re-resolves after the restart.
				bodyStream
					<< qint32(StoredKind::ServiceTtl)
					<< *notice
					<< ttlNoticeText(chat.data, *notice, item->out()).text;
			} else if (external) {
				// Server-side document (sticker / public doc): persist the remote
				// location + the shape needed to rebuild the rendering attributes.
				// IMPORTANT: persist the REAL file_reference + sticker set, because
				// restore reuses processDocument which merges into the shared (id-
				// keyed) DocumentData -- rebuilding with empty set/file_reference
				// would clobber the user's own sticker so a later re-send loses its
				// set short name (animated sticker -> "File" on the peer).
				const auto sticker = document->sticker();
				const auto isVideo = document->isVideoFile()
					|| document->isAnimation()
					|| document->isVideoMessage();
				bodyStream
					<< qint32(StoredKind::External)
					<< quint64(document->id)
					<< quint64(document->getAccessHash())
					<< qint32(document->getDC())
					<< document->fileReference()
					<< document->mimeString()
					<< document->filename()
					<< qint64(document->size)
					<< item->originalText().text
					<< qint32(sticker ? 1 : 0)
					<< (sticker ? sticker->alt : QString())
					<< (sticker ? sticker->set.shortName : QString())
					<< quint64(sticker ? sticker->set.id : 0)
					<< quint64(sticker ? sticker->set.accessHash : 0)
					<< qint32(document->dimensions.width())
					<< qint32(document->dimensions.height())
					<< qint32(isVideo ? 1 : 0)
					<< qint32(document->duration() / 1000);
			} else if (document) {
				// Capture the media shape so a restored video/voice rebuilds the
				// attributes that select inline rendering.
				auto stored = StoredMedia::File;
				auto w = qint32(0), h = qint32(0), duration = qint32(0);
				if (document->isVideoMessage()
					|| document->type == RoundVideoDocument) {
					// Round video: must be checked BEFORE isAnimation() (which is
					// true for round videos) so it does not get tagged as a Gif.
					stored = StoredMedia::Round;
					w = document->dimensions.width();
					h = document->dimensions.height();
					duration = qint32(document->duration() / 1000);
				} else if (document->type == AnimatedDocument
					|| document->isAnimation()) {
					stored = StoredMedia::Gif;
					w = document->dimensions.width();
					h = document->dimensions.height();
					duration = qint32(document->duration() / 1000);
				} else if (document->isVideoFile()) {
					stored = StoredMedia::Video;
					w = document->dimensions.width();
					h = document->dimensions.height();
					duration = qint32(document->duration() / 1000);
				} else if (document->isVoiceMessage()) {
					stored = StoredMedia::Voice;
					duration = qint32(document->duration() / 1000);
				} else if (document->isImage()) {
					stored = StoredMedia::Image;
					w = document->dimensions.width();
					h = document->dimensions.height();
				}
				bodyStream
					<< qint32(StoredKind::Document)
					<< quint64(document->id)
					<< document->secretEncryptedPath()
					<< document->mimeString()
					<< document->filename()
					<< qint64(document->size)
					<< item->originalText().text
					<< qint32(stored)
					<< w << h << duration;
			} else if (photo) {
				// Photos are rebuilt as PhotoData from the encrypted on-disk JPEG;
				// persist its path so the inline image reloads on restart.
				bodyStream
					<< qint32(StoredKind::Photo)
					<< quint64(photo->id)
					<< photo->secretEncryptedPath()
					<< item->originalText().text;
			} else {
				bodyStream
					<< qint32(StoredKind::Text)
					<< item->originalText().text;
			}
		}
		stream << chat.secretChatId << count;
		stream.writeRawData(body.constData(), body.size());
	}
	return result;
}

void EncryptedChats::writeMessagesLocal() {
	_session->local().writeSecretChatMessages(serializeMessages());
}

void EncryptedChats::restoreMessages(const QByteArray &serialized) {
	if (serialized.isEmpty()) {
		return;
	}
	auto stream = QDataStream(serialized);
	stream.setVersion(QDataStream::Qt_5_1);
	auto version = qint32(0), chatCount = qint32(0);
	stream >> version >> chatCount;
	if (stream.status() != QDataStream::Ok
		|| (version < 1 || version > kSecretMessagesVersion)
		|| chatCount < 0) {
		DEBUG_LOG(("Secret chats: bad messages header."));
		return;
	}
	for (auto c = 0; c != chatCount; ++c) {
		auto secretChatId = qint32(0), messageCount = qint32(0);
		stream >> secretChatId >> messageCount;
		if (stream.status() != QDataStream::Ok || messageCount < 0) {
			return;
		}
		const auto chat = _session->data().secretChatLoaded(
			secretChatIdFromWire(secretChatId));
		const auto history = chat
			? _session->data().history(chat->id).get()
			: nullptr;
		// Highest fresh id among read outgoing messages -> the rebuilt
		// outbox-read-till for this chat (read-ness is a prefix by date and we
		// restore oldest-first, so the last read outgoing has the highest id).
		auto maxReadOutId = std::optional<MsgId>();
		for (auto m = 0; m != messageCount; ++m) {
			auto storedId = qint64(0), size = qint64(0);
			auto randomId = quint64(0);
			auto date = qint32(0), out = qint32(0), kind = qint32(0);
			auto read = qint32(0);
			auto fileId = quint64(0);
			auto text = QString(), path = QString(), mime = QString();
			auto filename = QString(), caption = QString();
			auto stored = qint32(StoredMedia::File);
			auto w = qint32(0), h = qint32(0), duration = qint32(0);
			auto accessHash = quint64(0), setId = quint64(0);
			auto setAccessHash = quint64(0);
			auto dcId = qint32(0), isSticker = qint32(0), isVideo = qint32(0);
			auto alt = QString(), setShortName = QString();
			auto fileReference = QByteArray();
			auto ttlSeconds = qint32(0), ttlDestroyAt = qint32(0);
			stream >> storedId >> randomId >> date >> out >> read;
			stream >> ttlSeconds >> ttlDestroyAt;
			auto ttlNoticeSeconds = qint32(0);
			auto ttlNoticeText = QString();
			stream >> kind;
			if (kind == qint32(StoredKind::ServiceTtl)) {
				stream >> ttlNoticeSeconds >> ttlNoticeText;
			} else if (kind == qint32(StoredKind::Document)) {
				stream >> fileId >> path >> mime >> filename >> size >> caption
					>> stored >> w >> h >> duration;
			} else if (kind == qint32(StoredKind::Photo)) {
				stream >> fileId >> path >> caption;
			} else if (kind == qint32(StoredKind::External)) {
				stream >> fileId >> accessHash >> dcId >> fileReference >> mime
					>> filename >> size >> caption >> isSticker >> alt
					>> setShortName >> setId >> setAccessHash >> w >> h
					>> isVideo >> duration;
			} else {
				stream >> text;
			}
			if (stream.status() != QDataStream::Ok) {
				return;
			} else if (!chat || !chat->user()) {
				continue; // Keep parsing the stream, just drop this chat.
			}
			// A self-destruct message that was already counting down and whose
			// absolute destroy time passed while the app was closed is gone: skip
			// rebuilding it entirely (Android deletes such a task on load without
			// showing the message). A still-armed message (destroyAt in the future)
			// is rebuilt below and its countdown resumes from the remaining time.
			if (ttlDestroyAt > 0 && ttlDestroyAt <= base::unixtime::now()) {
				continue;
			}
			// A restored message is already-read history: no ClientSideUnread,
			// so the read path doesn't re-fire. Assign a fresh local id (the
			// stored id belonged to a previous session's counter) and re-map
			// its random_id so delete/read-by-random-id still resolve.
			auto flags = MessageFlags(MessageFlag::Local);
			const auto from = out
				? _session->userPeerId()
				: chat->user()->id;
			if (out) {
				flags |= MessageFlag::Outgoing;
			}
			const auto id = _session->data().nextLocalMessageId();
			const auto makeFields = [&] {
				return HistoryItemCommonFields{
					.id = id,
					.flags = flags,
					.from = from,
					.date = date,
				};
			};
			if (kind == qint32(StoredKind::ServiceTtl)) {
				// Prefer a freshly derived text: if the partner user is resolved (the
				// common case -- the dialog shows the real name), derive it now so the
				// notice matches. Only fall back to the persisted text when the partner
				// is still unresolved (then it at least keeps a name captured earlier).
				// The watch re-renders + re-persists if the name resolves later.
				const auto user = chat->user();
				const auto resolved = user && !user->name().isEmpty();
				const auto item = buildTtlServiceMessage(
					chat,
					ttlNoticeSeconds,
					date,
					(out != 0),
					id,
					(resolved || ttlNoticeText.isEmpty())
						? TextWithEntities()
						: TextWithEntities{ ttlNoticeText });
				if (randomId) {
					registerRandomId(chat->id, item->id, randomId);
				}
				_ttlNotices[chat->id][item->id] = ttlNoticeSeconds;
				continue;
			}
			if (kind == qint32(StoredKind::Document)) {
				auto attributes = QVector<MTPDocumentAttribute>();
				attributes.push_back(
					MTP_documentAttributeFilename(MTP_string(filename)));
				// Rebuild the media-shape attribute so video/voice render inline
				// again (photos render inline from mime+filename regardless).
				if (stored == qint32(StoredMedia::Video)
					|| stored == qint32(StoredMedia::Gif)
					|| stored == qint32(StoredMedia::Round)) {
					using Flag = MTPDdocumentAttributeVideo::Flag;
					attributes.push_back(MTP_documentAttributeVideo(
						MTP_flags(stored == qint32(StoredMedia::Round)
							? Flag::f_round_message
							: Flag(0)),
						MTP_double(duration),
						MTP_int(w),
						MTP_int(h),
						MTPint(),
						MTPdouble(),
						MTPstring()));
					if (stored == qint32(StoredMedia::Gif)) {
						attributes.push_back(MTP_documentAttributeAnimated());
					}
				} else if (stored == qint32(StoredMedia::Voice)) {
					using Flag = MTPDdocumentAttributeAudio::Flag;
					attributes.push_back(MTP_documentAttributeAudio(
						MTP_flags(Flag::f_voice),
						MTP_int(duration),
						MTPstring(),
						MTPstring(),
						MTPbytes()));
				} else if (stored == qint32(StoredMedia::Image)) {
					attributes.push_back(MTP_documentAttributeImageSize(
						MTP_int(w),
						MTP_int(h)));
				}
				const auto document = _session->data().processDocument(
					MTP_document(
						MTP_flags(0),
						MTP_long(fileId),
						MTP_long(0),
						MTP_bytes(),
						MTP_int(date),
						MTP_string(mime),
						MTP_long(size),
						MTPVector<MTPPhotoSize>(),
						MTPVector<MTPVideoSize>(),
						MTP_int(0),
						MTP_vector<MTPDocumentAttribute>(attributes)));
				if (!path.isEmpty()) {
					// Lazy: store the encrypted path only; SecretFileLoader
					// decrypts to memory on demand (click/open/autoplay).
					document->setSecretEncryptedLocation(path);
				}
				history->addNewLocalMessage(
					makeFields(),
					document,
					TextWithEntities{ caption });
			} else if (kind == qint32(StoredKind::Photo)) {
				// Eager (photos are small): decrypt the JPEG from disk and rebuild
				// the inline PhotoData; fall back to a placeholder if it's gone.
				// The exact plaintext size is not persisted, but JPEG decoding
				// ignores the <=15 random padding bytes Storage::File appends.
				auto bytesData = path.isEmpty()
					? QByteArray()
					: readSecretFileEncrypted(path, -1);
				auto image = QImage();
				if (!bytesData.isEmpty()
					&& image.loadFromData(bytesData)
					&& !image.isNull()) {
					const auto bytes = Images::FromImageInMemory(
						image,
						"JPG",
						bytesData);
					const auto photo = _session->data().photo(
						fileId,
						uint64(0),
						QByteArray(),
						date,
						0,
						false,
						QByteArray(),
						ImageWithLocation(),
						bytes,
						bytes,
						ImageWithLocation(),
						ImageWithLocation(),
						crl::time(0));
					// In-memory large image renders the photo; carry the encrypted
					// path only so it survives the next restart.
					photo->setSecretEncryptedLocation(path);
					history->addNewLocalMessage(
						makeFields(),
						photo,
						TextWithEntities{ caption });
				} else {
					history->addNewLocalMessage(
						makeFields(),
						TextWithEntities{ caption.isEmpty()
							? u"[photo]"_q
							: caption },
						MTP_messageMediaEmpty());
				}
			} else if (kind == qint32(StoredKind::External)) {
				// Rebuild the remote-location document (sticker / public doc) so the
				// standard loader re-downloads it; mirrors the incoming
				// MediaExternalDocument path (empty file_reference + empty set).
				auto attributes = QVector<MTPDocumentAttribute>();
				attributes.push_back(
					MTP_documentAttributeFilename(MTP_string(filename)));
				if (w > 0 && h > 0) {
					attributes.push_back(
						MTP_documentAttributeImageSize(MTP_int(w), MTP_int(h)));
				}
				if (isVideo) {
					attributes.push_back(MTP_documentAttributeVideo(
						MTP_flags(0),
						MTP_double(duration),
						MTP_int(w),
						MTP_int(h),
						MTPint(),
						MTPdouble(),
						MTPstring()));
				}
				if (isSticker) {
					// Rebuild with the REAL set so processDocument's id-keyed merge
					// doesn't wipe the user's own sticker set (which would break a
					// later re-send: animated sticker -> "File" on the peer).
					const auto set = (setId && setAccessHash)
						? MTP_inputStickerSetID(
							MTP_long(setId),
							MTP_long(setAccessHash))
						: !setShortName.isEmpty()
						? MTP_inputStickerSetShortName(MTP_string(setShortName))
						: MTP_inputStickerSetEmpty();
					attributes.push_back(MTP_documentAttributeSticker(
						MTP_flags(0),
						MTP_string(alt),
						set,
						MTPMaskCoords()));
				}
				// Reuse an already-populated DocumentData if the session knows this
				// id (e.g. the user's own sticker) -- only build when missing, so we
				// never overwrite the genuine doc's richer fields.
				auto document = _session->data().document(fileId);
				if (!document->hasRemoteLocation()) {
					document = _session->data().processDocument(
						MTP_document(
							MTP_flags(0),
							MTP_long(fileId),
							MTP_long(accessHash),
							MTP_bytes(fileReference),
							MTP_int(date),
							MTP_string(mime),
							MTP_long(size),
							MTPVector<MTPPhotoSize>(),
							MTPVector<MTPVideoSize>(),
							MTP_int(dcId),
							MTP_vector<MTPDocumentAttribute>(attributes)));
				}
				history->addNewLocalMessage(
					makeFields(),
					document,
					TextWithEntities{ caption });
			} else {
				history->addNewLocalMessage(
					makeFields(),
					TextWithEntities{ text },
					MTP_messageMediaEmpty());
			}
			if (randomId) {
				registerRandomId(chat->id, id, randomId);
			}
			// Restore the self-destruct timer: a still-armed message (destroyAt in
			// the future -- already-expired ones were skipped above) resumes its
			// countdown from the remaining time; an unread ttl message re-arms its
			// pending timer for the next read.
			if (ttlDestroyAt > 0) {
				if (const auto restored = _session->data().message(
						FullMsgId(chat->id, id))) {
					restored->setSecretChatSelfDestructAt(ttlDestroyAt);
				}
			} else if (ttlSeconds > 0) {
				noteMessageTtl(chat->id, id, ttlSeconds);
			}
			if (out && read) {
				maxReadOutId = id;
			}
		}
		if (history && maxReadOutId) {
			// Rebuild the outbox-read-till so restored outgoing bubbles that the
			// partner had already read show ✓✓ instead of a single ✓.
			history->outboxRead(*maxReadOutId);
		}
		if (chat) {
			ensureInDialogs(chat);
		}
	}
}

MTPInputEncryptedChat EncryptedChats::inputChat(
		not_null<SecretChatData*> chat) const {
	return MTP_inputEncryptedChat(
		MTP_int(chat->secretChatId()),
		MTP_long(chat->accessHash()));
}

SecretChatData *EncryptedChats::applyUpdateChat(const MTPEncryptedChat &chat) {
	return chat.match([&](const MTPDencryptedChat &data) {
		return applyChat(data);
	}, [&](const MTPDencryptedChatRequested &data) {
		return applyRequested(data);
	}, [&](const MTPDencryptedChatWaiting &data) {
		return applyWaiting(data);
	}, [&](const MTPDencryptedChatDiscarded &data) -> SecretChatData* {
		if (const auto chat = _session->data().secretChatLoaded(
				secretChatIdFromWire(data.vid().v))) {
			// The partner closed the chat -> drop the now-dead row from the
			// dialog list. If they chose "also delete for me" the discard carries
			// history_deleted, so wipe our local history too (matches the mobile
			// clients, e.g. SecretChatHelper.java's deleteDialog on the flag).
			clearLocalState(chat, data.is_history_deleted());
			// If we are viewing the chat the partner just closed, leave the now
			// dead conversation (the open section otherwise keeps it on screen --
			// and force-keeps its row in the list -- until we navigate away).
			if (const auto window = _session->tryResolveWindow()) {
				const auto active = window->activeChatCurrent().history();
				if (active && active->peer == chat) {
					window->showBackFromStack();
				}
			}
			return chat;
		}
		return nullptr;
	}, [&](const MTPDencryptedChatEmpty &) -> SecretChatData* {
		return nullptr;
	});
}

SecretChatData *EncryptedChats::applyWaiting(
		const MTPDencryptedChatWaiting &data) {
	const auto chat = _session->data().secretChat(secretChatIdFromWire(data.vid().v));
	// A discard already won for this id: never resurrect / re-add a dead chat.
	if (chat->state() == SecretChatState::Discarded) {
		return chat;
	}
	chat->setAccessHash(data.vaccess_hash().v);
	const auto self = _session->userId().bare;
	chat->setIsCreator(BareId(data.vadmin_id().v) == self);
	chat->setState(SecretChatState::Requested);
	chat->setLoadedStatus(PeerData::LoadedStatus::Full);
	ensureInDialogs(chat);
	return chat;
}

SecretChatData *EncryptedChats::applyRequested(
		const MTPDencryptedChatRequested &data) {
	const auto chat = _session->data().secretChat(secretChatIdFromWire(data.vid().v));
	// A discard already won for this id: never resurrect / re-add a dead chat
	// (and never re-fire accept() for it).
	if (chat->state() == SecretChatState::Discarded) {
		return chat;
	}
	chat->setAccessHash(data.vaccess_hash().v);

	// We are the receiver, the requester (admin) is the other side.
	const auto adminId = UserId(data.vadmin_id().v);
	chat->setIsCreator(false);
	chat->setUser(_session->data().user(adminId));
	chat->setState(SecretChatState::Waiting);
	chat->setLoadedStatus(PeerData::LoadedStatus::Full);

	_pending[chat->secretChatId()] = Pending{
		.gA = bytes::make_vector(bytes::make_span(data.vg_a().v)),
	};
	ensureInDialogs(chat);

	// Auto-accept incoming requests, mirroring the mobile/reference clients
	// (Mercurygram's SecretChatHelper accepts silently with no prompt).
	accept(chat);
	return chat;
}

SecretChatData *EncryptedChats::applyChat(const MTPDencryptedChat &data) {
	const auto chat = _session->data().secretChat(secretChatIdFromWire(data.vid().v));
	// A discard already won for this id (e.g. an in-flight acceptEncryption
	// .done racing the discard update): never resurrect / re-add a dead chat.
	if (chat->state() == SecretChatState::Discarded) {
		return chat;
	}
	chat->setAccessHash(data.vaccess_hash().v);

	const auto self = _session->userId().bare;
	const auto adminId = BareId(data.vadmin_id().v);
	const auto participantId = BareId(data.vparticipant_id().v);
	const auto amCreator = (adminId == self);
	chat->setIsCreator(amCreator);
	const auto otherId = UserId(amCreator ? participantId : adminId);
	chat->setUser(_session->data().user(otherId));
	chat->setLoadedStatus(PeerData::LoadedStatus::Full);
	ensureInDialogs(chat);

	// As the requester we now have the other party's g_b -> compute the key.
	if (!chat->hasKey()) {
		const auto i = _pending.find(chat->secretChatId());
		if (i != _pending.end() && !i->second.randomPower.empty()) {
			const auto computed = MTP::SecretChat::ComputeKey(
				bytes::make_span(data.vg_a_or_b().v),
				i->second.randomPower,
				_dhConfig.p);
			if (computed
				&& computed->fingerprint == uint64(data.vkey_fingerprint().v)) {
				chat->setKey(computed->key, computed->fingerprint);
				chat->setState(SecretChatState::Ready);
				_pending.erase(i);
				sendNotifyLayer(chat);
				flushPendingSends(chat);
				writeLocal();
				DEBUG_LOG(("Secret Chat: chat %1 Ready, fingerprint %2."
					).arg(chat->secretChatId()
					).arg(QString::number(computed->fingerprint, 16)));
			} else {
				LOG(("Secret Chat Error: Key fingerprint mismatch."));
				discard(chat, false);
			}
		}
	}
	return chat;
}

void EncryptedChats::sendText(
		not_null<SecretChatData*> chat,
		const QString &text) {
	if (queuePendingSend(chat, [=] { sendText(chat, text); })) {
		return;
	}
	const auto randomId = base::RandomValue<uint64>();
	auto randomBytes = bytes::vector(16);
	bytes::set_random(randomBytes);

	using MessageFlags = decrypted::MTPDdecryptedMessage::Flags;
	const auto message = decrypted::MTP_decryptedMessage(
		MTP_flags(MessageFlags()),
		MTP_long(randomId),
		MTP_int(chat->ttl()),
		MTP_string(text),
		decrypted::MTPDecryptedMessageMedia(),
		decrypted::MTPVector<decrypted::MTPMessageEntity>(),
		MTP_string(),
		MTP_long(0),
		MTP_long(0));
	const auto outSeqNo = chat->nextOutSeqNo();
	const auto layer = decrypted::MTP_decryptedMessageLayer(
		MTP_bytes(randomBytes),
		MTP_int(decrypted::details::kCurrentLayer),
		MTP_int(chat->currentInSeqNo()),
		MTP_int(outSeqNo),
		message);

	// Serialize the *boxed* layer: the decryptedMessageLayer constructor id
	// must be on the wire, otherwise the receiving client cannot parse it.
	const auto serialized = MTP::SecretChat::SerializeObject(
		decrypted::MTPDecryptedMessageLayer(layer));
	// Keep the payload so we can resend it verbatim if the peer reports a gap.
	rememberSentLayer(
		chat->secretChatId(),
		outSeqNo,
		serialized,
		randomId,
		/*isService=*/false);
	const auto encrypted = MTP::SecretChat::Encrypt(
		serialized,
		chat->key(),
		chat->keyFingerprint(),
		chat->amCreator());

	const auto date = base::unixtime::now();
	const auto requestId = _mtp.request(MTPmessages_SendEncrypted(
		MTP_flags(0),
		inputChat(chat),
		MTP_long(randomId),
		MTP_bytes(encrypted)
	)).done([=](const MTPmessages_SentEncryptedMessage &result) {
		DEBUG_LOG(("Secret Chat: sent message to chat %1, out_seq raw %2."
			).arg(chat->secretChatId()).arg(chat->rawOutSeqNo()));
	}).fail([=](const MTP::Error &error) {
		LOG(("Secret Chat Error: sendEncrypted failed: %1"
			).arg(error.type()));
	}).afterRequest(sendAfter(chat->secretChatId())).send();
	setSendAfter(chat->secretChatId(), requestId);

	addDecryptedMessage(chat, text.toUtf8(), date, true, randomId, chat->ttl());

	// out_seq_no advanced; persist so ordering survives a restart.
	writeLocal();

	chat->countKeyUseOut();
	maybeStartRekey(chat);
}

void EncryptedChats::sendFile(
		not_null<SecretChatData*> chat,
		FullMsgId itemId,
		const std::shared_ptr<FilePrepareResult> &file) {
	if (queuePendingSend(chat, [=] { sendFile(chat, itemId, file); })) {
		return;
	}

	// Reading the source (up to hundreds of MB), AES-encrypting it, and writing
	// the local plaintext copy are all heavy and used to run on the main thread,
	// hitching the UI at send start. Do them on a worker thread, then resume on
	// the main thread to touch the data model (bubble, uploader, persistence).
	const auto session = _session;
	const auto secretChatId = chat->secretChatId();
	const auto historyPeerId = chat->id;
	const auto fileId = base::RandomValue<uint64>();
	const auto randomId = base::RandomValue<uint64>();
	const auto asPhotoRequested = (file->type == SendMediaType::Photo);

	// A video attached as a file carries a documentAttributeVideo. Mirror the
	// official mobile clients (SendMessagesHelper type==3): a regular video is
	// sent as decryptedMessageMediaVideo (inline playable) while an animated GIF
	// or round video goes as a document -> detect those here and skip the video
	// path for them (GIFs use the asAnimation document path; round videos fall
	// back to a plain document, which is acceptable -- niche in a secret chat).
	auto asVideo = false;
	auto videoWidth = 0;
	auto videoHeight = 0;
	auto videoDuration = 0;
	auto asRoundVideo = false;
	auto asVoice = false;
	auto voiceDuration = 0;
	if (!asPhotoRequested) {
		auto isVideo = false;
		auto isAnimated = false;
		auto isRound = false;
		file->document.match([&](const MTPDdocument &d) {
			for (const auto &attr : d.vattributes().v) {
				attr.match([&](const MTPDdocumentAttributeVideo &v) {
					isVideo = true;
					videoWidth = v.vw().v;
					videoHeight = v.vh().v;
					videoDuration = int(v.vduration().v);
					isRound = v.is_round_message();
				}, [&](const MTPDdocumentAttributeAnimated &) {
					isAnimated = true;
				}, [&](const MTPDdocumentAttributeAudio &a) {
					// A voice note -> carry the voice attribute so the peer
					// renders a voice bubble, not an "audio_*.ogg" file row.
					if (a.is_voice()) {
						asVoice = true;
						voiceDuration = a.vduration().v;
					}
				}, [](const auto &) {});
			}
		}, [](const auto &) {});
		asVideo = isVideo && !isAnimated && !isRound;
		// A round video note travels as a document carrying documentAttributeVideo
		// with the round_message flag (mirrors mobile: isRoundVideoDocument path).
		asRoundVideo = isVideo && !isAnimated && isRound;
	}
	// Build a small unencrypted preview thumb (<=90px JPEG) from the poster frame
	// so the peer's video bubble shows a poster immediately, like mobile does.
	auto videoThumb = PreparedThumb();
	if ((asVideo || asRoundVideo) && !file->thumb.isNull()) {
		videoThumb = MakePhotoThumb(file->thumb);
	}

	// Attach the upload radial to the document bubble right away (the next main
	// tick, once SendConfirmedFile has created it) so it does not flash the
	// default download arrow during the off-thread encrypt. The provisional size
	// only matters once progress arrives; the async resume below replaces this
	// with an accurate UploadState(cipherSize).
	if (!asPhotoRequested) {
		const auto provisionalSize = std::max<int64>(file->filesize, 1);
		crl::on_main(session, [=] {
			const auto item = session->data().message(itemId);
			const auto media = item ? item->media() : nullptr;
			const auto document = media ? media->document() : nullptr;
			if (document && !document->uploading()) {
				document->uploadingData
					= std::make_unique<Data::UploadState>(provisionalSize);
				session->data().requestItemRepaint(item);
			}
		});
	}

	crl::async([=, this]() mutable {
		// `fileparts` holds the prepared upload payload and MUST win when present:
		// for a photo it is the *downscaled* JPEG, while `filepath`/`content`
		// still point at the full-resolution original (uploading that bloats the
		// transfer and overran the small-file Secure uploader). Documents leave
		// `fileparts` empty, so they fall through to content / the file on disk.
		auto plain = QByteArray();
		for (const auto &part : file->fileparts) {
			plain.append(part);
		}
		if (plain.isEmpty()) {
			plain = file->content;
		}
		if (plain.isEmpty() && !file->filepath.isEmpty()) {
			auto f = QFile(file->filepath);
			if (f.open(QIODevice::ReadOnly)) {
				plain = f.readAll();
			}
		}
		if (plain.isEmpty()) {
			LOG(("Secret Chat Error: empty file, nothing to send."));
			return;
		}

		// An image picked as a photo is sent as decryptedMessageMediaPhoto so the
		// peer renders it inline. Decode the JPEG for its dimensions + a preview
		// thumb (kept for building the local PhotoData on the main thread).
		auto asPhoto = asPhotoRequested;
		auto photoWidth = 0;
		auto photoHeight = 0;
		auto thumb = PreparedThumb();
		auto image = QImage();
		if (asPhoto) {
			image = QImage::fromData(plain);
			if (image.isNull()) {
				// Not a decodable image -> fall back to a plain document send.
				asPhoto = false;
			} else {
				photoWidth = image.width();
				photoHeight = image.height();
				thumb = MakePhotoThumb(image);
			}
		}

		const auto encrypted = MTP::SecretChat::EncryptFileContent(
			bytes::make_span(plain));

		// Build a fresh prepared file so the uploader does not also try the
		// normal send.
		auto prepared = MakePreparedFile(FilePrepareDescriptor{
			.id = fileId,
			.type = SendMediaType::Secure,
		});
		auto ciphertext = BytesToQ(encrypted.bytes);
		// The Secure uploader path normally slices `fileparts` and sends them via
		// upload.saveFilePart (small files only). For a big ciphertext hand the
		// bytes to the doc-part machinery instead (content + filesize -> the
		// uploader uses upload.saveBigFilePart), referenced with
		// inputEncryptedFileBigUploaded.
		const auto bigFile = (int64(ciphertext.size())
			> Storage::kUseBigFilesFrom);
		DEBUG_LOG(("Secret Chat: sendFile plain %1 cipher %2 big %3."
			).arg(plain.size()).arg(ciphertext.size()).arg(bigFile ? 1 : 0));
		if (bigFile) {
			prepared->content = ciphertext;
			prepared->filesize = ciphertext.size();
			prepared->partssize = ciphertext.size();
		} else {
			prepared->setFileData(ciphertext);
		}

		// Keep a local (ENCRYPTED) copy of the content so a restored outgoing
		// bubble can still open the file even if the user moves or deletes the
		// source. Never written as plaintext.
		auto savedPath = QString();
		{
			auto name = file->filename.isEmpty()
				? (u"file_"_q + QString::number(fileId))
				: file->filename;
			name.replace('/', '_').replace('\\', '_');
			const auto dir = cWorkingDir() + u"tdata/secret_files/"_q;
			QDir().mkpath(dir);
			const auto path = dir + QString::number(fileId) + '_' + name;
			if (writeSecretFileEncrypted(path, plain)) {
				savedPath = path;
			}
		}

		const auto md5 = bigFile
			? QString()
			: QString::fromLatin1(prepared->filemd5);
		const auto mime = file->filemime;
		const auto filename = file->filename;
		const auto caption = file->caption.text;
		const auto cipherSize = int64(ciphertext.size());
		const auto plainSize = int64(plain.size());
		// Extract only the small key material so the resume lambda does NOT
		// capture `encrypted` (whose .bytes holds the full ciphertext -> a costly
		// deep copy). The ciphertext already lives in `prepared`.
		const auto key = encrypted.key;
		const auto iv = encrypted.iv;
		const auto keyFingerprint = encrypted.keyFingerprint;

		crl::on_main(session, [=]() mutable {
			const auto chat = session->data().secretChatLoaded(
				secretChatIdFromWire(secretChatId));
			if (!chat || !chat->hasKey()) {
				return;
			}
			_outgoingFiles[itemId] = OutgoingFile{
				.chatId = secretChatId,
				.key = key,
				.iv = iv,
				.keyFingerprint = keyFingerprint,
				.md5 = md5,
				.randomId = randomId,
				.mime = mime,
				.filename = filename,
				.size = plainSize,
				.caption = caption,
				.asPhoto = asPhoto,
				.width = (asVideo || asRoundVideo) ? videoWidth : photoWidth,
				.height = (asVideo || asRoundVideo) ? videoHeight : photoHeight,
				.thumbWidth = (asVideo || asRoundVideo)
					? videoThumb.width
					: thumb.width,
				.thumbHeight = (asVideo || asRoundVideo)
					? videoThumb.height
					: thumb.height,
				.thumb = (asVideo || asRoundVideo)
					? videoThumb.bytes
					: thumb.bytes,
				.asVideo = asVideo,
				.asRoundVideo = asRoundVideo,
				.asVoice = asVoice,
				.duration = asVoice ? voiceDuration : videoDuration,
				.bigFile = bigFile,
				.cipherSize = cipherSize,
			};

			if (asPhoto) {
				// SendConfirmedFile skips its (black + spinner) photo bubble for
				// secret chats; build our own local PhotoData so it renders inline.
				if (!image.isNull()) {
					const auto bytes = Images::FromImageInMemory(
						image,
						"JPG",
						plain);
					const auto photo = session->data().photo(
						fileId,
						uint64(0), // access_hash -- local
						QByteArray(), // file_reference
						base::unixtime::now(),
						0, // dc_id
						false, // has_stickers
						QByteArray(), // inline thumbnail
						ImageWithLocation(), // small
						bytes, // thumbnail
						bytes, // large
						ImageWithLocation(), // video small
						ImageWithLocation(), // video large
						crl::time(0));
					// Large image is an in-memory copy of the plaintext; renders
					// inline/fullscreen/save from RAM. No plaintext file location;
					// carry the encrypted path only for persistence + reload.
					if (!savedPath.isEmpty()) {
						photo->setSecretEncryptedLocation(savedPath);
					}
					ensureInDialogs(chat);
					const auto history = session->data().history(historyPeerId);
					history->addNewLocalMessage({
						.id = itemId.msg,
						.flags = MessageFlags(MessageFlag::Local
							| MessageFlag::Outgoing),
						.from = session->userPeerId(),
						.date = base::unixtime::now(),
					}, photo, TextWithEntities{ caption });
				}
			} else if (!savedPath.isEmpty()) {
				// Document bubble (created by SendConfirmedFile): show an upload
				// radial while the ciphertext uploads; the local copy is linked
				// only on completion (fileUploadDone) -> openable + persisted.
				_outgoingFiles[itemId].localPath = savedPath;
				const auto item = session->data().message(itemId);
				const auto media = item ? item->media() : nullptr;
				const auto document = media ? media->document() : nullptr;
				if (document) {
					document->uploadingData
						= std::make_unique<Data::UploadState>(cipherSize);
					session->data().requestItemRepaint(item);
				}
			}

			registerRandomId(historyPeerId, itemId.msg, randomId);
			noteMessageTtl(historyPeerId, itemId.msg, chat->ttl());
			writeMessagesLocal();

			ensureUploadSubscribed();
			session->uploader().upload(itemId, prepared);
		});
	});
}

void EncryptedChats::sendExistingDocument(
		not_null<SecretChatData*> chat,
		not_null<DocumentData*> document,
		const QString &caption,
		bool afterSetRefetch) {
	DEBUG_LOG(("Secret Chat: sendExistingDocument id %1 mime %2 sticker %3."
		).arg(document->id
		).arg(document->mimeString()
		).arg(document->sticker() ? 1 : 0));
	if (queuePendingSend(chat, [=] {
		sendExistingDocument(chat, document, caption, afterSetRefetch);
	})) {
		return;
	}
	// We can only reference a document that actually lives on the server.
	if (!document->hasRemoteLocation()) {
		LOG(("Secret Chat Error: sticker has no remote location, not sent."));
		return;
	}

	// The secret layer references a sticker set only by short name, and the peer
	// needs a resolvable set to fetch the sticker -- external documents carry no
	// file_reference, so an empty set leaves the peer unable to download the file
	// and the sticker shows up empty. Resolve the short name locally; if the set
	// is not loaded, fetch it first and send from the callback (mirrors the
	// Android client's delayed-message path).
	const auto sticker = document->sticker();
	auto shortName = QString();
	auto setId = uint64(0);
	auto setAccessHash = uint64(0);
	if (sticker) {
		shortName = sticker->set.shortName;
		setId = sticker->set.id;
		setAccessHash = sticker->set.accessHash;
		// The document's own set identifier is frequently empty: a sticker that
		// arrived inside a set response carries inputStickerSetEmpty per-document,
		// and recent/faved stickers keep no set on the doc at all. Locate the
		// loaded set that actually CONTAINS this document and read its (resolvable)
		// short name from there. A loaded set always has a short name unless it is
		// a special set (recent/faved) -> those are skipped, so we land on the real
		// originating set.
		if (shortName.isEmpty()) {
			const auto &sets = _session->data().stickers().sets();
			if (setId) {
				const auto it = sets.find(setId);
				if (it != sets.end()) {
					shortName = it->second->shortName;
				}
			}
			if (shortName.isEmpty()) {
				for (const auto &[id, set] : sets) {
					if (!set->shortName.isEmpty()
						&& set->stickers.contains(document.get())) {
						shortName = set->shortName;
						setId = set->id;
						setAccessHash = set->accessHash;
						break;
					}
				}
			}
		}
	}
	if (!sticker) {
		// A non-sticker document (GIF / animation / any existing server doc):
		// the external-document reference carries no file_reference, so the peer
		// cannot fetch it (it would render a stuck thumbnail). The official mobile
		// clients upload these as an encrypted document -- mirror that: download
		// the bytes and re-upload them encrypted, preserving the media attributes.
		uploadExistingDocument(chat, document, caption);
		return;
	}
	if (!shortName.isEmpty()) {
		// A sticker whose set short name is resolved: reference the public sticker
		// document directly (stickers are fetchable without a file_reference).
		sendExternalDocument(chat, document, caption, shortName);
		return;
	}
	if (!setId || !setAccessHash) {
		// No usable set reference. The desktop cache stores Recent/Faved stickers
		// with an empty set (serialize_document forces StickerSetTypeEmpty for
		// special sets), so a sticker picked from there loses its originating set.
		// Re-fetch recent + faved from the server once (their documents DO carry
		// the real inputStickerSetID) and retry; only then fall back to uploading
		// the file as an encrypted document.
		if (!afterSetRefetch) {
			refetchRecentAndFavedSets([=] {
				sendExistingDocument(chat, document, caption, true);
			});
			return;
		}
		sendStickerAsDocument(chat, document, caption);
		return;
	}
	_mtp.request(MTPmessages_GetStickerSet(
		MTP_inputStickerSetID(MTP_long(setId), MTP_long(setAccessHash)),
		MTP_int(0) // hash
	)).done([=](const MTPmessages_StickerSet &result) {
		auto name = QString();
		result.match([&](const MTPDmessages_stickerSet &data) {
			_session->data().stickers().feedSetFull(data);
			name = qs(data.vset().data().vshort_name());
		}, [](const MTPDmessages_stickerSetNotModified &) {
		});
		if (name.isEmpty()) {
			sendStickerAsDocument(chat, document, caption);
		} else {
			sendExternalDocument(chat, document, caption, name);
		}
	}).fail([=](const MTP::Error &error) {
		// The set is gone / inaccessible -> upload the actual file instead so
		// the sticker still arrives.
		LOG(("Secret Chat: getStickerSet failed (%1), uploading sticker file."
			).arg(error.type()));
		sendStickerAsDocument(chat, document, caption);
	}).send();
}

void EncryptedChats::refetchRecentAndFavedSets(Fn<void()> then) {
	// Fire both fetches with hash 0 (force a full reload) and feed the results
	// through the same path the app uses, so the shared DocumentData objects get
	// their real sticker set populated. Run `then` once both finish (success or
	// fail) so resolution proceeds regardless.
	const auto left = std::make_shared<int>(2);
	const auto done = [=] {
		if (--*left == 0) {
			then();
		}
	};
	_mtp.request(MTPmessages_GetRecentStickers(
		MTP_flags(0),
		MTP_long(0)
	)).done([=](const MTPmessages_RecentStickers &result) {
		result.match([&](const MTPDmessages_recentStickers &data) {
			_session->data().stickers().specialSetReceived(
				Data::Stickers::CloudRecentSetId,
				tr::lng_recent_stickers(tr::now),
				data.vstickers().v,
				data.vhash().v,
				data.vpacks().v,
				data.vdates().v);
		}, [](const MTPDmessages_recentStickersNotModified &) {
		});
		done();
	}).fail([=] {
		done();
	}).send();
	_mtp.request(MTPmessages_GetFavedStickers(
		MTP_long(0)
	)).done([=](const MTPmessages_FavedStickers &result) {
		result.match([&](const MTPDmessages_favedStickers &data) {
			_session->data().stickers().specialSetReceived(
				Data::Stickers::FavedSetId,
				Lang::Hard::FavedSetTitle(),
				data.vstickers().v,
				data.vhash().v,
				data.vpacks().v);
		}, [](const MTPDmessages_favedStickersNotModified &) {
		});
		done();
	}).fail([=] {
		done();
	}).send();
}

void EncryptedChats::sendExternalDocument(
		not_null<SecretChatData*> chat,
		not_null<DocumentData*> document,
		const QString &caption,
		const QString &shortName) {
	if (!chat->hasKey() || chat->state() != SecretChatState::Ready) {
		return;
	}

	// Build the decrypted document attributes from what we know locally. The
	// peer needs an image size to lay out the bubble (without it a sticker falls
	// back to a generic file row), plus the sticker marker carrying the emoji +
	// resolved set short name so it renders -- and downloads -- as a sticker.
	const auto sticker = document->sticker();
	auto width = document->dimensions.width();
	auto height = document->dimensions.height();
	if ((width <= 0 || height <= 0) && sticker) {
		// A not-yet-loaded sticker may have no cached dimensions; stickers are
		// authored on a 512px canvas, so fall back to that.
		width = height = 512;
	}
	auto attributes = QVector<decrypted::MTPDocumentAttribute>();
	if (width > 0 && height > 0) {
		attributes.push_back(decrypted::MTP_documentAttributeImageSize(
			MTP_int(width),
			MTP_int(height)));
	}
	if (sticker) {
		const auto set = shortName.isEmpty()
			? decrypted::MTP_inputStickerSetEmpty()
			: decrypted::MTP_inputStickerSetShortName(MTP_string(shortName));
		attributes.push_back(decrypted::MTP_documentAttributeSticker(
			MTP_string(sticker->alt),
			set));
		DEBUG_LOG(("Secret Chat: sticker set shortName '%1' (id %2)."
			).arg(shortName).arg(sticker->set.id));
	}
	if (document->isAnimation()) {
		attributes.push_back(decrypted::MTP_documentAttributeAnimated());
	}
	DEBUG_LOG(("Secret Chat: send external doc id %1 dc %2 mime %3 size %4 "
		"sticker %5 dims %6x%7 attrs %8."
		).arg(document->id
		).arg(document->getDC()
		).arg(document->mimeString()
		).arg(document->size
		).arg(sticker ? 1 : 0
		).arg(width
		).arg(height
		).arg(attributes.size()));

	const auto media = decrypted::MTP_decryptedMessageMediaExternalDocument(
		MTP_long(document->id),
		MTP_long(document->getAccessHash()),
		MTP_int(document->date),
		MTP_string(document->mimeString()),
		MTP_int(int32(document->size)),
		ExternalDocumentThumb(document), // non-empty thumb (sticker recognition)
		MTP_int(document->getDC()),
		MTP_vector<decrypted::MTPDocumentAttribute>(attributes));

	const auto randomId = base::RandomValue<uint64>();
	auto randomBytes = bytes::vector(16);
	bytes::set_random(randomBytes);

	const auto flags = decrypted::MTPDdecryptedMessage::Flags()
		| decrypted::MTPDdecryptedMessage::Flag::f_media;
	const auto message = decrypted::MTP_decryptedMessage(
		MTP_flags(flags),
		MTP_long(randomId),
		MTP_int(chat->ttl()),
		MTP_string(caption),
		media,
		decrypted::MTPVector<decrypted::MTPMessageEntity>(),
		MTP_string(),
		MTP_long(0),
		MTP_long(0));
	const auto outSeqNo = chat->nextOutSeqNo();
	const auto layer = decrypted::MTP_decryptedMessageLayer(
		MTP_bytes(randomBytes),
		MTP_int(decrypted::details::kCurrentLayer),
		MTP_int(chat->currentInSeqNo()),
		MTP_int(outSeqNo),
		message);

	const auto serialized = MTP::SecretChat::SerializeObject(
		decrypted::MTPDecryptedMessageLayer(layer));
	// The external-document layer is self-contained (it references the file on
	// the server by id/access_hash/dc), so it can be resent from the cache.
	rememberSentLayer(
		chat->secretChatId(),
		outSeqNo,
		serialized,
		randomId,
		/*isService=*/false);
	const auto encrypted = MTP::SecretChat::Encrypt(
		serialized,
		chat->key(),
		chat->keyFingerprint(),
		chat->amCreator());

	// An external document carries no encrypted blob, so it rides the plain
	// messages.sendEncrypted transport (not sendEncryptedFile).
	const auto requestId = _mtp.request(MTPmessages_SendEncrypted(
		MTP_flags(0),
		inputChat(chat),
		MTP_long(randomId),
		MTP_bytes(encrypted)
	)).done([=](const MTPmessages_SentEncryptedMessage &result) {
		DEBUG_LOG(("Secret Chat: sent sticker to chat %1, out_seq raw %2."
			).arg(chat->secretChatId()).arg(chat->rawOutSeqNo()));
	}).fail([=](const MTP::Error &error) {
		LOG(("Secret Chat Error: sendEncrypted (sticker) failed: %1"
			).arg(error.type()));
	}).afterRequest(sendAfter(chat->secretChatId())).send();
	setSendAfter(chat->secretChatId(), requestId);

	// Optimistic local bubble (mirrors the incoming external-document path).
	ensureInDialogs(chat);
	const auto history = _session->data().history(chat->id);
	const auto item = history->addNewLocalMessage({
		.id = _session->data().nextLocalMessageId(),
		.flags = MessageFlags(MessageFlag::Local | MessageFlag::Outgoing),
		.from = _session->userPeerId(),
		.date = base::unixtime::now(),
	}, document, TextWithEntities{ caption });
	registerRandomId(chat->id, item->id, randomId);
	noteMessageTtl(chat->id, item->id, chat->ttl());
	writeMessagesLocal();

	// out_seq_no advanced; persist so ordering survives a restart.
	writeLocal();

	chat->countKeyUseOut();
	maybeStartRekey(chat);
}

void EncryptedChats::sendStickerAsDocument(
		not_null<SecretChatData*> chat,
		not_null<DocumentData*> document,
		const QString &caption) {
	DEBUG_LOG(("Secret Chat: sendStickerAsDocument id %1 (set unresolved -> "
		"upload; animated renders as File on the peer).").arg(document->id));
	if (!chat->hasKey() || chat->state() != SecretChatState::Ready) {
		return;
	}
	const auto view = document->createMediaView();
	view->checkStickerLarge(); // ensure the sticker file is loading

	// Show the bubble right away (with the sticker's loading state) so a burst of
	// sends appears on click instead of all at once once the bytes resolve.
	const auto itemId = addLocalDocumentBubble(chat, document, caption);

	// Keep the media view (and thus its loaded bytes) alive across the wait.
	const auto chatId = chat->secretChatId();
	const auto trySend = [=]() -> bool {
		if (!view->loaded()) {
			return false;
		}
		const auto plain = view->bytes();
		if (plain.isEmpty()) {
			return false;
		}
		if (const auto resolved = _session->data().secretChatLoaded(
				secretChatIdFromWire(chatId))) {
			uploadStickerDocument(resolved, document, plain, caption, itemId);
		}
		return true;
	};
	sendWhenDownloaded(trySend);
}

FullMsgId EncryptedChats::addLocalDocumentBubble(
		not_null<SecretChatData*> chat,
		not_null<DocumentData*> document,
		const QString &caption) {
	// Local bubble: the original document, so it renders (and plays) inline on
	// desktop straight away (and, for a re-uploaded existing document, shows its
	// loading state while we fetch the bytes).
	ensureInDialogs(chat);
	const auto history = _session->data().history(chat->id);
	const auto item = history->addNewLocalMessage({
		.id = _session->data().nextLocalMessageId(),
		.flags = MessageFlags(MessageFlag::Local | MessageFlag::Outgoing),
		.from = _session->userPeerId(),
		.date = base::unixtime::now(),
	}, document, TextWithEntities{ caption });
	return item->fullId();
}

void EncryptedChats::sendWhenDownloaded(Fn<bool()> trySend) {
	if (trySend()) {
		return;
	}
	// The content is not in memory yet -> retry once a load finishes.
	const auto lifetime = std::make_shared<rpl::lifetime>();
	_session->downloaderTaskFinished(
	) | rpl::on_next([=] {
		if (trySend()) {
			lifetime->destroy();
		}
	}, *lifetime);
	// The subscription self-owns via the captured shared_ptr, so if the load
	// never completes (permanent failure, logout) it would leak forever along
	// with the captured media view and document. Bound it to our lifetime so it
	// is torn down at the latest when this object dies.
	_lifetime.add([weak = std::weak_ptr<rpl::lifetime>(lifetime)] {
		if (const auto strong = weak.lock()) {
			strong->destroy();
		}
	});
}

void EncryptedChats::uploadEncryptedDocument(
		not_null<SecretChatData*> chat,
		not_null<DocumentData*> document,
		const QByteArray &plain,
		const QString &caption,
		int width,
		int height,
		Fn<void(OutgoingFile&)> applyKind,
		FullMsgId existing) {
	if (!chat->hasKey() || chat->state() != SecretChatState::Ready) {
		return;
	}
	const auto encrypted = MTP::SecretChat::EncryptFileContent(
		bytes::make_span(plain));
	const auto fileId = base::RandomValue<uint64>();
	auto prepared = MakePreparedFile(FilePrepareDescriptor{
		.id = fileId,
		.type = SendMediaType::Secure,
	});
	auto ciphertext = BytesToQ(encrypted.bytes);
	// Mirror sendFile(): a ciphertext above the big-file threshold must go through
	// the doc-part machinery (saveBigFilePart -> inputEncryptedFileBigUploaded),
	// not the small-file fileparts path (saveFilePart, small files only), or the
	// server rejects a large re-uploaded document / GIF / animation.
	const auto bigFile = (int64(ciphertext.size()) > Storage::kUseBigFilesFrom);
	if (bigFile) {
		prepared->content = ciphertext;
		prepared->filesize = ciphertext.size();
		prepared->partssize = ciphertext.size();
	} else {
		prepared->setFileData(ciphertext);
	}

	// Reuse the bubble added up front (addLocalDocumentBubble) when sending an
	// existing document, otherwise add it now.
	const auto itemId = existing
		? existing
		: addLocalDocumentBubble(chat, document, caption);

	const auto randomId = base::RandomValue<uint64>();
	auto file = OutgoingFile{
		.chatId = chat->secretChatId(),
		.key = encrypted.key,
		.iv = encrypted.iv,
		.keyFingerprint = encrypted.keyFingerprint,
		.md5 = bigFile ? QString() : QString::fromLatin1(prepared->filemd5),
		.randomId = randomId,
		.mime = document->mimeString(),
		.filename = document->filename(),
		.size = int64(plain.size()),
		.caption = caption,
		.width = width,
		.height = height,
		.bigFile = bigFile,
	};
	applyKind(file);
	_outgoingFiles[itemId] = std::move(file);
	registerRandomId(chat->id, itemId.msg, randomId);
	noteMessageTtl(chat->id, itemId.msg, chat->ttl());
	writeMessagesLocal();

	ensureUploadSubscribed();
	_session->uploader().upload(itemId, prepared);
}

void EncryptedChats::ensureUploadSubscribed() {
	if (_uploadSubscribed) {
		return;
	}
	_uploadSubscribed = true;
	_session->uploader().secureReady(
	) | rpl::on_next([=](const Storage::UploadSecureDone &done) {
		fileUploadDone(done);
	}, _lifetime);
	_session->uploader().secureFailed(
	) | rpl::on_next([=](FullMsgId failedId) {
		fileUploadFailed(failedId);
	}, _lifetime);
	_session->uploader().secureProgress(
	) | rpl::on_next([=](const Storage::UploadSecureProgress &data) {
		fileUploadProgress(data);
	}, _lifetime);
}

void EncryptedChats::uploadStickerDocument(
		not_null<SecretChatData*> chat,
		not_null<DocumentData*> document,
		const QByteArray &plain,
		const QString &caption,
		FullMsgId existing) {
	const auto sticker = document->sticker();
	auto width = document->dimensions.width();
	auto height = document->dimensions.height();
	if (width <= 0 || height <= 0) {
		width = height = 512;
	}
	uploadEncryptedDocument(
		chat,
		document,
		plain,
		caption,
		width,
		height,
		[&](OutgoingFile &file) {
			file.asSticker = true;
			file.stickerAlt = sticker ? sticker->alt : QString();
		},
		existing);
}

void EncryptedChats::uploadExistingDocument(
		not_null<SecretChatData*> chat,
		not_null<DocumentData*> document,
		const QString &caption) {
	if (!chat->hasKey() || chat->state() != SecretChatState::Ready) {
		return;
	}
	// GIFs/animations live in the streaming cache (and a re-forwarded secret file
	// lives encrypted on disk), so there is no plain file to read until we ask for
	// one. Materialize the content into MEMORY -- an empty toFile loads bytes via
	// the cache/network (or decrypts the secret file via SecretFileLoader) without
	// ever writing plaintext to disk -- then encrypt + upload from RAM. Keep a
	// media view alive so the loaded bytes survive until trySend reads them.
	const auto view = document->createMediaView();
	document->save(Data::FileOrigin(), QString(), LoadFromCloudOrLocal, true);

	// Show the bubble right away (with the document's loading state) so a burst
	// of sends appears on click instead of all at once once the bytes resolve.
	const auto itemId = addLocalDocumentBubble(chat, document, caption);

	const auto chatId = chat->secretChatId();
	const auto trySend = [=]() -> bool {
		const auto media = document->activeMediaView();
		const auto plain = media ? media->bytes() : QByteArray();
		if (plain.isEmpty()) {
			return false;
		}
		if (const auto resolved = _session->data().secretChatLoaded(
				secretChatIdFromWire(chatId))) {
			uploadDocumentFile(resolved, document, plain, caption, itemId);
		}
		return true;
	};
	sendWhenDownloaded(trySend);
}

void EncryptedChats::uploadDocumentFile(
		not_null<SecretChatData*> chat,
		not_null<DocumentData*> document,
		const QByteArray &plain,
		const QString &caption,
		FullMsgId existing) {
	uploadEncryptedDocument(
		chat,
		document,
		plain,
		caption,
		document->dimensions.width(),
		document->dimensions.height(),
		[&](OutgoingFile &file) {
			file.asAnimation = true;
			file.duration = int(document->duration() / 1000);
		},
		existing);
}

void EncryptedChats::fileUploadDone(const Storage::UploadSecureDone &done) {
	const auto i = _outgoingFiles.find(done.fullId);
	if (i == _outgoingFiles.end()) {
		return;
	}
	const auto info = i->second;
	_outgoingFiles.erase(i);

	DEBUG_LOG(("Secret Chat: fileUploadDone fileId %1 partsCount %2 big %3."
		).arg(done.fileId).arg(done.partsCount).arg(info.bigFile ? 1 : 0));

	const auto chat = _session->data().secretChatLoaded(
		secretChatIdFromWire(info.chatId));
	if (!chat || !chat->hasKey()) {
		return;
	}

	const auto keyBytes = MTP_bytes(BytesToQ(info.key));
	const auto ivBytes = MTP_bytes(BytesToQ(info.iv));
	const auto media = [&] {
		if (info.asPhoto) {
			// decryptedMessageMediaPhoto carries an unencrypted preview thumb +
			// full w/h so the peer shows the image inline (the size field is the
			// plaintext size, an int -- photos are well under 2GB).
			return decrypted::MTP_decryptedMessageMediaPhoto(
				MTP_bytes(info.thumb),
				MTP_int(info.thumbWidth),
				MTP_int(info.thumbHeight),
				MTP_int(info.width),
				MTP_int(info.height),
				MTP_int(int32(info.size)),
				keyBytes,
				ivBytes,
				MTP_string(info.caption));
		}
		if (info.asVideo) {
			// decryptedMessageMediaVideo carries an unencrypted preview thumb +
			// duration/w/h so the peer renders an inline playable video instead of
			// a plain file (mirrors the official mobile clients' wire format).
			return decrypted::MTP_decryptedMessageMediaVideo(
				MTP_bytes(info.thumb),
				MTP_int(info.thumbWidth),
				MTP_int(info.thumbHeight),
				MTP_int(info.duration),
				MTP_string(info.mime.isEmpty() ? u"video/mp4"_q : info.mime),
				MTP_int(info.width),
				MTP_int(info.height),
				MTP_int(int32(info.size)),
				keyBytes,
				ivBytes,
				MTP_string(info.caption));
		}
		auto attributes = QVector<decrypted::MTPDocumentAttribute>();
		if (info.asSticker) {
			// Sticker uploaded as an encrypted document (set unresolvable): carry
			// the image size + sticker marker so the peer renders it as a sticker
			// from the file alone. The set is empty, but a static sticker only
			// needs the attribute + mime (image/webp) to be recognized.
			if (info.width > 0 && info.height > 0) {
				attributes.push_back(decrypted::MTP_documentAttributeImageSize(
					MTP_int(info.width),
					MTP_int(info.height)));
			}
			attributes.push_back(decrypted::MTP_documentAttributeSticker(
				MTP_string(info.stickerAlt),
				decrypted::MTP_inputStickerSetEmpty()));
		} else if (info.asAnimation) {
			// A GIF/animation re-uploaded as an encrypted document: the peer needs
			// documentAttributeVideo (for the dimensions) + documentAttributeAnimated
			// (mime video/mp4) to recognize it as an auto-playing GIF rather than a
			// plain file. Mirrors the official mobile clients' wire format.
			if (info.width > 0 && info.height > 0) {
				attributes.push_back(decrypted::MTP_documentAttributeVideo(
					MTP_flags(decrypted::MTPDdocumentAttributeVideo::Flags()),
					MTP_int(info.duration),
					MTP_int(info.width),
					MTP_int(info.height)));
			}
			attributes.push_back(decrypted::MTP_documentAttributeAnimated());
			attributes.push_back(decrypted::MTP_documentAttributeFilename(
				MTP_string(info.filename)));
		} else if (info.asRoundVideo) {
			// A round video note: documentAttributeVideo with the round_message
			// flag so the peer renders a round video bubble (isRoundVideoDocument).
			using Flag = decrypted::MTPDdocumentAttributeVideo::Flag;
			attributes.push_back(decrypted::MTP_documentAttributeVideo(
				MTP_flags(Flag::f_round_message),
				MTP_int(info.duration),
				MTP_int(info.width),
				MTP_int(info.height)));
		} else if (info.asVoice) {
			// A voice note: documentAttributeAudio with the voice flag + duration
			// so the peer renders a voice bubble (mirrors the official clients,
			// which send voice as a document carrying this attribute). title/
			// performer/waveform are cosmetic and left empty.
			using Flag = decrypted::MTPDdocumentAttributeAudio::Flag;
			attributes.push_back(decrypted::MTP_documentAttributeAudio(
				MTP_flags(Flag::f_voice),
				MTP_int(info.duration),
				MTP_string(),
				MTP_string(),
				MTP_bytes()));
		} else {
			attributes.push_back(decrypted::MTP_documentAttributeFilename(
				MTP_string(info.filename)));
		}
		return decrypted::MTP_decryptedMessageMediaDocument(
			MTP_bytes(), // thumb
			MTP_int(0), // thumb_w
			MTP_int(0), // thumb_h
			MTP_string(info.mime),
			MTP_long(info.size),
			keyBytes,
			ivBytes,
			MTP_vector<decrypted::MTPDocumentAttribute>(attributes),
			MTP_string(info.caption));
	}();

	using MessageFlags = decrypted::MTPDdecryptedMessage::Flags;
	auto flags = MessageFlags()
		| decrypted::MTPDdecryptedMessage::Flag::f_media;
	const auto message = decrypted::MTP_decryptedMessage(
		MTP_flags(flags),
		MTP_long(info.randomId),
		MTP_int(chat->ttl()),
		MTP_string(info.caption),
		media,
		decrypted::MTPVector<decrypted::MTPMessageEntity>(),
		MTP_string(),
		MTP_long(0),
		MTP_long(0));

	auto randomBytes = bytes::vector(16);
	bytes::set_random(randomBytes);
	const auto layer = decrypted::MTP_decryptedMessageLayer(
		MTP_bytes(randomBytes),
		MTP_int(decrypted::details::kCurrentLayer),
		MTP_int(chat->currentInSeqNo()),
		MTP_int(chat->nextOutSeqNo()),
		message);

	const auto serialized = MTP::SecretChat::SerializeObject(
		decrypted::MTPDecryptedMessageLayer(layer));
	// NB: a file message is intentionally not cached for resend -- its layer omits
	// the server file reference (carried in the outer inputEncryptedFile), so it
	// could not be reconstructed. A resend request for this seq falls back to a
	// delete tombstone, which keeps the peer's sequence intact (it loses just this
	// one file, instead of discarding the whole chat).
	const auto data = MTP::SecretChat::Encrypt(
		serialized,
		chat->key(),
		chat->keyFingerprint(),
		chat->amCreator());

	// A big file was uploaded via saveBigFilePart and has no md5 checksum, so it
	// is referenced with inputEncryptedFileBigUploaded; small files keep the md5.
	const auto inputFile = info.bigFile
		? MTP_inputEncryptedFileBigUploaded(
			MTP_long(done.fileId),
			MTP_int(done.partsCount),
			MTP_int(info.keyFingerprint))
		: MTP_inputEncryptedFileUploaded(
			MTP_long(done.fileId),
			MTP_int(done.partsCount),
			MTP_string(info.md5),
			MTP_int(info.keyFingerprint));
	const auto requestId = _mtp.request(MTPmessages_SendEncryptedFile(
		MTP_flags(0),
		inputChat(chat),
		MTP_long(info.randomId),
		MTP_bytes(data),
		inputFile
	)).done([=](const MTPmessages_SentEncryptedMessage &result) {
		DEBUG_LOG(("Secret Chat: sent file to chat %1, out_seq raw %2."
			).arg(chat->secretChatId()).arg(chat->rawOutSeqNo()));
	}).fail([=](const MTP::Error &error) {
		LOG(("Secret Chat Error: sendEncryptedFile failed: %1"
			).arg(error.type()));
	}).afterRequest(sendAfter(chat->secretChatId())).send();
	setSendAfter(chat->secretChatId(), requestId);

	// out_seq_no advanced; persist so ordering survives a restart.
	writeLocal();

	// The upload finished and the encrypted file is on its way: drop the bubble's
	// upload radial and link it to the local ENCRYPTED copy so it becomes an
	// openable file (and serializeMessages persists that stable path).
	if (!info.localPath.isEmpty()) {
		const auto item = _session->data().message(done.fullId);
		const auto media = item ? item->media() : nullptr;
		const auto document = media ? media->document() : nullptr;
		if (document) {
			document->uploadingData = nullptr;
			document->setSecretEncryptedLocation(info.localPath);
			_session->data().requestItemRepaint(item);
		}
		writeMessagesLocal();
	}

	if (const auto chat = _session->data().secretChatLoaded(
			secretChatIdFromWire(info.chatId))) {
		chat->countKeyUseOut();
		maybeStartRekey(chat);
	}
}

void EncryptedChats::fileUploadFailed(FullMsgId itemId) {
	const auto i = _outgoingFiles.find(itemId);
	if (i != _outgoingFiles.end()) {
		const auto chatId = i->second.chatId;
		_outgoingFiles.erase(i);
		LOG(("Secret Chat Error: file upload failed for chat %1."
			).arg(chatId));
	}
}

void EncryptedChats::fileUploadProgress(
		const Storage::UploadSecureProgress &data) {
	const auto i = _outgoingFiles.find(data.fullId);
	if (i == _outgoingFiles.end() || i->second.localPath.isEmpty()) {
		return;
	}
	const auto item = _session->data().message(data.fullId);
	const auto media = item ? item->media() : nullptr;
	const auto document = media ? media->document() : nullptr;
	if (!document || !document->uploading()) {
		return;
	}
	document->uploadingData->offset = data.offset;
	_session->data().requestItemRepaint(item);
}

void EncryptedChats::setTyping(
		not_null<SecretChatData*> chat,
		bool typing) {
	_mtp.request(MTPmessages_SetEncryptedTyping(
		inputChat(chat),
		MTP_bool(typing)
	)).send();
}

void EncryptedChats::setSelfDestructTimer(
		not_null<SecretChatData*> chat,
		int seconds) {
	if (!chat->hasKey() || chat->state() != SecretChatState::Ready) {
		return;
	}
	// Notify the partner (a service message), then store the new default locally
	// so our subsequent messages carry this ttl. The action rides the same
	// out_seq_no ordering as content messages via sendServiceAction.
	sendServiceAction(
		chat,
		decrypted::MTP_decryptedMessageActionSetMessageTTL(MTP_int(seconds)));
	chat->setTtl(seconds);
	addTtlChangeNotice(chat, seconds, base::unixtime::now(), true);
	writeLocal();
}

void EncryptedChats::flushHistory(not_null<SecretChatData*> chat) {
	// Mirror the incoming decryptedMessageActionFlushHistory handler: tell the
	// partner to wipe the history too. sendServiceAction is a no-op unless the
	// chat has a key and is Ready, so an early local clear stays local-only.
	sendServiceAction(
		chat,
		decrypted::MTP_decryptedMessageActionFlushHistory());
}

void EncryptedChats::readHistory(
		not_null<SecretChatData*> chat,
		TimeId tillDate) {
	// The read path can re-fire on every repaint; only send the network
	// readEncryptedHistory when the read frontier actually advances. The LOCAL
	// self-destruct arming below must NOT be gated by this dedup: two messages
	// can share a date (same second), and a second readHistory at the same
	// tillDate would otherwise mark them read without arming them -> they would
	// persist as unread-pending and reload after a restart.
	auto &last = _readTillDates[chat->secretChatId()];
	if (tillDate > last) {
		last = tillDate;
		_mtp.request(MTPmessages_ReadEncryptedHistory(
			inputChat(chat),
			MTP_int(tillDate)
		)).send();
	}

	// We just read the incoming messages up to tillDate: start their
	// self-destruct timers (per the secret-chat spec the timer starts on read).
	startSelfDestructTimers(chat, false, tillDate);
}

void EncryptedChats::newMessage(
		const MTPEncryptedMessage &message,
		int32 qts) {
	const auto handle = [&](
			int32 chatId,
			TimeId date,
			const MTPbytes &payload,
			const MTPEncryptedFile *encryptedFile) {
		const auto chat = _session->data().secretChatLoaded(
			secretChatIdFromWire(chatId));
		if (!chat || !chat->hasKey()) {
			LOG(("Secret Chat Error: Message for unknown chat %1."
				).arg(chatId));
			return;
		}
		auto decrypted = MTP::SecretChat::Decrypt(
			bytes::make_span(payload.v),
			chat->key(),
			chat->keyFingerprint(),
			chat->amCreator());
		auto usedNewKey = false;
		if (!decrypted) {
			// The peer may already be encrypting with a rekeyed key whose
			// commitKey hasn't reached us yet. If we are the accepting side and
			// have the pending key, try it -- a successful decrypt means the
			// peer switched, so we commit the rekey now too.
			const auto i = _rekeys.find(chat->secretChatId());
			if (i != _rekeys.end()
				&& i->second.stage == Rekey::Stage::Accepted
				&& i->second.haveNewKey) {
				decrypted = MTP::SecretChat::Decrypt(
					bytes::make_span(payload.v),
					i->second.newKey,
					i->second.newKeyFingerprint,
					chat->amCreator());
				if (decrypted) {
					usedNewKey = true;
					commitNewKey(
						chat,
						i->second.newKey,
						i->second.newKeyFingerprint);
				}
			}
			if (!decrypted) {
				LOG(("Secret Chat Error: Could not decrypt message."));
				return;
			}
		}
		auto layer = decrypted::MTPDecryptedMessageLayer();
		if (!MTP::SecretChat::DeserializeObject(layer, *decrypted)) {
			LOG(("Secret Chat Error: Could not parse decrypted layer."));
			return;
		}
		// Count one decrypted incoming message against the current key and
		// maybe rotate (mirrors SecretChatHelper.java:818,1609). Skip a message
		// that arrived under the just-committed new key -- commitNewKey reset
		// the counters, so it belongs to the fresh key's first window, not the
		// retired one.
		if (!usedNewKey) {
			chat->countKeyUseIn();
			maybeStartRekey(chat);
		}
		layer.match([&](const decrypted::MTPDdecryptedMessageLayer &data) {
			receiveDecryptedLayer(chat, data, date, encryptedFile);
		});
	};
	message.match([&](const MTPDencryptedMessage &data) {
		const auto file = &data.vfile();
		handle(data.vchat_id().v, data.vdate().v, data.vbytes(), file);
	}, [&](const MTPDencryptedMessageService &data) {
		handle(data.vchat_id().v, data.vdate().v, data.vbytes(), nullptr);
	});
	// Advance + persist the qts checkpoint and ACK the server. For the live
	// updateNewEncryptedMessage path `qts` is this update's real qts; for the
	// getDifference catch-up path it is the (older) baseline we replayed from,
	// so setQts no-ops here and differenceDone advances to the final state qts.
	setQts(qts);
}

// Apply one decrypted layer with seq-no reliability (mirrors Android
// SecretChatHelper): drop duplicates/old messages, on a gap ask the peer to
// resend the missing range and buffer this newer message, otherwise accept it,
// advance our in-seq, and replay any buffered messages the gap now unblocks.
void EncryptedChats::receiveDecryptedLayer(
		not_null<SecretChatData*> chat,
		const decrypted::MTPDdecryptedMessageLayer &data,
		TimeId date,
		const MTPEncryptedFile *encryptedFile) {
	const auto remoteOut = data.vout_seq_no().v;
	const auto haveIn = chat->inSeqNo();
	DEBUG_LOG(("Secret Chat: decrypted message for chat %1, "
		"remote out_seq %2, our in_seq %3."
		).arg(chat->secretChatId()).arg(remoteOut).arg(haveIn));
	if (remoteOut <= haveIn) {
		// Already accepted (the server re-delivers until ACKed and the startup
		// catch-up replays from our checkpoint), so ignore the duplicate.
		return;
	} else if (haveIn != remoteOut - 2) {
		// A gap: earlier messages have not arrived. Ask the peer to resend the
		// missing range and hold this one until the gap is filled.
		const auto startSeqNo = haveIn + 2;
		const auto endSeqNo = remoteOut - 2;
		if (endSeqNo - startSeqNo > kMaxResendRange) {
			// The gap is wider than the peer will honour in a single resend (see
			// handleResend, which rejects ranges above kMaxResendRange), so it can
			// never be filled and the stream would stall forever. Discard the chat
			// immediately instead of sending a request the peer will drop.
			LOG(("Secret Chat Error: gap %1..%2 exceeds resend range, "
				"discarding chat %3."
				).arg(startSeqNo).arg(endSeqNo).arg(chat->secretChatId()));
			_holes.remove(chat->secretChatId());
			discard(chat, false);
			return;
		}
		sendResendRequest(chat, startSeqNo, endSeqNo);
		auto &holes = _holes[chat->secretChatId()];
		if (holes.size() >= kMaxHoles) {
			// The stream is unrecoverable; discard the chat, as mobile does.
			LOG(("Secret Chat Error: too many holes (%1), discarding chat %2."
				).arg(holes.size()).arg(chat->secretChatId()));
			_holes.remove(chat->secretChatId());
			discard(chat, false);
			return;
		}
		auto buffered = BufferedMessage();
		buffered.message = data.vmessage();
		buffered.date = date;
		buffered.outSeqNo = remoteOut;
		if (encryptedFile) {
			buffered.file = *encryptedFile;
		}
		holes.push_back(std::move(buffered));
		return;
	}
	chat->setInSeqNo(remoteOut);
	// Remember the peer's ACK of our outgoing messages so handleResend can
	// refuse to re-send anything the peer has already confirmed (mirrors
	// SecretChatHelper.java's chat.in_seq_no = layer.in_seq_no).
	chat->setPeerInSeqNo(data.vin_seq_no().v);
	writeLocal();
	processDecryptedMessage(chat, data.vmessage(), date, encryptedFile);
	drainHoles(chat);
}

void EncryptedChats::processDecryptedMessage(
		not_null<SecretChatData*> chat,
		const decrypted::MTPDecryptedMessage &message,
		TimeId date,
		const MTPEncryptedFile *encryptedFile) {
	message.match([&](
		const decrypted::MTPDdecryptedMessage &fields) {
		// Skip a content message we already have. The server re-delivers
		// encrypted updates until they are ACKed (messages.receivedQueue),
		// and a startup catch-up replays from our checkpoint, so the same
		// message can arrive twice; random_id makes it idempotent.
		const auto incomingRandomId = uint64(fields.vrandom_id().v);
		if (incomingRandomId
				&& findByRandomId(chat->id, incomingRandomId)) {
			return;
		}
		auto text = qs(fields.vmessage());
		auto startedDownload = false;
		if (const auto media = fields.vmedia()) {
			// Document/photo/video/audio all share one download+decrypt
			// path; only key/iv extraction, mime, filename and the rebuilt
			// document attributes differ. startMedia kicks off the download
			// of the encrypted file this message references; on a key
			// fingerprint mismatch / empty file it leaves startedDownload
			// false so the caller shows a placeholder instead.
			const auto startMedia = [&](
					bytes::const_span key,
					bytes::const_span iv,
					int64 plaintextSize,
					const QString &mime,
					const QString &filename,
					QVector<MTPDocumentAttribute> attributes,
					bool asPhoto,
					const QByteArray &thumb = QByteArray(),
					int thumbWidth = 0,
					int thumbHeight = 0) {
				if (!encryptedFile
					|| key.size() != 32
					|| iv.size() != 32) {
					return;
				}
				encryptedFile->match([&](const MTPDencryptedFile &f) {
					auto state = std::make_shared<IncomingFile>();
					bytes::copy(state->key, key);
					bytes::copy(state->iv, iv);
					if (MTP::SecretChat::FileKeyFingerprint(
							state->key,
							state->iv) != f.vkey_fingerprint().v) {
						LOG(("Secret Chat Error: file key "
							"fingerprint mismatch."));
						return;
					}
					if (f.vsize().v <= 0
						|| f.vsize().v > kMaxIncomingFileSize) {
						LOG(("Secret Chat Error: incoming file size "
							"out of bounds: %1").arg(f.vsize().v));
						return;
					}
					state->chatId = chat->secretChatId();
					state->fileId = f.vid().v;
					state->accessHash = f.vaccess_hash().v;
					state->dcId = f.vdc_id().v;
					state->ciphertextSize = f.vsize().v;
					state->plaintextSize = plaintextSize;
					state->mime = mime;
					state->filename = filename;
					state->attributes = std::move(attributes);
					state->asPhoto = asPhoto;
					state->caption = text;
					state->date = date;
					state->randomId = uint64(fields.vrandom_id().v);
					state->ttl = fields.vttl().v;
					state->thumbBytes = thumb;
					state->thumbWidth = thumbWidth;
					state->thumbHeight = thumbHeight;
					// Show the carried preview thumb right away (a bubble
					// up-front) so a large file isn't invisible while it
					// downloads; finishFileDownload then attaches the file.
					createPendingMediaItem(state);
					startFileDownload(state);
					startedDownload = true;
				}, [&](const MTPDencryptedFileEmpty &) {
				});
			};
			const auto randomId = uint64(fields.vrandom_id().v);
			media->match([&](
					const decrypted::MTPDdecryptedMessageMediaDocument &doc) {
				startMedia(
					bytes::make_span(doc.vkey().v),
					bytes::make_span(doc.viv().v),
					doc.vsize().v,
					qs(doc.vmime_type()),
					DocumentName(doc),
					ConvertDecryptedAttributes(doc.vattributes()),
					false,
					BytesToQ(bytes::make_span(doc.vthumb().v)),
					doc.vthumb_w().v,
					doc.vthumb_h().v);
				if (!startedDownload) {
					// Fingerprint mismatch / empty file / bad key-iv:
					// fall back to a visible placeholder rather than a
					// silent empty bubble.
					const auto name = DocumentName(doc);
					const auto info = name.isEmpty()
						? qs(doc.vmime_type())
						: name;
					const auto placeholder = u"[file] "_q
						+ info
						+ u" ("_q
						+ QString::number(doc.vsize().v)
						+ u" bytes)"_q;
					text = text.isEmpty()
						? placeholder
						: (placeholder + u"\n"_q + text);
				}
			}, [&](
					const decrypted::MTPDdecryptedMessageMediaPhoto &photo) {
				// Photos are always JPEG. MediaFile has no isImage->Photo
				// path, so finishFileDownload rebuilds this as a local
				// PhotoData (asPhoto=true) to render inline.
				startMedia(
					bytes::make_span(photo.vkey().v),
					bytes::make_span(photo.viv().v),
					photo.vsize().v,
					u"image/jpeg"_q,
					u"photo_"_q + QString::number(randomId) + u".jpg"_q,
					QVector<MTPDocumentAttribute>(),
					true,
					BytesToQ(bytes::make_span(photo.vthumb().v)),
					photo.vthumb_w().v,
					photo.vthumb_h().v);
			}, [&](
					const decrypted::MTPDdecryptedMessageMediaVideo &video) {
				const auto mime = qs(video.vmime_type());
				auto ext = ExtensionForMime(mime);
				if (ext.isEmpty()) {
					ext = u".mp4"_q;
				}
				auto attributes = QVector<MTPDocumentAttribute>();
				attributes.push_back(MTP_documentAttributeVideo(
					MTP_flags(0),
					MTP_double(video.vduration().v),
					video.vw(),
					video.vh(),
					MTPint(), // preload_prefix_size
					MTPdouble(), // video_start_ts
					MTPstring())); // video_codec
				startMedia(
					bytes::make_span(video.vkey().v),
					bytes::make_span(video.viv().v),
					video.vsize().v,
					mime.isEmpty() ? u"video/mp4"_q : mime,
					u"video_"_q + QString::number(randomId) + ext,
					std::move(attributes),
					false,
					BytesToQ(bytes::make_span(video.vthumb().v)),
					video.vthumb_w().v,
					video.vthumb_h().v);
			}, [&](
					const decrypted::MTPDdecryptedMessageMediaAudio &audio) {
				const auto mime = qs(audio.vmime_type());
				auto ext = ExtensionForMime(mime);
				if (ext.isEmpty()) {
					ext = u".ogg"_q;
				}
				// decryptedMessageMediaAudio is a voice note; mark it voice
				// so it renders as a playable voice message.
				using Flag = MTPDdocumentAttributeAudio::Flag;
				auto attributes = QVector<MTPDocumentAttribute>();
				attributes.push_back(MTP_documentAttributeAudio(
					MTP_flags(Flag::f_voice),
					audio.vduration(),
					MTPstring(), // title
					MTPstring(), // performer
					MTPbytes())); // waveform
				startMedia(
					bytes::make_span(audio.vkey().v),
					bytes::make_span(audio.viv().v),
					audio.vsize().v,
					mime.isEmpty() ? u"audio/ogg"_q : mime,
					u"audio_"_q + QString::number(randomId) + ext,
					std::move(attributes),
					false);
			}, [&](
					const decrypted::MTPDdecryptedMessageMediaExternalDocument &ext) {
				// Stickers / public documents: a reference to an existing,
				// NON-encrypted file on the server (id/access_hash/dc). Build
				// a normal document with that remote location so the standard
				// loader downloads it (sticker attr -> renders as a sticker).
				auto attributes = ConvertDecryptedAttributes(
					ext.vattributes());
				const auto document = _session->data().processDocument(
					MTP_document(
						MTP_flags(0),
						ext.vid(),
						ext.vaccess_hash(),
						MTP_bytes(), // file_reference (stickers need none)
						ext.vdate(),
						ext.vmime_type(),
						MTP_long(ext.vsize().v),
						MTPVector<MTPPhotoSize>(),
						MTPVector<MTPVideoSize>(),
						ext.vdc_id(),
						MTP_vector<MTPDocumentAttribute>(attributes)));
				ensureInDialogs(chat);
				const auto history = _session->data().history(chat->id);
				const auto item = history->addNewLocalMessage({
					.id = _session->data().nextLocalMessageId(),
					.flags = MessageFlags(MessageFlag::Local
						| MessageFlag::ClientSideUnread),
					.from = chat->user()->id,
					.date = date,
				}, document, TextWithEntities{ text });
				registerRandomId(chat->id, item->id, randomId);
				noteMessageTtl(chat->id, item->id, fields.vttl().v);
				writeMessagesLocal();
				startedDownload = true; // message already created
			}, [&](const auto &) {
				if (text.isEmpty()) {
					text = u"[unsupported media]"_q;
				}
			});
		}
		if (!startedDownload) {
			addDecryptedMessage(
				chat,
				text.toUtf8(),
				date,
				false,
				uint64(fields.vrandom_id().v),
				fields.vttl().v);
		}
	}, [&](const decrypted::MTPDdecryptedMessageService &service) {
		service.vaction().match([&](
				const decrypted::MTPDdecryptedMessageActionSetMessageTTL &a) {
			const auto seconds = a.vttl_seconds().v;
			chat->setTtl(seconds);
			addTtlChangeNotice(chat, seconds, date, false);
			writeLocal();
		}, [&](
				const decrypted::MTPDdecryptedMessageActionNotifyLayer &a) {
			chat->setLayer(a.vlayer().v);
			writeLocal();
			// If the peer announced a layer below ours, reply with our own
			// layer so it learns we speak the newer protocol (mirrors
			// SecretChatHelper.java applyPeerLayer). Equal/higher layers -- the
			// common case -- do not re-trigger, so there is no reply loop.
			if (a.vlayer().v < decrypted::details::kCurrentLayer) {
				sendNotifyLayer(chat);
			}
		}, [&](
				const decrypted::MTPDdecryptedMessageActionDeleteMessages &a) {
			for (const auto &rid : a.vrandom_ids().v) {
				const auto msgId = findByRandomId(
					chat->id,
					uint64(rid.v));
				if (!msgId) {
					continue;
				}
				if (const auto item = _session->data().message(
						FullMsgId(chat->id, msgId))) {
					item->destroy();
				}
				unregisterRandomId(chat->id, msgId);
				if (const auto k = _messageTtls.find(chat->id);
						k != _messageTtls.end()) {
					k->second.remove(msgId);
				}
			}
			writeMessagesLocal();
		}, [&](
				const decrypted::MTPDdecryptedMessageActionFlushHistory &a) {
			_session->data().history(chat->id)->clear(
				History::ClearType::ClearHistory);
			clearChatRandomIds(chat->id);
			_messageTtls.remove(chat->id);
			_ttlNotices.remove(chat->id);
			writeMessagesLocal();
		}, [&](
				const decrypted::MTPDdecryptedMessageActionScreenshotMessages &a) {
			const auto who = chat->user()
				? chat->user()->shortName()
				: QString();
			const auto text = who.isEmpty()
				? u"A screenshot was taken."_q
				: (who + u" took a screenshot."_q);
			addDecryptedMessage(chat, text.toUtf8(), date, false);
		}, [&](
				const decrypted::MTPDdecryptedMessageActionRequestKey &a) {
			handleRequestKey(
				chat,
				uint64(a.vexchange_id().v),
				bytes::make_span(a.vg_a().v));
		}, [&](
				const decrypted::MTPDdecryptedMessageActionAcceptKey &a) {
			handleAcceptKey(
				chat,
				uint64(a.vexchange_id().v),
				bytes::make_span(a.vg_b().v),
				uint64(a.vkey_fingerprint().v));
		}, [&](
				const decrypted::MTPDdecryptedMessageActionCommitKey &a) {
			handleCommitKey(
				chat,
				uint64(a.vexchange_id().v),
				uint64(a.vkey_fingerprint().v));
		}, [&](
				const decrypted::MTPDdecryptedMessageActionAbortKey &a) {
			handleAbortKey(chat, uint64(a.vexchange_id().v));
		}, [&](
				const decrypted::MTPDdecryptedMessageActionResend &a) {
			// The peer missed some of our messages and asks us to resend
			// the given out_seq_no range.
			handleResend(
				chat,
				a.vstart_seq_no().v,
				a.vend_seq_no().v);
		}, [&](
				const decrypted::MTPDdecryptedMessageActionReadMessages &a) {
			// The peer read our outgoing messages (identified by random_id):
			// flip them to read and start their self-destruct timers, the
			// same effect as the server-level updateEncryptedMessagesRead.
			// Mobile clients send this in-content action too (mirrors
			// SecretChatHelper.java's TL_decryptedMessageActionReadMessages
			// -> createTaskForSecretChat), so honour it as a second read path.
			handleReadMessages(chat, a.vrandom_ids().v);
		}, [&](const decrypted::MTPDdecryptedMessageActionTyping &a) {
			// Typing is transported out-of-band via messages.setEncryptedTyping
			// (see chatTyping); the in-message action is legacy, so ignore it.
		}, [&](const decrypted::MTPDdecryptedMessageActionNoop &a) {
			// Padding only: the out_seq_no it carries was already consumed by
			// setInSeqNo before dispatch, so there is nothing left to do.
		}, [&](const auto &) {
			// Any remaining actions carry no desktop-side effect.
		});
	});
}

void EncryptedChats::rememberSentLayer(
		int32 chatId,
		int32 outSeqNo,
		bytes::const_span serialized,
		uint64 randomId,
		bool isService) {
	auto &cache = _sentLayers[chatId];
	auto entry = SentLayer();
	entry.serialized = bytes::make_vector(serialized);
	entry.randomId = randomId;
	entry.isService = isService;
	cache[outSeqNo] = std::move(entry);
	// Bound the cache; a resend request for an evicted seq falls back to a
	// delete tombstone. std::map keeps the keys ordered, so begin() is the
	// oldest (lowest) out_seq_no.
	while (cache.size() > kSentLayerCacheSize) {
		cache.erase(cache.begin());
	}
}

void EncryptedChats::drainHoles(not_null<SecretChatData*> chat) {
	const auto id = chat->secretChatId();
	const auto i = _holes.find(id);
	if (i == _holes.end()) {
		return;
	}
	auto &holes = i->second;
	// Repeatedly pull the next-expected (or already-superseded) buffered message
	// and replay it in seq order, until none line up -- mirrors Android
	// checkSecretHoles. A message becomes processable once the gap before it has
	// been filled by the peer's resends.
	for (auto progressed = true; progressed;) {
		progressed = false;
		for (auto it = holes.begin(); it != holes.end(); ++it) {
			const auto expected = (it->outSeqNo == chat->inSeqNo() + 2);
			const auto superseded = (it->outSeqNo <= chat->inSeqNo());
			if (!expected && !superseded) {
				continue;
			}
			auto buffered = std::move(*it);
			holes.erase(it);
			progressed = true;
			if (expected) {
				chat->setInSeqNo(buffered.outSeqNo);
				writeLocal();
				processDecryptedMessage(
					chat,
					buffered.message,
					buffered.date,
					buffered.file ? &*buffered.file : nullptr);
			}
			break;
		}
	}
	if (holes.empty()) {
		_holes.remove(id);
	}
}

void EncryptedChats::sendResendRequest(
		not_null<SecretChatData*> chat,
		int32 startSeqNo,
		int32 endSeqNo) {
	if (endSeqNo < startSeqNo) {
		return;
	}
	DEBUG_LOG(("Secret Chat: requesting resend of %1..%2 in chat %3."
		).arg(startSeqNo).arg(endSeqNo).arg(chat->secretChatId()));
	sendServiceAction(
		chat,
		decrypted::MTP_decryptedMessageActionResend(
			MTP_int(startSeqNo),
			MTP_int(endSeqNo)));
}

void EncryptedChats::handleResend(
		not_null<SecretChatData*> chat,
		int32 startSeqNo,
		int32 endSeqNo) {
	if (!chat->hasKey() || chat->state() != SecretChatState::Ready) {
		return;
	}
	// Never re-send anything the peer has already acknowledged: clamp the start
	// to its reported in_seq_no and drop the request entirely if it asks only
	// for already-ACKed messages. Guards against a buggy or malicious peer
	// forcing us to re-encrypt very old messages (SecretChatHelper.java:1314).
	const auto peerAcked = chat->peerInSeqNo();
	if (endSeqNo < peerAcked) {
		return;
	}
	startSeqNo = std::max(startSeqNo, peerAcked);
	if (endSeqNo < startSeqNo
		|| (endSeqNo - startSeqNo) > kMaxResendRange) {
		LOG(("Secret Chat Error: bad resend range %1..%2 in chat %3."
			).arg(startSeqNo).arg(endSeqNo).arg(chat->secretChatId()));
		return;
	}
	const auto i = _sentLayers.find(chat->secretChatId());
	const auto cache = (i != _sentLayers.end()) ? &i->second : nullptr;
	// out_seq_no values advance by 2; resend each requested message from the
	// cache, or fill the slot with a delete tombstone if it is gone. The loop
	// counter is int64 so `seq += 2` cannot overflow when endSeqNo is near
	// INT_MAX (kMaxResendRange keeps the span bounded, so the cast back to int32
	// is always in range).
	for (int64 seq = startSeqNo; seq <= endSeqNo; seq += 2) {
		auto resent = false;
		if (cache) {
			const auto j = cache->find(int32(seq));
			if (j != cache->end()) {
				resendSentLayer(chat, j->second);
				resent = true;
			}
		}
		if (!resent) {
			resendTombstone(chat, int32(seq));
		}
	}
}

void EncryptedChats::resendSentLayer(
		not_null<SecretChatData*> chat,
		const SentLayer &layer) {
	// Re-encrypt with the current key (it may have been rekeyed since): the
	// serialized layer already carries the original in/out seq, so the peer sees
	// the exact same message and fills its hole. Reuse the original random_id so
	// the peer de-duplicates the resend against any copy it did receive.
	const auto data = MTP::SecretChat::Encrypt(
		layer.serialized,
		chat->key(),
		chat->keyFingerprint(),
		chat->amCreator());
	const auto chatId = chat->secretChatId();
	const auto requestId = layer.isService
		? _mtp.request(MTPmessages_SendEncryptedService(
			inputChat(chat),
			MTP_long(layer.randomId),
			MTP_bytes(data)
		)).afterRequest(sendAfter(chatId)).send()
		: _mtp.request(MTPmessages_SendEncrypted(
			MTP_flags(0),
			inputChat(chat),
			MTP_long(layer.randomId),
			MTP_bytes(data)
		)).afterRequest(sendAfter(chatId)).send();
	setSendAfter(chatId, requestId);
}

void EncryptedChats::resendTombstone(
		not_null<SecretChatData*> chat,
		int32 outSeqNo) {
	// We can no longer reconstruct this message (evicted from the cache, or a
	// file message we never cached). Occupy its out_seq_no with an empty delete
	// so the peer's sequence stays intact and it stops holing on this slot --
	// matching the Android fallback (createDeleteMessage).
	const auto randomId = base::RandomValue<uint64>();
	auto randomBytes = bytes::vector(16);
	bytes::set_random(randomBytes);
	const auto service = decrypted::MTP_decryptedMessageService(
		MTP_long(randomId),
		decrypted::MTP_decryptedMessageActionDeleteMessages(
			MTP_vector<MTPlong>()));
	const auto layer = decrypted::MTP_decryptedMessageLayer(
		MTP_bytes(randomBytes),
		MTP_int(decrypted::details::kCurrentLayer),
		MTP_int(chat->currentInSeqNo()),
		MTP_int(outSeqNo),
		service);
	const auto serialized = MTP::SecretChat::SerializeObject(
		decrypted::MTPDecryptedMessageLayer(layer));
	const auto data = MTP::SecretChat::Encrypt(
		serialized,
		chat->key(),
		chat->keyFingerprint(),
		chat->amCreator());
	const auto chatId = chat->secretChatId();
	const auto requestId = _mtp.request(MTPmessages_SendEncryptedService(
		inputChat(chat),
		MTP_long(randomId),
		MTP_bytes(data)
	)).afterRequest(sendAfter(chatId)).send();
	setSendAfter(chatId, requestId);
}

void EncryptedChats::startFileDownload(std::shared_ptr<IncomingFile> state) {
	const auto limit = Storage::kDownloadPartSize;
	const auto offset = state->offset;
	// cdn_supported is intentionally NOT set: for the (small) secret files we
	// support so far we want the bytes straight from the file dc, no CDN.
	_mtp.request(MTPupload_GetFile(
		MTP_flags(0),
		MTP_inputEncryptedFileLocation(
			MTP_long(state->fileId),
			MTP_long(state->accessHash)),
		MTP_long(offset),
		MTP_int(limit)
	)).done([=](const MTPupload_File &result) {
		result.match([&](const MTPDupload_file &data) {
			const auto &part = data.vbytes().v;
			state->data.append(
				reinterpret_cast<const char*>(part.data()),
				int(part.size()));
			state->offset += int64(part.size());
			if (int64(part.size()) < limit
				|| state->data.size() >= state->ciphertextSize) {
				finishFileDownload(state);
			} else {
				startFileDownload(state);
			}
		}, [&](const MTPDupload_fileCdnRedirect &) {
			LOG(("Secret Chat Error: CDN file download not supported yet."));
		});
	}).fail([=](const MTP::Error &error) {
		LOG(("Secret Chat Error: file download failed: %1"
			).arg(error.type()));
	}).toDC(MTP::downloadDcId(state->dcId, 0)).send();
}

void EncryptedChats::createPendingMediaItem(
		std::shared_ptr<IncomingFile> state) {
	// Build the bubble before the encrypted file finishes downloading, carrying
	// only the unencrypted inline thumb so a large media isn't invisible while it
	// transfers. finishFileDownload attaches the real file to this same item.
	if (state->asPhoto) {
		// Photos are capped at 2048x2048 and recompressed to JPEG (small + fast),
		// so we skip the placeholder entirely and just show the full image once it
		// arrives -- this avoids the in-memory image-slot upgrade limitations of
		// PhotoData (UpdateCloudFile won't swap a loaded in-memory slot in place).
		return;
	} else if (state->thumbBytes.isEmpty()) {
		return; // No preview to show -> build the bubble once the file arrives.
	}
	const auto chat = _session->data().secretChatLoaded(
		secretChatIdFromWire(state->chatId));
	if (!chat) {
		return;
	}
	ensureInDialogs(chat);
	const auto history = _session->data().history(chat->id);
	const auto makeFields = [&] {
		return HistoryItemCommonFields{
			.id = _session->data().nextLocalMessageId(),
			.flags = MessageFlags(MessageFlag::Local
				| MessageFlag::ClientSideUnread),
			.from = chat->user()->id,
			.date = state->date,
		};
	};
	const auto caption = TextWithEntities{ state->caption };

	// Document/video path: the mime + attributes are already known, so the bubble
	// shows the right media type with its poster thumb; only the file location is
	// missing until the download completes.
	auto attributes = state->attributes;
	auto hasFilename = false;
	for (const auto &attribute : attributes) {
		attribute.match([&](const MTPDdocumentAttributeFilename &) {
			hasFilename = true;
		}, [](const auto &) {
		});
	}
	if (!hasFilename) {
		const auto name = state->filename.isEmpty()
			? (u"file_"_q + QString::number(state->fileId))
			: state->filename;
		attributes.push_back(
			MTP_documentAttributeFilename(MTP_string(name)));
	}
	const auto document = _session->data().processDocument(MTP_document(
		MTP_flags(0),
		MTP_long(state->fileId),
		MTP_long(0), // access_hash -- local, attached on completion
		MTP_bytes(), // file_reference
		MTP_int(state->date),
		MTP_string(state->mime),
		MTP_long(state->plaintextSize),
		LocalDocumentThumbs(
			state->thumbBytes,
			state->thumbWidth,
			state->thumbHeight),
		MTPVector<MTPVideoSize>(), // video_thumbs (flag.1 unset)
		MTP_int(0), // dc_id
		MTP_vector<MTPDocumentAttribute>(attributes)));
	const auto item = history->addNewLocalMessage(
		makeFields(),
		document,
		caption);
	if (!item) {
		return;
	}
	state->pendingItemId = item->id;
	if (state->randomId) {
		registerRandomId(chat->id, item->id, state->randomId);
	}
	noteMessageTtl(chat->id, item->id, state->ttl);
}

bool EncryptedChats::writeSecretFileEncrypted(
		const QString &path,
		const QByteArray &plain) {
	const auto local = _session->local().peekLegacyLocalKey();
	if (!local) {
		return false;
	}
	auto key = Storage::EncryptionKey(bytes::make_vector(local->data()));
	auto file = Storage::File();
	if (file.open(path, Storage::File::Mode::Write, key)
			!= Storage::File::Result::Success) {
		return false;
	}
	// writeWithPadding encrypts the buffer in place, so hand it a detached copy
	// to keep the caller's plaintext intact.
	auto copy = plain;
	const auto span = bytes::make_detached_span(copy);
	const auto ok = span.empty() || file.writeWithPadding(span);
	file.flush();
	file.close();
	return ok;
}

QByteArray EncryptedChats::readSecretFileEncrypted(
		const QString &path,
		int64 plaintextSize) {
	const auto local = _session->local().peekLegacyLocalKey();
	if (!local) {
		return QByteArray();
	}
	auto key = Storage::EncryptionKey(bytes::make_vector(local->data()));
	auto file = Storage::File();
	if (file.open(path, Storage::File::Mode::Read, key)
			!= Storage::File::Result::Success) {
		return QByteArray();
	}
	// Storage::File pads to a 16-byte block (random tail) and does not store the
	// exact plaintext size, so truncate to the known size when we have it.
	const auto padded = file.size();
	auto result = QByteArray();
	if (padded > 0) {
		result = QByteArray(int(padded), Qt::Uninitialized);
		const auto read = file.read(bytes::make_detached_span(result));
		result.resize((read > 0) ? int(read) : 0);
	}
	file.close();
	if (plaintextSize >= 0 && plaintextSize <= result.size()) {
		result.resize(int(plaintextSize));
	}
	return result;
}

void EncryptedChats::finishFileDownload(std::shared_ptr<IncomingFile> state) {
	const auto chat = _session->data().secretChatLoaded(
		secretChatIdFromWire(state->chatId));
	if (!chat) {
		return;
	}
	auto cipher = state->data;
	if (cipher.size() > state->ciphertextSize) {
		cipher = cipher.left(state->ciphertextSize);
	}
	const auto decrypted = MTP::SecretChat::DecryptFileContent(
		bytes::make_span(cipher),
		state->key,
		state->iv);
	if (!decrypted) {
		LOG(("Secret Chat Error: could not decrypt downloaded file."));
		return;
	}
	auto plain = BytesToQ(*decrypted);
	if (state->plaintextSize >= 0 && state->plaintextSize <= plain.size()) {
		plain = plain.left(state->plaintextSize);
	}

	// Mirror the decrypted bytes into the local-key encrypted cache so a
	// document's first display is instant and survives a media-view churn /
	// restart without re-decrypting. The durable encrypted secret_files copy
	// stays the source of truth (the cache may evict and SecretFileLoader then
	// re-decrypts from it). Photos render from their in-memory InMemoryLocation
	// copy, so this is for documents only.
	const auto cacheDocumentBytes = [&](not_null<DocumentData*> document) {
		if (plain.size() <= Storage::kMaxFileInMemory) {
			_session->data().cache().put(
				document->cacheKey(),
				Storage::Cache::Database::TaggedValue(
					base::duplicate(plain),
					document->cacheTag()));
		}
	};

	auto name = state->filename.isEmpty()
		? (u"file_"_q + QString::number(state->fileId))
		: state->filename;
	name.replace('/', '_').replace('\\', '_');
	const auto dir = cWorkingDir() + u"tdata/secret_files/"_q;
	QDir().mkpath(dir);
	const auto path = dir + QString::number(state->fileId) + '_' + name;
	if (!writeSecretFileEncrypted(path, plain)) {
		LOG(("Secret Chat Error: could not write decrypted file."));
		return;
	}

	// If a document/video bubble was created up-front to show the inline thumb
	// during download, attach the finished file to it IN PLACE (setSecretEncrypted
	// Location) rather than adding a new message. Photos never have a placeholder
	// (see createPendingMediaItem) -> they fall through to the full-image build below.
	if (state->pendingItemId) {
		const auto item = _session->data().message(
			FullMsgId(chat->id, state->pendingItemId));
		if (item) {
			const auto document = _session->data().document(state->fileId);
			document->setSecretEncryptedLocation(path);
			cacheDocumentBytes(document);
			_session->data().requestItemRepaint(item);
			writeMessagesLocal();
			return;
		}
	}

	ensureInDialogs(chat);
	const auto history = _session->data().history(chat->id);
	const auto makeFields = [&] {
		return HistoryItemCommonFields{
			.id = _session->data().nextLocalMessageId(),
			.flags = MessageFlags(MessageFlag::Local
				| MessageFlag::ClientSideUnread),
			.from = chat->user()->id,
			.date = state->date,
		};
	};
	const auto caption = TextWithEntities{ state->caption };

	auto item = (HistoryItem*)nullptr;
	if (state->asPhoto) {
		// Rebuild a local PhotoData from the decrypted JPEG so it renders inline
		// (MediaFile has no isImage->Photo branch). Falls back to the document
		// path below if the bytes don't decode as an image.
		auto image = QImage();
		if (image.loadFromData(plain) && !image.isNull()) {
			const auto bytes = Images::FromImageInMemory(image, "JPG", plain);
			const auto photo = _session->data().photo(
				state->fileId,
				uint64(0), // access_hash -- local
				QByteArray(), // file_reference
				state->date,
				0, // dc_id
				false, // has_stickers
				state->thumbBytes, // inline thumbnail
				ImageWithLocation(), // small
				bytes, // thumbnail
				bytes, // large
				ImageWithLocation(), // video small
				ImageWithLocation(), // video large
				crl::time(0));
			// The large image is an in-memory (InMemoryLocation) copy of the
			// decrypted JPEG, so inline/fullscreen/save all render from RAM. We
			// must NOT point a plaintext FileLocation at the now-encrypted file;
			// carry the encrypted path only so it can be persisted + reloaded.
			photo->setSecretEncryptedLocation(path);
			item = history->addNewLocalMessage(makeFields(), photo, caption);
		}
	}
	if (!item) {
		// Document path (and the photo-decode fallback). The decrypted media's
		// attributes drive inline video/voice rendering; always carry a filename
		// so the bubble has a name and the right name type.
		auto attributes = state->attributes;
		auto hasFilename = false;
		for (const auto &attribute : attributes) {
			attribute.match([&](const MTPDdocumentAttributeFilename &) {
				hasFilename = true;
			}, [](const auto &) {
			});
		}
		if (!hasFilename) {
			attributes.push_back(
				MTP_documentAttributeFilename(MTP_string(name)));
		}
		const auto document = _session->data().processDocument(MTP_document(
			MTP_flags(0),
			MTP_long(state->fileId),
			MTP_long(0), // access_hash -- local file, no server reference
			MTP_bytes(), // file_reference
			MTP_int(state->date),
			MTP_string(state->mime),
			MTP_long(plain.size()),
			LocalDocumentThumbs( // inline preview thumb (empty -> no thumbs)
				state->thumbBytes,
				state->thumbWidth,
				state->thumbHeight),
			MTPVector<MTPVideoSize>(), // video_thumbs (flag.1 unset)
			MTP_int(0), // dc_id
			MTP_vector<MTPDocumentAttribute>(attributes)));
		document->setSecretEncryptedLocation(path);
		cacheDocumentBytes(document);
		item = history->addNewLocalMessage(makeFields(), document, caption);
	}
	if (state->randomId) {
		registerRandomId(chat->id, item->id, state->randomId);
	}
	noteMessageTtl(chat->id, item->id, state->ttl);
	writeMessagesLocal();
}

void EncryptedChats::messagesRead(int32 chatId, TimeId maxDate) {
	const auto chat = _session->data().secretChatLoaded(
		secretChatIdFromWire(chatId));
	if (!chat) {
		return;
	}
	// The partner read our outgoing messages up to maxDate. Local MsgIds are
	// negative but still increase with date, so the newest outgoing item with
	// date <= maxDate is the highest id to mark read. Advancing the History's
	// outbox-read-till flips those bubbles from ✓ to ✓✓ (see HistoryItem::unread).
	auto readTill = std::optional<MsgId>();
	const auto i = _messageRandomIds.find(chat->id);
	if (i != _messageRandomIds.end()) {
		for (const auto &[msgId, randomId] : i->second) {
			const auto item = _session->data().message(
				FullMsgId(chat->id, msgId));
			if (item
				&& item->out()
				&& item->date() <= maxDate
				&& (!readTill || msgId > *readTill)) {
				readTill = msgId;
			}
		}
	}
	if (readTill) {
		_session->data().history(chat->id)->outboxRead(*readTill);
		// Persist the advanced read frontier: serializeMessages snapshots each
		// outgoing message's read-state, so without this the ✓✓ would be lost
		// on restart (it would only reappear when the next read update arrives).
		writeMessagesLocal();
	}
	// The partner read our outgoing messages up to maxDate: start their
	// self-destruct timers (the sender's timer starts on the recipient's read).
	startSelfDestructTimers(chat, true, maxDate);
}

void EncryptedChats::handleReadMessages(
		not_null<SecretChatData*> chat,
		const QVector<MTPlong> &randomIds) {
	// Random-id-keyed counterpart of messagesRead(): the partner explicitly
	// reports which of our outgoing messages it read. Flip them to read (✓✓)
	// and arm each one's self-destruct timer (the sender's TTL starts on the
	// recipient's read), then persist so both survive a restart.
	const auto now = base::unixtime::now();
	const auto ttls = _messageTtls.find(chat->id);
	const auto notices = _ttlNotices.find(chat->id);
	auto readTill = std::optional<MsgId>();
	auto armed = false;
	for (const auto &rid : randomIds) {
		const auto msgId = findByRandomId(chat->id, uint64(rid.v));
		if (!msgId) {
			continue;
		}
		const auto item = _session->data().message(
			FullMsgId(chat->id, msgId));
		if (!item || !item->out()) {
			continue;
		}
		if (!readTill || msgId > *readTill) {
			readTill = msgId;
		}
		if (ttls == _messageTtls.end()) {
			continue;
		}
		const auto j = ttls->second.find(msgId);
		if (j == ttls->second.end()) {
			continue;
		}
		// A ttl change-notice must never self-destruct; just drop its timer.
		const auto isNotice = (notices != _ttlNotices.end())
			&& notices->second.contains(msgId);
		if (!isNotice) {
			item->setSecretChatSelfDestructAt(now + j->second);
		}
		ttls->second.erase(j);
		armed = true;
	}
	if (ttls != _messageTtls.end() && ttls->second.empty()) {
		_messageTtls.erase(ttls);
	}
	if (readTill) {
		_session->data().history(chat->id)->outboxRead(*readTill);
	}
	if (readTill || armed) {
		writeMessagesLocal();
	}
}

void EncryptedChats::chatTyping(int32 chatId) {
	const auto chat = _session->data().secretChatLoaded(
		secretChatIdFromWire(chatId));
	if (!chat || !chat->user()) {
		return;
	}
	// updateEncryptedChatTyping carries no action kind, just "is typing";
	// feed it to the shared send-action UI as a plain typing action so the
	// history shows "typing..." the same way a normal 1:1 chat does.
	_session->data().sendActionManager().registerFor(
		_session->data().history(chat->id),
		MsgId(0),
		chat->user(),
		MTP_sendMessageTypingAction(),
		base::unixtime::now());
}

void EncryptedChats::addDecryptedMessage(
		not_null<SecretChatData*> chat,
		const QByteArray &decrypted,
		TimeId date,
		bool outgoing,
		uint64 randomId,
		int32 ttlSeconds) {
	ensureInDialogs(chat);
	const auto history = _session->data().history(chat->id);
	const auto from = outgoing
		? _session->userPeerId()
		: chat->user()->id;
	auto flags = MessageFlags(MessageFlag::Local);
	if (outgoing) {
		flags |= MessageFlag::Outgoing;
	} else {
		// Marks the incoming message unread so the read path fires when the
		// chat is viewed (we then tell the partner via readHistory). Cleared
		// in Histories::readInboxTill once read.
		flags |= MessageFlag::ClientSideUnread;
	}
	const auto item = history->addNewLocalMessage({
		.id = _session->data().nextLocalMessageId(),
		.flags = flags,
		.from = from,
		.date = date,
	}, TextWithEntities{ QString::fromUtf8(decrypted) }, MTP_messageMediaEmpty());
	if (randomId) {
		registerRandomId(chat->id, item->id, randomId);
	}
	noteMessageTtl(chat->id, item->id, ttlSeconds);
	writeMessagesLocal();
}

TextWithEntities EncryptedChats::ttlNoticeText(
		not_null<SecretChatData*> chat,
		int seconds,
		bool outgoing) const {
	if (outgoing) {
		return TextWithEntities{ seconds
			? tr::lng_action_ttl_changed_you(
				tr::now,
				lt_duration,
				SecretChatTtlDuration(seconds))
			: tr::lng_action_ttl_removed_you(tr::now) };
	}
	// Prefer the partner's first name, then full name, then the secret chat's
	// mirrored display name (which itself falls back to phone / "Secret chat"), so
	// the notice never drops the {from} for a minimal/unresolved partner user.
	const auto user = chat->user();
	auto from = user ? user->shortName() : QString();
	if (from.isEmpty() && user) {
		from = user->name();
	}
	if (from.isEmpty()) {
		from = chat->name();
	}
	return TextWithEntities{ seconds
		? tr::lng_action_ttl_changed(
			tr::now,
			lt_from,
			from,
			lt_duration,
			SecretChatTtlDuration(seconds))
		: tr::lng_action_ttl_removed(tr::now, lt_from, from) };
}

not_null<HistoryItem*> EncryptedChats::buildTtlServiceMessage(
		not_null<SecretChatData*> chat,
		int seconds,
		TimeId date,
		bool outgoing,
		MsgId id,
		TextWithEntities text) {
	// Centered service notice. The live path derives the text now (partner loaded);
	// restore passes the persisted rendered text so the real name survives even if
	// the minimal partner never re-resolves.
	if (text.empty()) {
		text = ttlNoticeText(chat, seconds, outgoing);
	}
	const auto history = _session->data().history(chat->id);
	auto flags = MessageFlags(MessageFlag::Local);
	if (outgoing) {
		flags |= MessageFlag::Outgoing;
	}
	const auto item = history->makeMessage(
		HistoryItemCommonFields{
			.id = id,
			.flags = flags,
			.from = outgoing ? _session->userPeerId() : chat->user()->id,
			.date = date,
		},
		PreparedServiceText{ std::move(text) });
	// The partner user can be minimal at restore (name not loaded yet) -> the
	// incoming notice would bake "Secret chat". Re-render every notice once the
	// real name arrives (mirrors SecretChatData's own name-mirror subscription).
	watchTtlNoticeNames(chat);
	return history->addNewLocalMessage(item);
}

void EncryptedChats::watchTtlNoticeNames(not_null<SecretChatData*> chat) {
	const auto user = chat->user();
	if (!user || _ttlNoticeNameWatch.contains(chat->secretChatId())) {
		return;
	}
	auto &lifetime = _ttlNoticeNameWatch[chat->secretChatId()];
	_session->changes().peerUpdates(
		user,
		Data::PeerUpdate::Flag::Name
	) | rpl::on_next([=] {
		refreshTtlNotices(chat);
	}, lifetime);
}

void EncryptedChats::refreshTtlNotices(not_null<SecretChatData*> chat) {
	const auto user = chat->user();
	// Only re-render when the partner is actually resolved -- never downgrade a
	// correctly shown name back to the "Secret chat" fallback on a transient
	// Name update (the partner can momentarily read empty while it reloads).
	if (!user || user->name().isEmpty()) {
		return;
	}
	const auto i = _ttlNotices.find(chat->id);
	if (i == _ttlNotices.end()) {
		return;
	}
	for (const auto &[msgId, seconds] : i->second) {
		const auto item = _session->data().message(FullMsgId(chat->id, msgId));
		if (!item) {
			continue;
		}
		item->updateServiceText(
			PreparedServiceText{ ttlNoticeText(chat, seconds, item->out()) });
	}
	// Re-persist so the now-resolved name is captured (serialize re-derives the
	// notice text); otherwise the next restart would replay the stale fallback.
	writeMessagesLocal();
}

void EncryptedChats::addTtlChangeNotice(
		not_null<SecretChatData*> chat,
		int seconds,
		TimeId date,
		bool outgoing) {
	ensureInDialogs(chat);
	const auto item = buildTtlServiceMessage(
		chat,
		seconds,
		date,
		outgoing,
		_session->data().nextLocalMessageId());
	// Register a random_id (so it rides the persist index, like every other
	// secret message -- Android also assigns service messages a random_id) and
	// record the ttl value so serialize emits a StoredKind::ServiceTtl record.
	registerRandomId(chat->id, item->id, base::RandomValue<uint64>());
	_ttlNotices[chat->id][item->id] = seconds;
	// A change-notice must never self-destruct -- make sure it carries no pending
	// timer (defensive: it is created fresh and is never noteMessageTtl'd).
	if (const auto k = _messageTtls.find(chat->id); k != _messageTtls.end()) {
		k->second.remove(item->id);
	}
	writeMessagesLocal();
}

void EncryptedChats::noteMessageTtl(
		PeerId chatId,
		MsgId msgId,
		int32 ttlSeconds) {
	if (ttlSeconds > 0) {
		_messageTtls[chatId][msgId] = ttlSeconds;
	}
}

int32 EncryptedChats::messageTtlSeconds(PeerId chatId, MsgId msgId) const {
	const auto i = _messageTtls.find(chatId);
	if (i == _messageTtls.end()) {
		return 0;
	}
	const auto j = i->second.find(msgId);
	return (j != i->second.end()) ? j->second : 0;
}

void EncryptedChats::startSelfDestructTimers(
		not_null<SecretChatData*> chat,
		bool outgoing,
		TimeId tillDate) {
	const auto i = _messageTtls.find(chat->id);
	if (i == _messageTtls.end()) {
		return;
	}
	const auto now = base::unixtime::now();
	const auto notices = _ttlNotices.find(chat->id);
	auto armed = false;
	for (auto j = i->second.begin(); j != i->second.end();) {
		const auto msgId = j->first;
		const auto ttl = j->second;
		// A ttl change-notice must never self-destruct -- drop any stray pending
		// timer for it without arming.
		if (notices != _ttlNotices.end() && notices->second.contains(msgId)) {
			j = i->second.erase(j);
			armed = true;
			continue;
		}
		const auto item = _session->data().message(
			FullMsgId(chat->id, msgId));
		if (!item) {
			// The message is gone (deleted) -> drop the pending timer.
			j = i->second.erase(j);
			armed = true;
			continue;
		}
		if (item->out() != outgoing || item->date() > tillDate) {
			++j; // Not in the read range / wrong direction yet.
			continue;
		}
		// Read now -> destroy ttl seconds from now. The owner's message-TTL timer
		// owns it from here, so drop the pending entry.
		item->setSecretChatSelfDestructAt(now + ttl);
		j = i->second.erase(j);
		armed = true;
	}
	if (i->second.empty()) {
		_messageTtls.erase(i);
	}
	if (armed) {
		writeMessagesLocal();
	}
}

void EncryptedChats::registerRandomId(
		PeerId chatId,
		MsgId msgId,
		uint64 randomId) {
	_messageRandomIds[chatId][msgId] = randomId;
	_randomIdToMsg[chatId][randomId] = msgId;
}

std::optional<uint64> EncryptedChats::unregisterRandomId(
		PeerId chatId,
		MsgId msgId) {
	const auto i = _messageRandomIds.find(chatId);
	if (i == _messageRandomIds.end()) {
		return std::nullopt;
	}
	const auto j = i->second.find(msgId);
	if (j == i->second.end()) {
		return std::nullopt;
	}
	const auto randomId = j->second;
	i->second.erase(j);
	if (i->second.empty()) {
		_messageRandomIds.erase(i);
	}
	if (const auto r = _randomIdToMsg.find(chatId);
			r != _randomIdToMsg.end()) {
		r->second.remove(randomId);
		if (r->second.empty()) {
			_randomIdToMsg.erase(r);
		}
	}
	return randomId;
}

void EncryptedChats::clearChatRandomIds(PeerId chatId) {
	_messageRandomIds.remove(chatId);
	_randomIdToMsg.remove(chatId);
}

MsgId EncryptedChats::findByRandomId(PeerId chatId, uint64 randomId) const {
	const auto i = _randomIdToMsg.find(chatId);
	if (i != _randomIdToMsg.end()) {
		const auto j = i->second.find(randomId);
		if (j != i->second.end()) {
			return j->second;
		}
	}
	return MsgId(0);
}

void EncryptedChats::deleteMessages(
		not_null<SecretChatData*> chat,
		const std::vector<MsgId> &localIds) {
	auto randomIds = QVector<MTPlong>();
	for (const auto &msgId : localIds) {
		if (const auto randomId = unregisterRandomId(chat->id, msgId)) {
			randomIds.push_back(MTP_long(*randomId));
		}
		if (const auto item = _session->data().message(
				FullMsgId(chat->id, msgId))) {
			item->destroy();
		}
		if (const auto k = _messageTtls.find(chat->id);
				k != _messageTtls.end()) {
			k->second.remove(msgId);
		}
	}
	writeMessagesLocal();
	if (randomIds.isEmpty()
		|| !chat->hasKey()
		|| chat->state() != SecretChatState::Ready) {
		return;
	}

	sendServiceAction(
		chat,
		decrypted::MTP_decryptedMessageActionDeleteMessages(
			MTP_vector<MTPlong>(randomIds)));
}

void EncryptedChats::sendServiceAction(
		not_null<SecretChatData*> chat,
		const decrypted::MTPDecryptedMessageAction &action) {
	if (!chat->hasKey() || chat->state() != SecretChatState::Ready) {
		return;
	}
	const auto randomId = base::RandomValue<uint64>();
	const auto service = decrypted::MTP_decryptedMessageService(
		MTP_long(randomId),
		action);
	auto randomBytes = bytes::vector(16);
	bytes::set_random(randomBytes);
	const auto outSeqNo = chat->nextOutSeqNo();
	const auto layer = decrypted::MTP_decryptedMessageLayer(
		MTP_bytes(randomBytes),
		MTP_int(decrypted::details::kCurrentLayer),
		MTP_int(chat->currentInSeqNo()),
		MTP_int(outSeqNo),
		service);
	const auto serialized = MTP::SecretChat::SerializeObject(
		decrypted::MTPDecryptedMessageLayer(layer));
	// Cache service actions for resend too -- they consume an out_seq_no, so a
	// gap on the peer side would otherwise stall its sequence.
	rememberSentLayer(
		chat->secretChatId(),
		outSeqNo,
		serialized,
		randomId,
		/*isService=*/true);
	// NB: encrypt with the key current at call time. For a commitKey action the
	// caller switches to the new key only *after* this returns, so commitKey
	// itself still rides the old key, as the spec requires.
	const auto data = MTP::SecretChat::Encrypt(
		serialized,
		chat->key(),
		chat->keyFingerprint(),
		chat->amCreator());
	const auto requestId = _mtp.request(MTPmessages_SendEncryptedService(
		inputChat(chat),
		MTP_long(randomId),
		MTP_bytes(data)
	)).afterRequest(sendAfter(chat->secretChatId())).send();
	setSendAfter(chat->secretChatId(), requestId);
	writeLocal();
}

void EncryptedChats::sendNotifyLayer(not_null<SecretChatData*> chat) {
	sendServiceAction(
		chat,
		decrypted::MTP_decryptedMessageActionNotifyLayer(
			MTP_int(decrypted::details::kCurrentLayer)));
}

bool EncryptedChats::queuePendingSend(
		not_null<SecretChatData*> chat,
		Fn<void()> retry) {
	if (chat->hasKey() && chat->state() == SecretChatState::Ready) {
		return false;
	}
	// Hold the send until the chat is established; a discarded chat just drops it.
	const auto state = chat->state();
	if (state == SecretChatState::Requested
		|| state == SecretChatState::Waiting) {
		_pendingSends[chat->secretChatId()].push_back(std::move(retry));
	}
	return true;
}

void EncryptedChats::flushPendingSends(not_null<SecretChatData*> chat) {
	const auto i = _pendingSends.find(chat->secretChatId());
	if (i == _pendingSends.end()) {
		return;
	}
	auto actions = std::move(i->second);
	_pendingSends.erase(i);
	// Each action re-enters its send function, which now sees the Ready chat and
	// proceeds (so no re-queue). Replayed in the order the user sent them.
	for (const auto &action : actions) {
		action();
	}
}

mtpRequestId EncryptedChats::sendAfter(int32 chatId) const {
	const auto i = _sendAfter.find(chatId);
	return (i != _sendAfter.end()) ? i->second : mtpRequestId(0);
}

void EncryptedChats::setSendAfter(int32 chatId, mtpRequestId requestId) {
	if (requestId) {
		_sendAfter[chatId] = requestId;
	}
}

void EncryptedChats::maybeStartRekey(not_null<SecretChatData*> chat) {
	if (!chat->hasKey() || chat->state() != SecretChatState::Ready) {
		return;
	} else if (chat->layer() < kMinRekeyLayer) {
		return; // The peer can't negotiate PFS keys at this layer.
	} else if (_rekeys.contains(chat->secretChatId())) {
		return; // An exchange is already in progress for this chat.
	}
	const auto now = base::unixtime::now();
	const auto byCount = (chat->keyUseCountOut() >= kRekeyEveryOut)
		|| (chat->keyUseCountIn() >= kRekeyEveryIn);
	const auto byTime = (chat->keyCreationDate() != 0)
		&& (now - chat->keyCreationDate() >= kRekeyAfter);
	if (byCount || byTime) {
		startRekey(chat);
	}
}

void EncryptedChats::startRekey(not_null<SecretChatData*> chat) {
	const auto id = chat->secretChatId();
	ensureDhConfig([=] {
		// Another exchange may have started (or one was inbound) while we waited
		// for the DH config.
		if (_rekeys.contains(id)) {
			return;
		}
		const auto power = RandomPower();
		const auto first = MTP::CreateModExp(_dhConfig.g, _dhConfig.p, power);
		if (first.modexp.empty()) {
			LOG(("Secret Chat Error: Could not create rekey g_a."));
			return;
		}
		const auto exchangeId = base::RandomValue<uint64>();
		auto &rk = _rekeys[id];
		rk.stage = Rekey::Stage::Requested;
		rk.exchangeId = exchangeId;
		rk.randomPower = first.randomPower;
		sendServiceAction(
			chat,
			decrypted::MTP_decryptedMessageActionRequestKey(
				MTP_long(exchangeId),
				MTP_bytes(first.modexp)));
		DEBUG_LOG(("Secret Chat: rekey requested for chat %1, exchange_id %2."
			).arg(id).arg(exchangeId));
	});
}

void EncryptedChats::handleRequestKey(
		not_null<SecretChatData*> chat,
		uint64 exchangeId,
		bytes::const_span gA) {
	const auto id = chat->secretChatId();
	const auto i = _rekeys.find(id);
	if (i != _rekeys.end()) {
		if (i->second.stage == Rekey::Stage::Accepted) {
			// We already answered an exchange; a duplicate requestKey is ignored
			// (the spec forbids aborting after acceptKey was sent).
			return;
		}
		// Simultaneous initiation: both sides sent requestKey. The instance with
		// the smaller exchange_id is aborted; the larger one wins.
		if (i->second.exchangeId > exchangeId) {
			return; // Ours is larger -> we win; ignore the incoming request.
		} else if (i->second.exchangeId == exchangeId) {
			_rekeys.erase(i); // 2^-64 collision -> both abort silently.
			return;
		}
		_rekeys.erase(i); // Ours is smaller -> abort it and accept the incoming.
	}

	// We become the accepting side: generate b, compute the new key = g_a^b.
	auto gAcopy = bytes::make_vector(gA);
	ensureDhConfig([=] {
		const auto power = RandomPower();
		const auto first = MTP::CreateModExp(_dhConfig.g, _dhConfig.p, power);
		if (first.modexp.empty()) {
			return;
		}
		const auto computed = MTP::SecretChat::ComputeKey(
			gAcopy,
			first.randomPower,
			_dhConfig.p);
		if (!computed) {
			LOG(("Secret Chat Error: Bad rekey g_a, cannot accept."));
			sendServiceAction(
				chat,
				decrypted::MTP_decryptedMessageActionAbortKey(
					MTP_long(exchangeId)));
			return;
		}
		auto &rk = _rekeys[id];
		rk.stage = Rekey::Stage::Accepted;
		rk.exchangeId = exchangeId;
		rk.randomPower = first.randomPower;
		rk.newKey = computed->key;
		rk.newKeyFingerprint = computed->fingerprint;
		rk.haveNewKey = true;
		sendServiceAction(
			chat,
			decrypted::MTP_decryptedMessageActionAcceptKey(
				MTP_long(exchangeId),
				MTP_bytes(first.modexp),
				MTP_long(computed->fingerprint)));
		DEBUG_LOG(("Secret Chat: rekey accepted for chat %1, exchange_id %2."
			).arg(id).arg(exchangeId));
	});
}

void EncryptedChats::handleAcceptKey(
		not_null<SecretChatData*> chat,
		uint64 exchangeId,
		bytes::const_span gB,
		uint64 fingerprint) {
	const auto id = chat->secretChatId();
	const auto i = _rekeys.find(id);
	if (i == _rekeys.end()
		|| i->second.stage != Rekey::Stage::Requested
		|| i->second.exchangeId != exchangeId) {
		// Unknown or stale exchange -> abort it.
		sendServiceAction(
			chat,
			decrypted::MTP_decryptedMessageActionAbortKey(
				MTP_long(exchangeId)));
		return;
	}
	const auto computed = MTP::SecretChat::ComputeKey(
		gB,
		i->second.randomPower,
		_dhConfig.p);
	if (!computed || computed->fingerprint != fingerprint) {
		LOG(("Secret Chat Error: rekey accept fingerprint mismatch."));
		sendServiceAction(
			chat,
			decrypted::MTP_decryptedMessageActionAbortKey(
				MTP_long(exchangeId)));
		_rekeys.erase(i);
		return;
	}
	// Commit: tell the peer (still encrypted with the old key), then switch.
	sendServiceAction(
		chat,
		decrypted::MTP_decryptedMessageActionCommitKey(
			MTP_long(exchangeId),
			MTP_long(computed->fingerprint)));
	commitNewKey(chat, computed->key, computed->fingerprint);
}

void EncryptedChats::handleCommitKey(
		not_null<SecretChatData*> chat,
		uint64 exchangeId,
		uint64 fingerprint) {
	const auto i = _rekeys.find(chat->secretChatId());
	if (i == _rekeys.end()
		|| i->second.stage != Rekey::Stage::Accepted
		|| i->second.exchangeId != exchangeId
		|| !i->second.haveNewKey) {
		return; // Stale/unknown; can't abort after acceptKey, so just ignore.
	} else if (i->second.newKeyFingerprint != fingerprint) {
		LOG(("Secret Chat Error: rekey commit fingerprint mismatch."));
		return; // Do not switch to a key the peer disagrees about.
	}
	commitNewKey(chat, i->second.newKey, i->second.newKeyFingerprint);
}

void EncryptedChats::handleAbortKey(
		not_null<SecretChatData*> chat,
		uint64 exchangeId) {
	const auto i = _rekeys.find(chat->secretChatId());
	if (i != _rekeys.end() && i->second.exchangeId == exchangeId) {
		_rekeys.erase(i); // Drop the pending exchange; it will never commit.
		DEBUG_LOG(("Secret Chat: rekey aborted for chat %1, exchange_id %2."
			).arg(chat->secretChatId()).arg(exchangeId));
	}
}

void EncryptedChats::commitNewKey(
		not_null<SecretChatData*> chat,
		bytes::const_span newKey,
		uint64 fingerprint) {
	// setKey() copies the bytes and resets the key age/use count, so the new
	// key's PFS clock starts now.
	chat->setKey(newKey, fingerprint);
	_rekeys.remove(chat->secretChatId());
	writeLocal();
	DEBUG_LOG(("Secret Chat: rekey committed for chat %1, new fingerprint %2."
		).arg(chat->secretChatId()).arg(QString::number(fingerprint, 16)));
}

void EncryptedChats::ensureItemRemovedWatch() {
	if (_itemRemovedWatching) {
		return;
	}
	_itemRemovedWatching = true;
	// A secret-chat message destroyed at runtime -- most importantly by its
	// self-destruct (TTL) timer via Data::Session::registerMessageTTL -- must be
	// dropped from our random_id/ttl indices and the persisted blob re-written;
	// otherwise it reappears on the next launch (restoreMessages rebuilds it from
	// a stale blob, then it re-arms its past destroy time and only flashes away).
	_session->data().itemRemoved(
	) | rpl::on_next([=](not_null<const HistoryItem*> item) {
		const auto peerId = item->history()->peer->id;
		const auto msgId = item->id;
		if (!unregisterRandomId(peerId, msgId)) {
			return; // Not a tracked secret-chat message.
		}
		if (const auto k = _messageTtls.find(peerId); k != _messageTtls.end()) {
			k->second.remove(msgId);
			if (k->second.empty()) {
				_messageTtls.erase(k);
			}
		}
		writeMessagesLocal();
	}, _lifetime);
}

void EncryptedChats::ensureInDialogs(not_null<SecretChatData*> chat) {
	if (chat->state() == SecretChatState::Discarded) {
		// Never resurrect a closed chat into the dialog list: a late/replayed
		// update for an already-discarded chat must not re-add its row.
		return;
	}
	ensureItemRemovedWatch();
	const auto history = _session->data().history(chat->id);
	if (!history->folderKnown()) {
		// Marks the folder known (top-level), which inserts the history into
		// the chat list -- secret chats never receive a server dialog.
		history->clearFolder();
	}
	if (!history->chatListTimeId()) {
		// An empty history has chatListTimeId() == 0, which yields sort key 0
		// and Entry::setChatListExistence() then refuses to add it. Give the
		// chat a date so it shows in the list before the first message.
		history->setChatListTimeId(base::unixtime::now());
	}
	if (!history->unreadCountKnown()) {
		// Secret chats get no server dialog, so the unread count is never known
		// from the server. Seed it to 0 (folder is known above, as setUnreadCount
		// requires) so newItemAdded increments it for incoming messages instead of
		// firing a doomed requestDialogEntry on a peer with no InputPeer.
		history->setUnreadCount(0);
	}
}

} // namespace Api
