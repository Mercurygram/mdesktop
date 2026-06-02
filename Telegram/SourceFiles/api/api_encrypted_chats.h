/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/bytes.h"
#include "base/flat_map.h"
#include "data/data_msg_id.h"
#include "mtproto/mtproto_auth_key.h"
#include "mtproto/sender.h"

#include "secret_scheme.h"

#include <map>
#include <optional>
#include <vector>

class ApiWrap;
class UserData;
class SecretChatData;
class DocumentData;
class HistoryItem;
struct FilePrepareResult;

namespace Main {
class Session;
} // namespace Main

namespace Storage {
struct UploadSecureDone;
struct UploadSecureProgress;
} // namespace Storage

namespace Api {

// Human-readable self-destruct duration ("5 seconds", "1 week", ...) for a
// positive number of seconds.
[[nodiscard]] QString SecretChatTtlDuration(int seconds);

// Drives the MTProto secret (end-to-end encrypted) chat protocol: the
// Diffie-Hellman handshake, key agreement, and sending / receiving of
// encrypted messages. Owned by ApiWrap.
class EncryptedChats final {
public:
	explicit EncryptedChats(not_null<ApiWrap*> api);
	~EncryptedChats();

	// Start requesting a new secret chat with the given user.
	void create(not_null<UserData*> user);

	// Accept an incoming secret chat request (state == Waiting).
	void accept(not_null<SecretChatData*> chat);

	// Close a secret chat.
	void discard(not_null<SecretChatData*> chat, bool deleteHistory);

	// The configured self-destruct lifetime (seconds) of a pending secret
	// message, or 0 if it has none (or its timer already started). Used to
	// keep short-lived self-destruct media out of the shared-media index.
	[[nodiscard]] int32 messageTtlSeconds(PeerId chatId, MsgId msgId) const;

	// Report the secret chat as spam, then discard it (deleting the history).
	void reportSpam(not_null<SecretChatData*> chat);

	// Send a plain text message into a ready secret chat.
	void sendText(not_null<SecretChatData*> chat, const QString &text);

	// Encrypt + upload a prepared file and send it into a ready secret chat.
	// Routed here from Api::SendConfirmedFile when the target is a secret chat.
	void sendFile(
		not_null<SecretChatData*> chat,
		FullMsgId itemId,
		const std::shared_ptr<FilePrepareResult> &file);

	// Send an existing server document (a sticker or GIF) into a ready secret
	// chat as decryptedMessageMediaExternalDocument -- it references the file on
	// the server (id/access_hash/dc), with no encrypted upload. Creates the local
	// outgoing bubble itself. Routed here from Api::SendExistingDocument.
	void sendExistingDocument(
		not_null<SecretChatData*> chat,
		not_null<DocumentData*> document,
		const QString &caption,
		bool afterSetRefetch = false);

	// A sticker picked from Recent/Faved keeps no originating set locally (the
	// desktop cache stores special-set members with an empty set -- unlike the
	// Android client, which keeps the inputStickerSetID per document). When the
	// set cannot be resolved we force-fetch recent + faved stickers from the
	// server (their documents carry the real set id) and retry once.
	void refetchRecentAndFavedSets(Fn<void()> then);

	// Build + send the external-document message once the sticker set's short
	// name is known (the secret layer references a set only by short name; an
	// empty set leaves the peer unable to fetch the file). sendExistingDocument
	// resolves the name -- fetching the set first if it is not loaded -- then
	// calls this.
	void sendExternalDocument(
		not_null<SecretChatData*> chat,
		not_null<DocumentData*> document,
		const QString &caption,
		const QString &shortName);

	// Fallback for a sticker whose set cannot be resolved (e.g. a faved sticker
	// from a deleted set): the external-document reference would be unfetchable
	// by the peer (no file_reference, set gone), so upload the actual sticker
	// file as an encrypted document with the sticker attributes. Waits for the
	// sticker file to be loaded, then routes through uploadStickerDocument.
	void sendStickerAsDocument(
		not_null<SecretChatData*> chat,
		not_null<DocumentData*> document,
		const QString &caption);
	void uploadStickerDocument(
		not_null<SecretChatData*> chat,
		not_null<DocumentData*> document,
		const QByteArray &plain,
		const QString &caption,
		FullMsgId existing = {});

	// A GIF/animation (or any non-sticker existing document) cannot be sent as an
	// external reference (no file_reference -> the peer can't fetch it), so the
	// actual bytes are downloaded and re-uploaded encrypted, mirroring the Android
	// client. Waits for the file to be loaded, then uploads via uploadDocumentFile.
	void uploadExistingDocument(
		not_null<SecretChatData*> chat,
		not_null<DocumentData*> document,
		const QString &caption);
	void uploadDocumentFile(
		not_null<SecretChatData*> chat,
		not_null<DocumentData*> document,
		const QByteArray &plain,
		const QString &caption,
		FullMsgId existing = {});

	void setTyping(not_null<SecretChatData*> chat, bool typing);
	void readHistory(not_null<SecretChatData*> chat, TimeId tillDate);

	// Set the chat's self-destruct timer (seconds; 0 = off): persist it locally
	// and notify the partner via decryptedMessageActionSetMessageTTL. Subsequent
	// messages then carry this ttl and self-destruct once read.
	void setSelfDestructTimer(not_null<SecretChatData*> chat, int seconds);

	// Clear the chat history on the partner's device too, via
	// decryptedMessageActionFlushHistory. Called when the user clears history
	// locally for a secret chat (see ApiWrap::clearHistory).
	void flushHistory(not_null<SecretChatData*> chat);

	// Delete messages on both sides via a decryptedMessageActionDeleteMessages
	// service action. `localIds` are the local message ids in this secret chat
	// (these are client-side MsgIds above ServerMaxMsgId -- 64-bit, never narrow
	// them to int32).
	void deleteMessages(
		not_null<SecretChatData*> chat,
		const std::vector<MsgId> &localIds);

	// Rebuild locally persisted message bubbles into their histories on
	// startup. Called by Storage::Account::readSecretChatMessages with the
	// blob produced by serializeMessages().
	void restoreMessages(const QByteArray &serialized);

	// Handlers wired from Api::Updates.
	void processUpdate(const MTPEncryptedChat &chat);
	void newMessage(const MTPEncryptedMessage &message, int32 qts);
	void messagesRead(int32 chatId, TimeId maxDate);
	void chatTyping(int32 chatId);

	// qts checkpoint for secret-chat (encrypted) updates. Secret messages ride
	// the qts sequence (separate from pts/seq); persisting it lets us pull the
	// offline gap via updates.getDifference on the next launch. setQts advances
	// the checkpoint, persists it, and ACKs the server (messages.receivedQueue)
	// so already-delivered encrypted updates stop being re-sent.
	[[nodiscard]] int32 qts() const {
		return _qts;
	}
	void setQts(int32 qts);
	// Restore the checkpoint from local storage at startup (no persist / no ACK).
	void restoreQts(int32 qts) {
		_qts = qts;
	}

	// Re-render this chat's self-destruct timer notices (called by SecretChatData::
	// mirrorUser when the partner name resolves, so a restored notice picks up the
	// real name exactly when the dialog row does). No-op if the chat has no notices.
	void refreshTtlNotices(not_null<SecretChatData*> chat);

private:
	struct DhConfig {
		int32 version = -1;
		int32 g = 0;
		bytes::vector p;
	};
	// In-flight handshake material kept until the key is computed.
	struct Pending {
		bytes::vector randomPower; // our secret exponent (a or b)
		bytes::vector gA; // the requester's g_a, for the accepting side
	};
	// An incoming encrypted file being downloaded part-by-part from its dc.
	// Held alive by the in-flight request callbacks; decrypted on completion.
	struct IncomingFile {
		int32 chatId = 0;
		uint64 fileId = 0;
		uint64 accessHash = 0;
		int32 dcId = 0;
		int64 ciphertextSize = 0; // padded size stored on the server
		bytes::array<32> key = { { gsl::byte{} } };
		bytes::array<32> iv = { { gsl::byte{} } };
		int64 plaintextSize = 0; // real size from the decrypted media
		QString mime;
		QString filename;
		// Document attributes for the rebuilt local document (image size /
		// video / audio); drive inline photo/video/voice rendering. Empty ->
		// a plain filename attribute is synthesized on completion.
		QVector<MTPDocumentAttribute> attributes;
		// True for decryptedMessageMediaPhoto: rebuilt as a local PhotoData so it
		// renders inline (MediaFile has no isImage->Photo path), not a document.
		bool asPhoto = false;
		QString caption;
		TimeId date = 0;
		uint64 randomId = 0; // the decrypted message's random_id
		// Unencrypted inline preview thumbnail carried alongside the media (JPEG
		// bytes + dimensions); shown immediately while the encrypted file
		// downloads. Empty for plain files / voice notes that carry no thumb.
		QByteArray thumbBytes;
		int thumbWidth = 0;
		int thumbHeight = 0;
		// Per-message self-destruct timer in seconds (decryptedMessage.ttl); 0 =
		// no self-destruct. Noted on the created item so it expires once read.
		int32 ttl = 0;
		// The local message created up-front to show the thumb during download
		// (0 if none -- then the bubble is built once the file finishes).
		MsgId pendingItemId = 0;
		QByteArray data; // accumulated ciphertext
		int64 offset = 0;
	};
	// In-flight Perfect-Forward-Secrecy rekey exchange (the requestKey /
	// acceptKey / commitKey handshake). Kept until the new key is committed.
	struct Rekey {
		enum class Stage : uchar {
			Requested, // we initiated; sent requestKey, awaiting acceptKey
			Accepted,  // we responded; sent acceptKey, awaiting commitKey
		};
		Stage stage = Stage::Requested;
		uint64 exchangeId = 0;
		bytes::vector randomPower; // our a (Requested) or b (Accepted)
		MTP::AuthKey::Data newKey = { { gsl::byte{} } };
		uint64 newKeyFingerprint = 0;
		bool haveNewKey = false; // true once newKey/newKeyFingerprint are set
	};
	// An encrypted file whose upload is in progress; once the uploader reports
	// the file id + parts we build decryptedMessageMediaDocument and send it.
	struct OutgoingFile {
		int32 chatId = 0;
		bytes::array<32> key = { { gsl::byte{} } };
		bytes::array<32> iv = { { gsl::byte{} } };
		int32 keyFingerprint = 0;
		QString md5; // hex md5 of the ciphertext (inputEncryptedFileUploaded)
		uint64 randomId = 0;
		QString mime;
		QString filename;
		int64 size = 0; // real (unpadded) plaintext size
		QString caption;
		// True for an image sent as a photo: build decryptedMessageMediaPhoto so
		// the peer renders it inline, not as a file. width/height are the full
		// image size; thumb is a small unencrypted JPEG preview with its size.
		bool asPhoto = false;
		int32 width = 0;
		int32 height = 0;
		int32 thumbWidth = 0;
		int32 thumbHeight = 0;
		QByteArray thumb;
		// True when uploading a sticker whose set could not be resolved: send it
		// as a decryptedMessageMediaDocument carrying the sticker + image-size
		// attributes (the peer renders it as a sticker from the file alone, with
		// no set reference needed). stickerAlt is the sticker's emoji.
		bool asSticker = false;
		QString stickerAlt;
		// True for a GIF/animation re-uploaded as an encrypted document: send it
		// as a decryptedMessageMediaDocument carrying documentAttributeVideo +
		// documentAttributeAnimated (mime video/mp4) so the peer auto-plays it.
		// An external reference carries no file_reference, so a non-sticker doc
		// cannot be fetched that way -- the bytes must travel encrypted.
		bool asAnimation = false;
		// True for a regular (non-animated, non-round) video: send it as a
		// decryptedMessageMediaVideo (thumb + w/h/duration) so the peer renders an
		// inline playable video instead of a plain file. Reuses width/height/thumb.
		bool asVideo = false;
		// True for a round video note: send a decryptedMessageMediaDocument with
		// documentAttributeVideo(round_message) so the peer renders a round video
		// bubble instead of a file. Reuses width/height/thumb + the duration field.
		bool asRoundVideo = false;
		// True for a voice note: send a decryptedMessageMediaDocument carrying
		// documentAttributeAudio(voice) so the peer renders a voice bubble instead
		// of a plain "audio_*.ogg" file row. Reuses the duration field.
		bool asVoice = false;
		int32 duration = 0;
		// True when the ciphertext exceeds kUseBigFilesFrom: it was uploaded via
		// upload.saveBigFilePart and must be referenced with
		// inputEncryptedFileBigUploaded (no md5 checksum) instead of
		// inputEncryptedFileUploaded.
		bool bigFile = false;
		// Local on-disk copy of the plaintext (secret_files/...) for a document
		// bubble: linked to the document only once the upload+send completes, so
		// during the upload the bubble shows a progress radial, not an openable
		// file. Empty for photos/stickers (which manage their own bubble).
		QString localPath;
		// Ciphertext size, used as the document upload-progress denominator.
		int64 cipherSize = 0;
	};
	// A decrypted incoming message that arrived out of order (its out_seq_no is
	// ahead of the next expected one). Held until the gap before it is filled by
	// resent messages, then replayed in seq order -- mirrors Android's
	// secretHolesQueue / checkSecretHoles.
	struct BufferedMessage {
		decrypted::MTPDecryptedMessage message;
		std::optional<MTPEncryptedFile> file;
		TimeId date = 0;
		int32 outSeqNo = 0;
	};
	// A previously sent decrypted-layer payload, kept so we can honour a peer's
	// decryptedMessageActionResend: the bytes already carry the original in/out
	// seq, so re-encrypting (with the current key) and re-sending reproduces the
	// exact message. randomId is the outer messages.sendEncrypted* random_id so
	// the peer de-duplicates the resend against the original.
	struct SentLayer {
		bytes::vector serialized;
		uint64 randomId = 0;
		bool isService = false;
	};

	void ensureDhConfig(Fn<void()> done);
	[[nodiscard]] bool dhConfigReady() const;

	SecretChatData *applyUpdateChat(const MTPEncryptedChat &chat);
	[[nodiscard]] SecretChatData *applyChat(const MTPDencryptedChat &data);
	[[nodiscard]] SecretChatData *applyRequested(
		const MTPDencryptedChatRequested &data);
	[[nodiscard]] SecretChatData *applyWaiting(
		const MTPDencryptedChatWaiting &data);

	[[nodiscard]] MTPInputEncryptedChat inputChat(
		not_null<SecretChatData*> chat) const;

	// Tear down a closed secret chat locally: mark it Discarded, drop it from the
	// dialog list, and clear all per-chat in-memory state (seq/hole/rekey/ttl/
	// random-id maps). When `clearHistory` is set the local message history is
	// wiped too. Shared by the user-initiated discard() and the incoming
	// encryptedChatDiscarded handler (which clears history iff history_deleted).
	void clearLocalState(
		not_null<SecretChatData*> chat,
		bool clearHistory);

	void addDecryptedMessage(
		not_null<SecretChatData*> chat,
		const QByteArray &decrypted,
		TimeId date,
		bool outgoing,
		uint64 randomId = 0,
		int32 ttlSeconds = 0);

	// Adds the in-chat service notice for a self-destruct timer change (outgoing =
	// we changed it; otherwise the partner did). seconds == 0 means disabled. The
	// notice is a centered service message (matching official clients) and is
	// persisted (StoredKind::ServiceTtl) so it re-renders on restart.
	void addTtlChangeNotice(
		not_null<SecretChatData*> chat,
		int seconds,
		TimeId date,
		bool outgoing);
	// Builds (and inserts) the service-message HistoryItem for a ttl change; shared
	// by the live path and restore. Returns the inserted item.
	not_null<HistoryItem*> buildTtlServiceMessage(
		not_null<SecretChatData*> chat,
		int seconds,
		TimeId date,
		bool outgoing,
		MsgId id,
		TextWithEntities text = {});
	[[nodiscard]] TextWithEntities ttlNoticeText(
		not_null<SecretChatData*> chat,
		int seconds,
		bool outgoing) const;
	// Re-renders all restored ttl notices once the partner name loads (a minimal
	// partner bakes the "Secret chat" fallback at restore time otherwise).
	void watchTtlNoticeNames(not_null<SecretChatData*> chat);

	// Per-message self-destruct (TTL) book-keeping. noteMessageTtl records a
	// message's ttl (seconds) until it is read; startSelfDestructTimers arms the
	// owner's message-TTL timer for every read message of the given direction up
	// to tillDate (incoming when we view the chat, outgoing when the partner
	// reads), then drops it from the pending map (the timer now lives on the
	// item). See core.telegram.org/api/secret -- the timer starts on read.
	void noteMessageTtl(PeerId chatId, MsgId msgId, int32 ttlSeconds);
	void startSelfDestructTimers(
		not_null<SecretChatData*> chat,
		bool outgoing,
		TimeId tillDate);
	// Handle the in-content decryptedMessageActionReadMessages: the partner
	// read the listed (random_id-keyed) outgoing messages, so mark them read
	// and start their self-destruct timers, as messagesRead() does by date.
	void handleReadMessages(
		not_null<SecretChatData*> chat,
		const QVector<MTPlong> &randomIds);

	// Serialize, encrypt and send a service action (decryptedMessageService)
	// into a ready secret chat. Advances out_seq_no and persists.
	void sendServiceAction(
		not_null<SecretChatData*> chat,
		const decrypted::MTPDecryptedMessageAction &action);

	// Announce our protocol layer to the partner via
	// decryptedMessageActionNotifyLayer as soon as the chat becomes Ready, so the
	// peer does not fall back to the minimum layer and downgrade features.
	void sendNotifyLayer(not_null<SecretChatData*> chat);

	// Queue a send issued before the chat is Ready (Requested/Waiting), to be
	// replayed by flushPendingSends() once the key is established. Returns true if
	// the action was queued (caller must stop), false if the chat is Ready and the
	// caller should proceed normally.
	[[nodiscard]] bool queuePendingSend(
		not_null<SecretChatData*> chat,
		Fn<void()> retry);
	void flushPendingSends(not_null<SecretChatData*> chat);

	// Apply one decrypted layer with seq-no gap / duplicate handling (the
	// reliability layer ported from Android), then replay any buffered
	// out-of-order messages the accepted one unblocks.
	void receiveDecryptedLayer(
		not_null<SecretChatData*> chat,
		const decrypted::MTPDdecryptedMessageLayer &data,
		TimeId date,
		const MTPEncryptedFile *encryptedFile);

	// Dispatch one decrypted message (content or service action) into the chat:
	// build the local bubble, start a media download, or apply a service action.
	// Shared by the live receive path and the out-of-order replay (drainHoles).
	void processDecryptedMessage(
		not_null<SecretChatData*> chat,
		const decrypted::MTPDecryptedMessage &message,
		TimeId date,
		const MTPEncryptedFile *encryptedFile);

	// Sequence-number reliability (mirrors Android SecretChatHelper): on a gap we
	// ask the peer to resend the missing range and buffer the newer message; we
	// honour the peer's resend requests from a small per-chat cache of sent
	// layers; too many unfilled holes discard the chat.
	void rememberSentLayer(
		int32 chatId,
		int32 outSeqNo,
		bytes::const_span serialized,
		uint64 randomId,
		bool isService);
	void drainHoles(not_null<SecretChatData*> chat);
	void sendResendRequest(
		not_null<SecretChatData*> chat,
		int32 startSeqNo,
		int32 endSeqNo);
	void handleResend(
		not_null<SecretChatData*> chat,
		int32 startSeqNo,
		int32 endSeqNo);
	void resendSentLayer(
		not_null<SecretChatData*> chat,
		const SentLayer &layer);
	void resendTombstone(not_null<SecretChatData*> chat, int32 outSeqNo);

	// Perfect-Forward-Secrecy rekeying (decryptedMessageActionRequestKey /
	// AcceptKey / CommitKey / AbortKey). Triggered after kRekeyEvery messages or
	// kRekeyAfter time on the current key; see core.telegram.org/api/end-to-end/pfs.
	void maybeStartRekey(not_null<SecretChatData*> chat);
	void startRekey(not_null<SecretChatData*> chat);
	void handleRequestKey(
		not_null<SecretChatData*> chat,
		uint64 exchangeId,
		bytes::const_span gA);
	void handleAcceptKey(
		not_null<SecretChatData*> chat,
		uint64 exchangeId,
		bytes::const_span gB,
		uint64 fingerprint);
	void handleCommitKey(
		not_null<SecretChatData*> chat,
		uint64 exchangeId,
		uint64 fingerprint);
	void handleAbortKey(
		not_null<SecretChatData*> chat,
		uint64 exchangeId);
	// Switch the chat to a committed new key and clear the exchange state.
	void commitNewKey(
		not_null<SecretChatData*> chat,
		bytes::const_span newKey,
		uint64 fingerprint);

	// Add the local outgoing bubble for an (existing) document immediately, before
	// the bytes are resolved/encrypted, so a burst of sends shows up on click
	// instead of all at once at the end. Returns the new message's id.
	[[nodiscard]] FullMsgId addLocalDocumentBubble(
		not_null<SecretChatData*> chat,
		not_null<DocumentData*> document,
		const QString &caption);

	// Run `trySend` now; if the content is not in memory yet (returns false),
	// retry on every downloaderTaskFinished until it succeeds, with a leak-safe
	// self-owning subscription bounded by our lifetime.
	void sendWhenDownloaded(Fn<bool()> trySend);

	// Shared body of uploadStickerDocument / uploadDocumentFile: encrypt the
	// plaintext, queue the upload, and link it to the local bubble. `existing` is
	// the bubble added up front by addLocalDocumentBubble; when unset the bubble is
	// created here. `applyKind` fills in the media-kind-specific OutgoingFile
	// fields (sticker vs animation).
	void uploadEncryptedDocument(
		not_null<SecretChatData*> chat,
		not_null<DocumentData*> document,
		const QByteArray &plain,
		const QString &caption,
		int width,
		int height,
		Fn<void(OutgoingFile&)> applyKind,
		FullMsgId existing = {});
	// Subscribe to the uploader's secret-file signals exactly once.
	void ensureUploadSubscribed();

	// Map a secret-chat message's random_id <-> its local MsgId, so service
	// actions (delete/read) addressed by random_id can find the local message.
	// Both directions are indexed (_messageRandomIds / _randomIdToMsg), so a
	// lookup by random_id is O(log n) instead of a full scan; all mutations go
	// through these three helpers to keep the two indices in lockstep.
	void registerRandomId(PeerId chatId, MsgId msgId, uint64 randomId);
	std::optional<uint64> unregisterRandomId(PeerId chatId, MsgId msgId);
	void clearChatRandomIds(PeerId chatId);
	[[nodiscard]] MsgId findByRandomId(PeerId chatId, uint64 randomId) const;

	// Wired to the uploader: completes an in-flight secret-chat file send.
	void fileUploadDone(const Storage::UploadSecureDone &done);
	void fileUploadFailed(FullMsgId itemId);
	void fileUploadProgress(const Storage::UploadSecureProgress &data);

	// Incoming file download: request parts until complete, then decrypt and
	// add a local document message.
	void startFileDownload(std::shared_ptr<IncomingFile> state);
	void createPendingMediaItem(std::shared_ptr<IncomingFile> state);
	void finishFileDownload(std::shared_ptr<IncomingFile> state);

	// At-rest encryption for decrypted secret-chat media: stored under
	// tdata/secret_files/ encrypted with the local key (Storage::File), never as
	// plaintext. Read back to memory on demand (see SecretFileLoader / restore).
	[[nodiscard]] bool writeSecretFileEncrypted(
		const QString &path,
		const QByteArray &plain);
	[[nodiscard]] QByteArray readSecretFileEncrypted(
		const QString &path,
		int64 plaintextSize);

	// Secret chats get no server dialog, so their History is never marked
	// folder-known and would be excluded from the chat list. Mark it known so
	// it shows up in the dialogs list.
	void ensureInDialogs(not_null<SecretChatData*> chat);

	// Arm (once) the itemRemoved subscription that re-persists when a secret-chat
	// message is destroyed (e.g. its self-destruct timer fires).
	void ensureItemRemovedWatch();

	// Persist secret-chat keys and state (the server stores none of this).
	void writeLocal();

	// Serialize / persist the local message bubbles of all secret chats.
	// serializeMessages() walks each chat's _messageRandomIds index (every
	// persistable message is registered there) and dumps text + document
	// messages; writeMessagesLocal() hands the blob to Storage::Account.
	[[nodiscard]] QByteArray serializeMessages() const;
	void writeMessagesLocal();

	const not_null<ApiWrap*> _api;
	const not_null<Main::Session*> _session;
	MTP::Sender _mtp;

	DhConfig _dhConfig;
	// Chains each outgoing messages.sendEncrypted{,File,Service} after the
	// previous one for the same chat (invokeAfterMsg) so the server delivers
	// them in out_seq_no order. Concurrent sends would otherwise complete out of
	// order, and a rekey/service action arriving with seq gaps makes the peer
	// discard the chat. Keyed by secretChatId; holds the last request id.
	[[nodiscard]] mtpRequestId sendAfter(int32 chatId) const;
	void setSendAfter(int32 chatId, mtpRequestId requestId);

	base::flat_map<int32, Pending> _pending;
	base::flat_map<int32, Rekey> _rekeys; // keyed by secretChatId
	base::flat_map<int32, mtpRequestId> _sendAfter; // keyed by secretChatId
	// Sends issued before the chat became Ready (e.g. right after creating it,
	// while still Requested/Waiting for the peer to accept). Queued in order and
	// flushed by flushPendingSends() once the key is established, so the first
	// message is not silently dropped. Keyed by secretChatId.
	base::flat_map<int32, std::vector<Fn<void()>>> _pendingSends;
	// Out-of-order incoming messages awaiting the gap before them (keyed by
	// secretChatId). Capped at kMaxHoles entries; overflowing discards the chat.
	base::flat_map<int32, std::vector<BufferedMessage>> _holes;
	// Recently sent decrypted-layer payloads (keyed by secretChatId, then by
	// out_seq_no) for answering decryptedMessageActionResend. Bounded per chat.
	base::flat_map<int32, std::map<int32, SentLayer>> _sentLayers;
	base::flat_map<FullMsgId, OutgoingFile> _outgoingFiles;
	base::flat_map<PeerId, base::flat_map<MsgId, uint64>> _messageRandomIds;
	// Reverse index of the above (random_id -> MsgId), kept in lockstep by
	// register/unregister/clearChatRandomIds so findByRandomId is O(log n).
	base::flat_map<PeerId, base::flat_map<uint64, MsgId>> _randomIdToMsg;
	// Pending self-destruct timers: msgId -> ttl seconds, for messages not yet
	// read. Cleared once the timer is armed (then it lives on the HistoryItem).
	base::flat_map<PeerId, base::flat_map<MsgId, int32>> _messageTtls;
	// Self-destruct timer change notices (service messages): msgId -> ttl seconds
	// at the time of the change (0 = disabled). Drives StoredKind::ServiceTtl so the
	// notice is re-rendered (not a plain text bubble) on restore.
	base::flat_map<PeerId, base::flat_map<MsgId, int32>> _ttlNotices;
	// One Name-update subscription per chat (keyed by secretChatId) that re-renders
	// that chat's ttl notices when the partner name resolves.
	base::flat_map<int32, rpl::lifetime> _ttlNoticeNameWatch;
	base::flat_map<int32, TimeId> _readTillDates; // keyed by secretChatId
	// Highest qts of any processed encrypted update; persisted with the chats so
	// the offline gap can be fetched on the next launch. 0 = no baseline yet.
	int32 _qts = 0;
	bool _uploadSubscribed = false;
	// Lazily-armed (data() isn't ready in the ctor): when a secret-chat item is
	// destroyed -- notably by the self-destruct (TTL) timer -- drop it from the
	// random_id/ttl maps and re-persist so it does not survive the next restart.
	bool _itemRemovedWatching = false;
	rpl::lifetime _lifetime;

};

} // namespace Api
