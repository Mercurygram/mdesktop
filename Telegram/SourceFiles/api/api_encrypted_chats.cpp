/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "api/api_encrypted_chats.h"

#include "api/api_common.h"
#include "api/api_sending.h"
#include "apiwrap.h"
#include "main/main_session.h"
#include "data/data_session.h"
#include "data/data_changes.h"
#include "data/data_user.h"
#include "data/data_secret_chat.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/stickers/data_stickers.h"
#include "data/stickers/data_stickers_set.h"
#include "data/stickers/data_custom_emoji.h"
#include "data/data_send_action.h"
#include "data/data_media_types.h"
#include "data/data_file_origin.h"
#include "data/data_location.h"
#include "ui/basic_click_handlers.h"
#include "ui/image/image.h"
#include "ui/text/text_options.h"
#include "ui/image/image_location_factory.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_helpers.h"
#include "mtproto/mtproto_dh_utils.h"
#include "mtproto/mtproto_auth_key.h"
#include "mtproto/facade.h"
#include "mtproto/secret_chat/secret_chat_dh.h"
#include "mtproto/secret_chat/secret_chat_encryption.h"
#include "base/call_delayed.h"
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
#include "storage/cache/storage_cache_database.h"
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
// A failed part request of an incoming file is retried this many times with
// a growing delay; after that the download waits for the next launch, which
// starts it over from the persisted message. No error classification: a
// permanent error costs kDownloadRetries requests per launch.
constexpr auto kDownloadRetries = 3;
constexpr auto kDownloadRetryDelay = crl::time(5000);

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
	// Same quality as the Android client's secret-chat thumbs (90px, q55).
	scaled.save(&buffer, "JPG", 55);
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

// Incoming text / captions: a RIGHT-TO-LEFT OVERRIDE (U+202E) lets a peer
// visually reverse the rest of the line (e.g. disguise a file name). Replace
// it with a space, 1:1, so entity offsets stay valid; same as Android.
[[nodiscard]] QString StripRtlOverride(QString text) {
	return text.replace(QChar(0x202E), QChar(' '));
}

// Add the link / mention / hashtag entities a server would have attached: a
// secret message carries only the formatting the sender chose (Android
// linkifies at render time; the persisted tags never include plain links),
// and the item renderer takes links from entities alone. Existing entities
// are kept, the parser only fills the gaps between them.
[[nodiscard]] TextWithEntities Linkified(TextWithEntities text) {
	TextUtilities::ParseEntities(text, Ui::ItemTextDefaultOptions().flags);
	return text;
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

// Bounds for the unencrypted preview thumb a peer attaches to a media message
// (Android drops a photo/video thumb above 6000 bytes and a document thumb
// above 20000, both at most 100px a side). Anything larger is dropped before
// it reaches QImage::fromData. We send 90px ourselves, so the size bound is
// safe for every kind.
[[nodiscard]] QByteArray AcceptedThumb(
		const QByteArray &thumb,
		int width,
		int height,
		int maxBytes) {
	return (thumb.size() > maxBytes || width > 100 || height > 100)
		? QByteArray()
		: thumb;
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
// The best already-decoded preview of a document, for the unencrypted thumb of
// an outgoing secret-chat media. Prefer the inline (stripped) preview that
// ships WITH the document: it decodes synchronously with no download, exactly
// like the photoStrippedSize the Android client embeds
// (SendMessagesHelper.getThumbForSecretChat). Only fall back to an already-
// decoded raster if there is no inline preview -- never to a still-loading
// image (its original() is empty, which would yield an empty thumb). Null when
// nothing is decoded (voice notes, plain files).
[[nodiscard]] QImage DocumentThumbImage(not_null<DocumentData*> document) {
	auto image = QImage();
	const auto view = document->createMediaView();
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
	return image;
}

[[nodiscard]] decrypted::MTPPhotoSize ExternalDocumentThumb(
		not_null<DocumentData*> document) {
	// A non-empty preview also keeps the peer from showing a "File" during
	// its sticker-set verification window.
	const auto prepared = MakePhotoThumb(DocumentThumbImage(document));
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

// The secret layer's own sticker set reference (short name / empty) or the
// api.tl one the Android client copies verbatim (id + access_hash).
[[nodiscard]] MTPInputStickerSet ConvertSet(
		const decrypted::MTPInputStickerSet &set) {
	return set.match([](const decrypted::MTPDinputStickerSetID &d) {
		return MTP_inputStickerSetID(d.vid(), d.vaccess_hash());
	}, [](const decrypted::MTPDinputStickerSetShortName &d) {
		return MTP_inputStickerSetShortName(d.vshort_name());
	}, [](const decrypted::MTPDinputStickerSetEmpty &) {
		return MTP_inputStickerSetEmpty();
	});
}

[[nodiscard]] bool IsInternalUrl(const QString &url) {
	return url.startsWith(u"internal:"_q, Qt::CaseInsensitive);
}

// Api::EntitiesToMTP cannot be reused: the secret layer's entities are a
// separate generated namespace and its blockquote has no collapsed flag.
// Only the formatting a composer produces goes out (same set as the normal
// path's SkipLocal); links / mentions / hashtags are re-parsed by the peer.
[[nodiscard]] auto EntitiesToDecrypted(const EntitiesInText &entities)
-> decrypted::MTPVector<decrypted::MTPMessageEntity> {
	auto v = QVector<decrypted::MTPMessageEntity>();
	v.reserve(entities.size());
	for (const auto &entity : entities) {
		if (entity.length() <= 0) {
			continue;
		}
		const auto offset = MTP_int(entity.offset());
		const auto length = MTP_int(entity.length());
		switch (entity.type()) {
		case EntityType::Bold:
			v.push_back(decrypted::MTP_messageEntityBold(offset, length));
			break;
		case EntityType::Italic:
			v.push_back(decrypted::MTP_messageEntityItalic(offset, length));
			break;
		case EntityType::Underline:
			v.push_back(decrypted::MTP_messageEntityUnderline(offset, length));
			break;
		case EntityType::StrikeOut:
			v.push_back(decrypted::MTP_messageEntityStrike(offset, length));
			break;
		case EntityType::Code:
			v.push_back(decrypted::MTP_messageEntityCode(offset, length));
			break;
		case EntityType::Pre:
			v.push_back(decrypted::MTP_messageEntityPre(
				offset,
				length,
				MTP_string(entity.data())));
			break;
		case EntityType::Blockquote:
			v.push_back(decrypted::MTP_messageEntityBlockquote(offset, length));
			break;
		case EntityType::Spoiler:
			v.push_back(decrypted::MTP_messageEntitySpoiler(offset, length));
			break;
		case EntityType::CustomEmoji:
			if (const auto id = Data::ParseCustomEmojiData(entity.data())) {
				v.push_back(decrypted::MTP_messageEntityCustomEmoji(
					offset,
					length,
					MTP_long(id)));
			}
			break;
		case EntityType::CustomUrl: {
			// Never let an internal: link leave the client (as EntitiesToMTP).
			const auto external = UrlClickHandler::ExternalUrlFromInternalUrl(
				entity.data());
			const auto url = external.isEmpty() ? entity.data() : external;
			if (!IsInternalUrl(url)) {
				v.push_back(decrypted::MTP_messageEntityTextUrl(
					offset,
					length,
					MTP_string(url)));
			}
		} break;
		default:
			break;
		}
	}
	return MTP_vector<decrypted::MTPMessageEntity>(std::move(v));
}

[[nodiscard]] EntitiesInText EntitiesFromDecrypted(
		not_null<Main::Session*> session,
		const QVector<decrypted::MTPMessageEntity> &entities) {
	auto result = EntitiesInText();
	result.reserve(entities.size());
	const auto push = [&](EntityType type, const auto &d) {
		result.push_back({ type, d.voffset().v, d.vlength().v });
	};
	const auto mentionName = [&](uint64 userId, uint64 accessHash) {
		return TextUtilities::MentionNameDataFromFields({
			.selfId = session->userId().bare,
			.userId = userId,
			.accessHash = accessHash,
		});
	};
	for (const auto &entity : entities) {
		entity.match([&](const decrypted::MTPDmessageEntityUnknown &) {
		}, [&](const decrypted::MTPDmessageEntityMention &d) {
			push(EntityType::Mention, d);
		}, [&](const decrypted::MTPDmessageEntityHashtag &d) {
			push(EntityType::Hashtag, d);
		}, [&](const decrypted::MTPDmessageEntityBotCommand &d) {
			push(EntityType::BotCommand, d);
		}, [&](const decrypted::MTPDmessageEntityUrl &d) {
			push(EntityType::Url, d);
		}, [&](const decrypted::MTPDmessageEntityEmail &d) {
			push(EntityType::Email, d);
		}, [&](const decrypted::MTPDmessageEntityBold &d) {
			push(EntityType::Bold, d);
		}, [&](const decrypted::MTPDmessageEntityItalic &d) {
			push(EntityType::Italic, d);
		}, [&](const decrypted::MTPDmessageEntityCode &d) {
			push(EntityType::Code, d);
		}, [&](const decrypted::MTPDmessageEntityPre &d) {
			result.push_back({
				EntityType::Pre,
				d.voffset().v,
				d.vlength().v,
				qs(d.vlanguage()),
			});
		}, [&](const decrypted::MTPDmessageEntityTextUrl &d) {
			const auto url = qs(d.vurl());
			if (!IsInternalUrl(url)) {
				result.push_back({
					EntityType::CustomUrl,
					d.voffset().v,
					d.vlength().v,
					url,
				});
			}
		}, [&](const decrypted::MTPDmessageEntityUnderline &d) {
			push(EntityType::Underline, d);
		}, [&](const decrypted::MTPDmessageEntityStrike &d) {
			push(EntityType::StrikeOut, d);
		}, [&](const decrypted::MTPDmessageEntityBlockquote &d) {
			push(EntityType::Blockquote, d);
		}, [&](const decrypted::MTPDmessageEntityBlockquoteCollapsible &d) {
			result.push_back({
				EntityType::Blockquote,
				d.voffset().v,
				d.vlength().v,
				d.is_collapsed() ? u"1"_q : QString(),
			});
		}, [&](const decrypted::MTPDmessageEntitySpoiler &d) {
			push(EntityType::Spoiler, d);
		}, [&](const decrypted::MTPDmessageEntityCustomEmoji &d) {
			result.push_back({
				EntityType::CustomEmoji,
				d.voffset().v,
				d.vlength().v,
				Data::SerializeCustomEmojiId(d.vdocument_id().v),
			});
		}, [&](const decrypted::MTPDmessageEntityFormattedDate &d) {
			auto flags = FormattedDateFlags();
			if (d.is_relative()) {
				flags |= FormattedDateFlag::Relative;
			}
			if (d.is_short_time()) {
				flags |= FormattedDateFlag::ShortTime;
			}
			if (d.is_long_time()) {
				flags |= FormattedDateFlag::LongTime;
			}
			if (d.is_short_date()) {
				flags |= FormattedDateFlag::ShortDate;
			}
			if (d.is_long_date()) {
				flags |= FormattedDateFlag::LongDate;
			}
			if (d.is_day_of_week()) {
				flags |= FormattedDateFlag::DayOfWeek;
			}
			result.push_back({
				EntityType::FormattedDate,
				d.voffset().v,
				d.vlength().v,
				SerializeFormattedDateData(d.vdate().v, flags),
			});
		}, [&](const decrypted::MTPDmessageEntityMentionName &d) {
			const auto userId = UserId(d.vuser_id());
			const auto user = session->data().userLoaded(userId);
			result.push_back({
				EntityType::MentionName,
				d.voffset().v,
				d.vlength().v,
				mentionName(userId.bare, user ? user->accessHash() : 0),
			});
		}, [&](const decrypted::MTPDinputMessageEntityMentionName &d) {
			const auto data = d.vuser_id().match([&](
					const decrypted::MTPDinputUserSelf &) {
				return mentionName(
					session->userId().bare,
					session->user()->accessHash());
			}, [&](const decrypted::MTPDinputUser &u) {
				return mentionName(
					UserId(u.vuser_id()).bare,
					u.vaccess_hash().v);
			}, [&](const decrypted::MTPDinputUserFromMessage &u) {
				return mentionName(UserId(u.vuser_id()).bare, 0);
			}, [](const decrypted::MTPDinputUserEmpty &) {
				return QString();
			});
			if (!data.isEmpty()) {
				result.push_back({
					EntityType::MentionName,
					d.voffset().v,
					d.vlength().v,
					data,
				});
			}
		});
	}
	return result;
}

// Sets f_entities only when something is carried; the vector is written
// (and read by the peer) only then.
[[nodiscard]] auto ApplyEntities(
	decrypted::MTPDdecryptedMessage::Flags &flags,
	const EntitiesInText &entities)
-> decrypted::MTPVector<decrypted::MTPMessageEntity> {
	auto result = EntitiesToDecrypted(entities);
	if (!result.v.isEmpty()) {
		flags |= decrypted::MTPDdecryptedMessage::Flag::f_entities;
	}
	return result;
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
			// Keep the set + alt (emoji): a set reference lets a video / tgs
			// sticker be verified against the server before it is rendered.
			result.push_back(MTP_documentAttributeSticker(
				MTP_flags(0),
				d.valt(),
				ConvertSet(d.vstickerset()),
				MTPMaskCoords()));
		}, [&](const decrypted::MTPDdocumentAttributeStickerFull &d) {
			using Flag = MTPDdocumentAttributeSticker::Flag;
			result.push_back(MTP_documentAttributeSticker(
				MTP_flags(d.is_mask() ? Flag::f_mask : Flag()),
				d.valt(),
				ConvertSet(d.vstickerset()),
				MTPMaskCoords())); // mask placement is not rendered here
		}, [&](const decrypted::MTPDdocumentAttributeCustomEmoji &d) {
			using Flag = MTPDdocumentAttributeCustomEmoji::Flag;
			result.push_back(MTP_documentAttributeCustomEmoji(
				MTP_flags((d.is_free() ? Flag::f_free : Flag())
					| (d.is_text_color() ? Flag::f_text_color : Flag())),
				d.valt(),
				ConvertSet(d.vstickerset())));
		}, [&](const decrypted::MTPDdocumentAttributeHasStickers &) {
			result.push_back(MTP_documentAttributeHasStickers());
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
			// Copied verbatim (like Android): the waveform draws the voice
			// bubble, title/performer label an audio file.
			using Flag = MTPDdocumentAttributeAudio::Flag;
			const auto flags = (d.is_voice() ? Flag::f_voice : Flag())
				| (d.vtitle() ? Flag::f_title : Flag())
				| (d.vperformer() ? Flag::f_performer : Flag())
				| (d.vwaveform() ? Flag::f_waveform : Flag());
			result.push_back(MTP_documentAttributeAudio(
				MTP_flags(flags),
				d.vduration(),
				MTP_string(d.vtitle() ? qs(*d.vtitle()) : QString()),
				MTP_string(d.vperformer() ? qs(*d.vperformer()) : QString()),
				MTP_bytes(d.vwaveform().value_or_empty())));
		}, [&](const decrypted::MTPDdocumentAttributeFilename &d) {
			result.push_back(MTP_documentAttributeFilename(d.vfile_name()));
		}, [&](const auto &) {
		});
	}
	return result;
}

// The mime an unverified animated sticker is stored under. DocumentData picks
// the Lottie / WebM sticker decoders from the MIME ALONE (see
// DocumentData::validateLottieSticker and "any video/webm is a video-sticker"
// in setattributes), so dropping the sticker attributes is not enough to keep
// peer-supplied bytes away from them -- the mime has to go too, exactly as the
// Android client rewrites it.
constexpr auto kUnverifiedStickerMime = "application/octet-stream";

// Whether a document mime is one the sticker / custom-emoji renderers decode
// (Lottie or WebM) -- the kinds that must not be fed peer-supplied bytes
// without a server-side check (Android: verifyAnimatedStickerMessage).
// Case-insensitive: DocumentData lower-cases the mime before matching it, so a
// peer sending "Application/X-TgSticker" would otherwise slip past. A Lottie
// document is a sticker by mime alone; a WebM is one only when it also carries
// a sticker attribute (a plain webm video keeps its mime, as on Android).
[[nodiscard]] bool IsLottieStickerMime(const QString &mime) {
	return !mime.compare(u"application/x-tgsticker"_q, Qt::CaseInsensitive);
}
[[nodiscard]] bool IsAnimatedStickerMime(const QString &mime) {
	return IsLottieStickerMime(mime)
		|| !mime.compare(u"video/webm"_q, Qt::CaseInsensitive);
}

// Drop the sticker / custom-emoji attributes so the document renders as a
// plain file. Returns the set the sticker attribute referenced
// (inputStickerSetEmpty when only a custom-emoji attribute was there), or
// nullopt when there was nothing to strip.
[[nodiscard]] std::optional<MTPInputStickerSet> StripStickerAttributes(
		QVector<MTPDocumentAttribute> &attributes) {
	auto set = std::optional<MTPInputStickerSet>();
	for (auto i = attributes.begin(); i != attributes.end();) {
		const auto sticker = i->match([&](
				const MTPDdocumentAttributeSticker &d) {
			set = d.vstickerset();
			return true;
		}, [&](const MTPDdocumentAttributeCustomEmoji &) {
			if (!set) {
				set = MTPInputStickerSet(MTP_inputStickerSetEmpty());
			}
			return true;
		}, [](const auto &) {
			return false;
		});
		i = sticker ? attributes.erase(i) : (i + 1);
	}
	return set;
}

// Pull the file name out of a decrypted document's attributes, if present.
[[nodiscard]] QString DocumentName(
		const decrypted::MTPVector<decrypted::MTPDocumentAttribute> &attributes) {
	for (const auto &attribute : attributes.v) {
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
, _mtp(&api->instance())
, _messagesWrite([=] {
	if (_messagesDirty) {
		writeMessagesLocal();
	}
})
, _localWrite([=] { writeLocal(); }) {
}

EncryptedChats::~EncryptedChats() = default;

bool EncryptedChats::dhConfigReady() const {
	return (_dhConfig.version >= 0)
		&& (_dhConfig.g != 0)
		&& !_dhConfig.p.empty();
}

void EncryptedChats::ensureDhConfig(Fn<void()> done, Fn<void()> fail) {
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
			if (fail) {
				fail();
			}
		}
	}).fail([=] {
		LOG(("Secret Chat Error: Could not get DH config."));
		if (fail) {
			fail();
		}
	}).send();
}

void EncryptedChats::create(not_null<UserData*> user) {
	// Every dead end below only logs otherwise, leaving the user with a
	// menu click that did nothing.
	const auto failed = [=] {
		if (const auto window = _session->tryResolveWindow()) {
			window->showToast(tr::lng_secret_chat_create_failed(tr::now));
		}
	};
	ensureDhConfig([=] {
		const auto power = RandomPower();
		const auto first = MTP::CreateModExp(_dhConfig.g, _dhConfig.p, power);
		if (first.modexp.empty()) {
			LOG(("Secret Chat Error: Could not create g_a."));
			failed();
			return;
		}
		const auto randomId = base::RandomValue<int32>();
		const auto randomPower = first.randomPower;
		_mtp.request(MTPmessages_RequestEncryption(
			user->inputUser(),
			MTP_int(randomId),
			MTP_bytes(first.modexp)
		)).done([=](const MTPEncryptedChat &result) {
			// Register the exponent and the partner BEFORE applying the update:
			// applyWaiting drops a keyless chat it has no exponent for (one
			// created on another device), and writeLocal needs the user. The
			// exponent is persisted so a restart before the peer accepts can
			// still complete the handshake.
			result.match([&](const auto &data) {
				using Data = std::decay_t<decltype(data)>;
				if constexpr (!std::is_same_v<Data, MTPDencryptedChatEmpty>
					&& !std::is_same_v<Data, MTPDencryptedChatDiscarded>) {
					const auto chat = _session->data().secretChat(
						secretChatIdFromWire(data.vid().v));
					_pending[chat->secretChatId()] = Pending{
						.randomPower = randomPower,
					};
					chat->setUser(user);
				}
			});
			if (const auto chat = applyUpdateChat(result)) {
				// A discard could already have landed for this id (e.g. an
				// immediate close from the other side): don't (re)open a dead chat.
				if (chat->state() == SecretChatState::Discarded) {
					return;
				}
				writeLocal();
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
			failed();
		}).send();
	}, failed);
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
		// Persist our exponent: if we restart before the server answers, the
		// stored g_a + b let the restored Waiting chat re-run accept().
		if (const auto j = _pending.find(id); j != _pending.end()) {
			j->second.randomPower = first.randomPower;
			writeLocal();
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
			_pending.remove(id);
			applyUpdateChat(result);
			sendNotifyLayer(chat);
			flushPendingSends(chat);
			flushPendingMessages(chat);
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
	if (chat->state() == SecretChatState::Discarded) {
		// Already closed by the partner ("Secret chat cancelled" row kept for
		// its history): the server no longer knows the id, so just wipe the
		// local remains.
		clearLocalState(chat, true);
		return;
	}
	using Flag = MTPmessages_DiscardEncryption::Flag;
	auto flags = MTPmessages_DiscardEncryption::Flags();
	if (deleteHistory) {
		flags |= Flag::f_delete_history;
		// discardEncryption(delete_history) does not reliably wipe the chat
		// on the partner's device (bugs.telegram.org/c/27416), so first send
		// the E2E flushHistory action, the same mechanism "Clear history"
		// relies on. No-op unless the chat is Ready with a key.
		flushHistory(chat);
	}
	// After the flush: once the server has processed the discard it rejects
	// any later sendEncryptedService for this chat.
	_mtp.request(MTPmessages_DiscardEncryption(
		MTP_flags(flags),
		MTP_int(chat->secretChatId())
	)).fail([=](const MTP::Error &error) {
		LOG(("Secret Chat Error: discardEncryption failed: %1"
			).arg(error.type()));
	}).afterRequest(sendAfter(chat->secretChatId())).send();
	// The user closed the chat: always wipe the local history too (the dead,
	// keyless row must not linger with its messages).
	clearLocalState(chat, true);
}

void EncryptedChats::clearLocalState(
		not_null<SecretChatData*> chat,
		bool clearHistory) {
	chat->setState(SecretChatState::Discarded);
	// A dead id must decrypt nothing that arrives late for it.
	chat->clearKey();
	chat->clearPreviousKey();
	_pendingMessages.remove(chat->secretChatId());
	// A cancelled chat keeps its row (with the history) until the user deletes
	// it, like the mobile clients; a discard carrying history_deleted, or our
	// own delete, clears it and the row leaves the list (shouldBeInChatList).
	const auto history = _session->data().history(chat->id);
	if (clearHistory) {
		history->clear(History::ClearType::ClearHistory);
		// clear() keeps still-sending messages; a closed chat has nothing to
		// send them to, and a leftover would re-enter the list later.
		const auto leftovers = history->clientSideMessages()
			| ranges::to_vector;
		if (!leftovers.empty()) {
			_session->data().notifyItemsAboutToBeDestroyed(leftovers);
		}
		for (const auto &item : leftovers) {
			item->destroy();
		}
	}
	history->updateChatListExistence();
	clearChatRandomIds(chat->id);
	_messageTtls.remove(chat->id);
	_ttlNotices.remove(chat->id);
	_screenshotNotices.remove(chat->id);
	// Drop the seq-no reliability state: a discarded chat can neither buffer
	// out-of-order messages nor answer resend requests.
	_holes.remove(chat->secretChatId());
	_requestedHoles.remove(chat->secretChatId());
	_sentLayers.remove(chat->secretChatId());
	// Drop the remaining per-chat state. A discarded chat never reaches Ready, so
	// queued sends would never flush (and _pendingSends can hold large prepared
	// upload payloads); the handshake/rekey maps would otherwise keep DH private
	// exponents alive for the rest of the session. The rpl::lifetime in
	// _ttlNoticeNameWatch is torn down by removing its entry.
	_pendingSends.remove(chat->secretChatId());
	cancelIncomingFiles(chat->secretChatId());
	_incomingFiles.remove(chat->secretChatId());
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
	// The prime is fetched per run and never stored, so right after a restart
	// we have none. applyChat() needs it to turn the peer's g_b into the key,
	// and computing with an empty prime fails the fingerprint check, which
	// discards the very chat a restored handshake was kept alive for -- fetch
	// it first and apply the update once it is here.
	if (chat.type() == mtpc_encryptedChat && !dhConfigReady()) {
		ensureDhConfig([=] {
			applyUpdateChat(chat);
		});
		return;
	}
	applyUpdateChat(chat);
}

void EncryptedChats::writeLocal() {
	// The chats blob carries every chat's advanced in_seq_no (and the qts
	// checkpoint): the messages those sequences cover must be on disk first,
	// or a crash between the two writes leaves a sequence that rejects the
	// server's replay as a duplicate. So any pending message write goes
	// first, whichever path (a service action, a send, a rekey) writes here.
	if (_messagesDirty) {
		writeMessagesLocal();
	}
	_session->local().writeSecretChats();
}

void EncryptedChats::setQts(int32 qts) {
	if (qts <= _qts) {
		return;
	}
	_qts = qts;
	// Flush the messages blob first: the checkpoint below tells the server we
	// have everything up to `qts`, so a crash between the two writes would
	// lose messages the server will never resend. This rewrites the whole
	// blob per message, like the chats blob already does; batch both if it
	// ever shows up in a profile.
	writeMessagesLocal();
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

// v2 appends each message's reply linkage (the replied-to message's stable
// random_id) right after the common header. v3 appends the text entities
// (serialized as composer tags) after every record's kind payload, the voice
// waveform after a Document record, and adds the screenshot notice kind. v4
// adds the pending download kind.
constexpr auto kSecretMessagesVersion = qint32(4);

// Stamp reply_to_random_id onto an outgoing decrypted message: when the
// replied-to message has a known local random_id, set the flag and return it
// (0 / flag unset otherwise).
[[nodiscard]] uint64 ApplyReplyToRandomId(
		decrypted::MTPDdecryptedMessage::Flags &flags,
		std::optional<uint64> randomId) {
	if (!randomId) {
		return 0;
	}
	flags |= decrypted::MTPDdecryptedMessage::Flag::f_reply_to_random_id;
	return *randomId;
}

// Outer transport flags of messages.sendEncrypted: only "silent" exists.
[[nodiscard]] MTPflags<MTPmessages_SendEncrypted::Flags> SendEncryptedFlags(
		bool silent) {
	using Flag = MTPmessages_SendEncrypted::Flag;
	return MTP_flags(silent ? Flag::f_silent : Flag());
}

// The server's timestamp of a sent encrypted message (both result kinds
// carry one); stamped on the local bubble so its time matches the peer's.
[[nodiscard]] TimeId SentDate(const MTPmessages_SentEncryptedMessage &result) {
	return result.match([](const auto &d) { return d.vdate().v; });
}

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
	// A location. Covers a venue too: an empty title AND address means a plain
	// point, and MediaLocation keeps nothing else that is rendered.
	Geo = 5,
	Contact = 6,
	// "X took a screenshot" notice (service message, incoming only): no
	// payload, the record header already carries the date.
	ServiceScreenshot = 7,
	// An incoming media whose download had not finished: the decrypted
	// message and the server file, serialized as TL, redone through
	// processDecryptedMessage on restore. Header fields are all zero except
	// random_id, date, and the placeholder's read / ttlDestroyAt (the
	// message carries the rest).
	PendingDownload = 8,
};
// A StoredKind::PendingDownload record, applied once its chat's items are
// restored (see restoreMessages).
struct PendingDownload {
	decrypted::MTPDecryptedMessage message;
	MTPEncryptedFile file;
	TimeId date = 0;
	// The placeholder bubble's state at the time of the write: already
	// viewed, and the absolute destroy time if its countdown was running.
	bool read = false;
	TimeId ttlDestroyAt = 0;
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

// The media shape a document is stored and sent as. Order matters:
// isAnimation() is true for round videos too, so the round check has to come
// first or a video note goes out as a GIF. Anything matching nothing is a
// plain file.
[[nodiscard]] StoredMedia DocumentShape(not_null<DocumentData*> document) {
	if (document->isVideoMessage()
		|| document->type == RoundVideoDocument) {
		return StoredMedia::Round;
	} else if (document->type == AnimatedDocument
		|| document->isAnimation()) {
		return StoredMedia::Gif;
	} else if (document->isVideoFile()) {
		return StoredMedia::Video;
	} else if (document->isVoiceMessage()) {
		return StoredMedia::Voice;
	} else if (document->isImage()) {
		return StoredMedia::Image;
	}
	return StoredMedia::File;
}

// Self-destruct media whose timer starts when the user OPENS it, not when the
// chat is read (the mobile clients exclude these from the read-triggered
// start): a photo / non-round video with a short ttl is shown covered
// (view-once style), and any voice / round video arms on playback.
struct SelfDestructOnOpen {
	bool cover = false; // shown covered (view-once style)
	bool unread = false; // marked media-unread: unplayed dot, opening arms it
	// Skipped by the read-triggered start: the timer waits for the open /
	// playback. Android limits this to ttl <= 60 (getMessageMediaType); a
	// longer-lived voice note or round video starts on the chat read like
	// text, so both sides count from the same moment.
	bool deferArm = false;
};
[[nodiscard]] SelfDestructOnOpen SelfDestructMode(
		int32 ttl,
		bool photo,
		bool video,
		bool round,
		bool voice) {
	auto result = SelfDestructOnOpen();
	if (ttl <= 0) {
		return result;
	}
	result.cover = (ttl <= 60) && (photo || (video && !round));
	result.unread = result.cover || voice || round;
	result.deferArm = (ttl <= 60) && result.unread;
	return result;
}

[[nodiscard]] SelfDestructOnOpen SelfDestructMode(
		int32 ttl,
		PhotoData *photo,
		DocumentData *document) {
	return SelfDestructMode(
		ttl,
		(photo != nullptr),
		document && (document->isVideoFile() || document->isGifv()),
		document && document->isVideoMessage(),
		document && document->isVoiceMessage());
}

[[nodiscard]] SelfDestructOnOpen SelfDestructMode(
		int32 ttl,
		not_null<HistoryItem*> item) {
	const auto media = item->media();
	return SelfDestructMode(
		ttl,
		media ? media->photo() : nullptr,
		media ? media->document() : nullptr);
}

// A playable self-destruct media must outlive its own duration: Android
// floors the ttl at duration + 1 on both the sending and the receiving side,
// so the two copies expire together.
[[nodiscard]] int32 EffectiveMediaTtl(int32 ttl, int duration) {
	return (ttl > 0 && duration > 0) ? std::max(duration + 1, ttl) : ttl;
}

// Human-readable self-destruct duration for the in-chat TTL-change notice. Mirrors
// the picker labels (window_peer_menu.cpp) so short presets read as seconds/minutes,
// not "0 hours" (Ui::FormatTTL collapses anything sub-day to hours).
} // namespace

int32 SecretChatMediaCoverTtl(
		int32 ttl,
		PhotoData *photo,
		DocumentData *document) {
	return SelfDestructMode(ttl, photo, document).cover ? ttl : 0;
}

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

MsgId LocalReplyToMsgId(FullMsgId replyMessageId, PeerId chatId) {
	return (replyMessageId && replyMessageId.peer == chatId)
		? replyMessageId.msg
		: MsgId(0);
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
		const std::vector<std::shared_ptr<IncomingFile>> *downloads = nullptr;
	};
	static const auto kNoMessages = base::flat_map<MsgId, uint64>();
	auto chats = std::vector<Chat>();
	_session->data().enumerateSecretChats([&](
			not_null<SecretChatData*> chat) {
		const auto i = _messageRandomIds.find(chat->id);
		const auto messages = (i != _messageRandomIds.end()
			&& !i->second.empty())
			? &i->second
			: nullptr;
		const auto d = _incomingFiles.find(chat->secretChatId());
		const auto downloads = (d != _incomingFiles.end()
			&& !d->second.empty())
			? &d->second
			: nullptr;
		if (messages || downloads) {
			chats.push_back({
				chat->secretChatId(),
				chat->id,
				chat,
				messages ? messages : &kNoMessages,
				downloads,
			});
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
		const auto downloading = [&](MsgId msgId) {
			return chat.downloads && ranges::any_of(
				*chat.downloads,
				[&](const auto &state) {
					return (state->pendingItemId == msgId);
				});
		};
		for (const auto &[msgId, randomId] : *chat.messages) {
			const auto item = _session->data().message(
				FullMsgId(chat.peerId, msgId));
			if (!item || downloading(msgId)) {
				// A placeholder bubble is persisted as its download below,
				// and rebuilt from that (a Document record would register
				// its random_id and make the redo look like a duplicate).
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
			const auto screenshot = [&] {
				const auto n = _screenshotNotices.find(chat.peerId);
				return (n != _screenshotNotices.end())
					&& n->second.contains(msgId);
			}();
			const auto media = item->media();
			const auto document = media ? media->document() : nullptr;
			const auto photo = (media && !document) ? media->photo() : nullptr;
			// location() is overridden only by MediaLocation, so it is a sound
			// discriminator for the downcast; the class keeps its point, title
			// and description private with no other way to reach them.
			const auto location = (media && media->location())
				? static_cast<const Data::MediaLocation*>(media)
				: nullptr;
			const auto contact = media ? media->sharedContact() : nullptr;
			// External/public documents (stickers) reference a server file by
			// id/access_hash/dc and carry no on-disk blob -> persisted as a
			// StoredKind::External record (rebuilt with that remote location).
			// (Local E2E files have no remote location.)
			const auto external = document && document->hasRemoteLocation();
			// Persist outgoing read-state so restored bubbles keep ✓✓. Local
			// MsgIds are reassigned on restore, so the outbox-read-till can't
			// be carried verbatim; instead we flag each read outgoing message
			// and rebuild the till in the new id space (see restoreMessages).
			// Outgoing: seen by the partner (outbox-read-till). Incoming: the
		// ClientSideUnread bit is clear, i.e. the chat was viewed after it
		// arrived; restored as such so the unread badge survives a restart.
		const auto read = !item->unread(item->history());
			// Self-destruct (TTL), Android model: persist the PENDING ttl seconds of
			// an UNREAD message (no countdown yet) so it survives restart and arms on
			// the next read, AND the ABSOLUTE ttlDestroyAt of a READ message so its
			// countdown resumes from the remaining time (never restarts) -- the two are
			// mutually exclusive (startSelfDestructTimers erases the pending entry when
			// it arms). A notice has no timer -> force 0 (it must never self-destruct).
			auto ttlSeconds = qint32(0);
			if (!notice && !screenshot) {
				if (const auto k = _messageTtls.find(chat.peerId);
						k != _messageTtls.end()) {
					if (const auto m = k->second.find(msgId);
							m != k->second.end()) {
						ttlSeconds = m->second;
					}
				}
			}
			// Reply linkage (v2): persist the replied-to message's stable
			// random_id, not its MsgId (MsgIds are reassigned on restore). 0 when
			// the message is not a reply or the target is no longer known locally.
			// chat.messages already points at this chat's inner map, so look the
			// reply target up directly (skip randomIdByMsg's redundant outer search
			// and the guaranteed-miss inner search for the common non-reply case).
			const auto replyToId = item->replyToId();
			const auto replyIt = replyToId
				? chat.messages->find(replyToId)
				: chat.messages->end();
			const auto replyToRandomId = quint64((replyIt != chat.messages->end())
				? replyIt->second
				: 0);
			++count;
			bodyStream
				<< qint64(msgId.bare)
				<< quint64(randomId)
				<< qint32(item->date())
				<< qint32(item->out() ? 1 : 0)
				<< qint32(read ? 1 : 0)
				<< ttlSeconds
				<< qint32((notice || screenshot) ? 0 : item->ttlDestroyAt())
				<< replyToRandomId
				// v3: album membership (grouped_id), so restored members
				// are laid out as one album again.
				<< quint64(item->groupId().raw());
			if (screenshot) {
				bodyStream << qint32(StoredKind::ServiceScreenshot);
			} else if (notice) {
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
				const auto stored = DocumentShape(document);
				const auto sized = (stored != StoredMedia::File)
					&& (stored != StoredMedia::Voice);
				const auto timed = (stored != StoredMedia::File)
					&& (stored != StoredMedia::Image);
				const auto w = sized
					? qint32(document->dimensions.width())
					: 0;
				const auto h = sized
					? qint32(document->dimensions.height())
					: 0;
				const auto duration = timed
					? qint32(document->duration() / 1000)
					: 0;
				// v3: the voice waveform, in its 5-bit wire encoding.
				const auto waveform = document->voice()
					? documentWaveformEncode5bit(document->voice()->waveform)
					: QByteArray();
				bodyStream
					<< qint32(StoredKind::Document)
					<< quint64(document->id)
					<< document->secretEncryptedPath()
					<< document->mimeString()
					<< document->filename()
					<< qint64(document->size)
					<< item->originalText().text
					<< qint32(stored)
					<< w << h << duration
					<< waveform;
			} else if (photo) {
				// Photos are rebuilt as PhotoData from the encrypted on-disk JPEG;
				// persist its path so the inline image reloads on restart.
				bodyStream
					<< qint32(StoredKind::Photo)
					<< quint64(photo->id)
					<< photo->secretEncryptedPath()
					<< item->originalText().text;
			} else if (location) {
				// Both sides of the stream must use the same floating-point
				// width, or every later record of the chat is misread.
				bodyStream
					<< qint32(StoredKind::Geo)
					<< double(location->point().lat())
					<< double(location->point().lon())
					<< location->title()
					<< location->description()
					<< item->originalText().text;
			} else if (contact) {
				bodyStream
					<< qint32(StoredKind::Contact)
					<< contact->phoneNumber
					<< contact->firstName
					<< contact->lastName
					<< qint64(contact->userId.bare)
					<< item->originalText().text;
			} else {
				bodyStream
					<< qint32(StoredKind::Text)
					<< item->originalText().text;
			}
			// v3: formatting of the record's text / caption, in the draft
			// tag format (strings, so it survives EntityType renumbering).
			bodyStream << TextUtilities::SerializeTags(
				TextUtilities::ConvertEntitiesToTextTags(
					item->originalText().entities));
		}
		// After the items, so a message this one replies to is restored
		// (and its random_id registered) before the download is redone.
		if (chat.downloads) {
			for (const auto &state : *chat.downloads) {
				// The placeholder bubble (if any) was viewed / armed like
				// any other message: carry that, or the redo notifies again
				// and a running countdown restarts from the full ttl.
				const auto item = state->pendingItemId
					? _session->data().message(
						FullMsgId(chat.peerId, state->pendingItemId))
					: nullptr;
				const auto read = item && !item->unread(item->history());
				++count;
				bodyStream
					<< qint64(0)
					<< quint64(state->randomId)
					<< qint32(state->date)
					<< qint32(0) // out
					<< qint32(read ? 1 : 0)
					<< qint32(0) // ttlSeconds: carried by the message
					<< qint32(item ? item->ttlDestroyAt() : 0)
					<< quint64(0) // replyToRandomId: carried by the message
					<< quint64(0) // groupedId: carried by the message
					<< qint32(StoredKind::PendingDownload)
					<< BytesToQ(MTP::SecretChat::SerializeObject(state->message))
					<< BytesToQ(MTP::SecretChat::SerializeObject(state->file))
					<< QByteArray(); // tags
			}
		}
		stream << chat.secretChatId << count;
		stream.writeRawData(body.constData(), body.size());
	}
	return result;
}

void EncryptedChats::writeMessagesLocal() {
	_messagesDirty = false;
	_session->local().writeSecretChatMessages(serializeMessages());
}

void EncryptedChats::scheduleMessagesWrite() {
	_messagesDirty = true;
	_messagesWrite.call();
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
	// Set when the blob has to be written back without waiting for the next
	// message event: a moved file or a dropped expired record. Checked on
	// every exit path (a truncated blob returns from inside the loops).
	auto rewrite = false;
	const auto rewriteGuard = gsl::finally([&] {
		if (rewrite) {
			scheduleMessagesWrite();
		}
	});
	// The secret_files dir moved from a global tdata/ location into the
	// per-account base path (wiped on logout); carry old files over the first
	// time they are seen. The target dir is created on the first hit only.
	const auto legacy = cWorkingDir() + u"tdata/secret_files/"_q;
	const auto dir = _session->local().secretFilesPath();
	auto dirReady = false;
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
		auto unread = 0;
		auto downloads = std::vector<PendingDownload>();
		if (chat) {
			// The unread count must be known before the first item lands:
			// History::newItemAdded requests a (doomed) dialog entry otherwise.
			ensureInDialogs(chat);
		}
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
			auto lat = double(0), lon = double(0);
			auto address = QString(), phone = QString();
			auto firstName = QString(), lastName = QString();
			auto contactUserId = qint64(0);
			auto waveform = QByteArray();
			auto pendingMessage = QByteArray(), pendingFile = QByteArray();
			stream >> storedId >> randomId >> date >> out >> read;
			stream >> ttlSeconds >> ttlDestroyAt;
			auto replyToRandomId = quint64(0);
			if (version >= 2) {
				stream >> replyToRandomId;
			}
			auto groupedId = quint64(0);
			if (version >= 3) {
				stream >> groupedId;
			}
			auto ttlNoticeSeconds = qint32(0);
			auto ttlNoticeText = QString();
			stream >> kind;
			if (kind == qint32(StoredKind::ServiceTtl)) {
				stream >> ttlNoticeSeconds >> ttlNoticeText;
			} else if (kind == qint32(StoredKind::ServiceScreenshot)) {
				// No payload.
			} else if (kind == qint32(StoredKind::Document)) {
				stream >> fileId >> path >> mime >> filename >> size >> caption
					>> stored >> w >> h >> duration;
				if (version >= 3) {
					stream >> waveform;
				}
			} else if (kind == qint32(StoredKind::Photo)) {
				stream >> fileId >> path >> caption;
			} else if (kind == qint32(StoredKind::External)) {
				stream >> fileId >> accessHash >> dcId >> fileReference >> mime
					>> filename >> size >> caption >> isSticker >> alt
					>> setShortName >> setId >> setAccessHash >> w >> h
					>> isVideo >> duration;
			} else if (kind == qint32(StoredKind::Geo)) {
				stream >> lat >> lon >> caption >> address >> text;
			} else if (kind == qint32(StoredKind::Contact)) {
				stream >> phone >> firstName >> lastName >> contactUserId
					>> text;
			} else if (kind == qint32(StoredKind::Text)) {
				stream >> text;
			} else if (kind == qint32(StoredKind::PendingDownload)) {
				stream >> pendingMessage >> pendingFile;
			} else {
				// Records carry no length prefix, so a kind written by a newer
				// build cannot be skipped: reading it as a Text record would
				// misparse every later message of this chat. Stop instead and
				// keep what was already restored.
				DEBUG_LOG(("Secret chats: unknown stored kind %1."
					).arg(kind));
				return;
			}
			auto tags = QByteArray();
			if (version >= 3) {
				stream >> tags;
			}
			// Re-attaches the persisted formatting to whichever string the
			// kind uses as the item text (caption for media, text otherwise).
			const auto withEntities = [&](const QString &value) {
				return Linkified(TextWithEntities{
					value,
					TextUtilities::ConvertTextTagsToEntities(
						TextUtilities::DeserializeTags(tags, value.size())),
				});
			};
			if (stream.status() != QDataStream::Ok) {
				return;
			} else if (!chat || !chat->user()) {
				continue; // Keep parsing the stream, just drop this chat.
			}
			if (kind == qint32(StoredKind::PendingDownload)) {
				// Its placeholder expired while we were closed: nothing was
				// written for it yet, so there is only the record to drop.
				if (ttlDestroyAt > 0 && ttlDestroyAt <= base::unixtime::now()) {
					rewrite = true;
					continue;
				}
				auto download = PendingDownload{
					.date = date,
					.read = (read != 0),
					.ttlDestroyAt = ttlDestroyAt,
				};
				const auto ok = MTP::SecretChat::DeserializeObject(
					download.message,
					bytes::make_span(pendingMessage))
					&& MTP::SecretChat::DeserializeObject(
						download.file,
						bytes::make_span(pendingFile));
				if (ok && chat->state() != SecretChatState::Discarded) {
					downloads.push_back(std::move(download));
				}
				continue;
			}
			// A self-destruct message that was already counting down and whose
			// absolute destroy time passed while the app was closed is gone: skip
			// rebuilding it entirely (Android deletes such a task on load without
			// showing the message). A still-armed message (destroyAt in the future)
			// is rebuilt below and its countdown resumes from the remaining time.
			if (ttlDestroyAt > 0 && ttlDestroyAt <= base::unixtime::now()) {
				// The runtime expiry deletes the media with the message
				// (removeSecretFile); a message that expired while we were
				// closed must not leave its file and cache mirror behind, nor
				// stay in the blob.
				if (!path.isEmpty()
					&& (path.startsWith(dir) || path.startsWith(legacy))) {
					QFile::remove(path);
					if (kind == qint32(StoredKind::Document)) {
						_session->data().cache().remove(
							Data::DocumentCacheKey(0, fileId));
					}
				}
				// Nor may the resend cache keep its text and file keys: the
				// runtime expiry retires them from the itemRemoved hook, but
				// no item is ever built for a record skipped here.
				if (out && randomId) {
					retireSentLayers(
						chat->secretChatId(),
						[&](const SentLayer &layer) {
							return (layer.randomId == randomId);
						});
				}
				rewrite = true;
				continue;
			}
			// ClientSideUnread is re-applied AFTER the item is added (below):
			// set at creation it would replay the desktop notification and the
			// count increment from History::newItemAdded. Assign a fresh local
			// id (the stored id belonged to a previous session's counter) and
			// re-map its random_id so delete/read-by-random-id still resolve.
			auto flags = MessageFlags(MessageFlag::Local);
			const auto from = out
				? _session->userPeerId()
				: chat->user()->id;
			if (out) {
				flags |= MessageFlag::Outgoing;
			}
			// Self-destruct media must not be savable/shareable (matches the
			// receive path and the Android client); NoForwards makes
			// forbidsSaving() true. Mirrors the media-only receive sites; text
			// stays copyable. An already-read message was persisted with the
			// pending ttlSeconds erased (it lives in ttlDestroyAt instead), so
			// gate on either: otherwise read self-destruct media would restore
			// savable, the sole guard since hasCopyRestriction() is off here.
			if ((ttlSeconds > 0 || ttlDestroyAt > 0)
				&& (kind == qint32(StoredKind::Document)
					|| kind == qint32(StoredKind::Photo)
					|| kind == qint32(StoredKind::External))) {
				flags |= MessageFlag::NoForwards;
			}
			// Reply linkage (v2): the replied-to message is older, so it was
			// already rebuilt + re-registered earlier in this loop -> resolve its
			// fresh MsgId by the persisted random_id. Drop the link if its target
			// is gone (e.g. it self-destructed and was skipped). NoForwards is set
			// above (kind-gated), so pass ttlSeconds 0 here.
			const auto repliedTo = replyToRandomId
				? findByRandomId(chat->id, replyToRandomId)
				: MsgId(0);
			const auto replyTo = resolveLocalReply(
				chat->id,
				repliedTo,
				0,
				flags);
			// An unread self-destruct media (pending ttl, no countdown yet)
			// re-arms on open / playback, never on the chat read: rebuild the
			// unread-media flag and the view-once cover exactly as the receive
			// path set them (see IncomingFile::coverOnOpen / contentUnread).
			auto mediaTtlSeconds = crl::time(0);
			if (out || ttlDestroyAt == 0) {
				const auto isDocument = (kind == qint32(StoredKind::Document));
				const auto mode = SelfDestructMode(
					ttlSeconds,
					(kind == qint32(StoredKind::Photo)),
					isDocument
						&& (stored == qint32(StoredMedia::Video)
							|| stored == qint32(StoredMedia::Gif)),
					isDocument && (stored == qint32(StoredMedia::Round)),
					isDocument && (stored == qint32(StoredMedia::Voice)));
				if (mode.unread && !out) {
					flags |= MessageFlag::MediaIsUnread;
				}
				if (mode.cover) {
					mediaTtlSeconds = ttlSeconds;
				}
			}
			if (path.startsWith(legacy)) {
				if (!dirReady) {
					QDir().mkpath(dir);
					dirReady = true;
				}
				const auto moved = dir + QString::number(fileId, 16);
				if (QFile::rename(path, moved)) {
					path = moved;
					// The rename is one-way: the blob still points at the old
					// location, so it has to be rewritten before we quit or
					// the file is unreachable on the next launch.
					rewrite = true;
				}
			}
			const auto id = _session->data().nextLocalMessageId();
			const auto makeFields = [&] {
				return HistoryItemCommonFields{
					.id = id,
					.flags = flags,
					.from = from,
					.replyTo = replyTo,
					.date = date,
					.groupedId = uint64(groupedId),
					.mediaSpoiler = (mediaTtlSeconds > 0),
					.mediaTtlSeconds = mediaTtlSeconds,
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
			if (kind == qint32(StoredKind::ServiceScreenshot)) {
				const auto item = buildScreenshotServiceMessage(chat, date, id);
				if (randomId) {
					registerRandomId(chat->id, item->id, randomId);
				}
				_screenshotNotices[chat->id].emplace(item->id);
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
					// processDocument merges into the id-keyed DocumentData, so
					// an outgoing voice restored without its waveform would lose
					// the live one; carry it for both directions.
					using Flag = MTPDdocumentAttributeAudio::Flag;
					attributes.push_back(MTP_documentAttributeAudio(
						MTP_flags(Flag::f_voice
							| (waveform.isEmpty() ? Flag() : Flag::f_waveform)),
						MTP_int(duration),
						MTPstring(),
						MTPstring(),
						MTP_bytes(waveform)));
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
					withEntities(caption));
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
						withEntities(caption));
				} else {
					history->addNewLocalMessage(
						makeFields(),
						caption.isEmpty()
							? TextWithEntities{ u"[photo]"_q }
							: withEntities(caption),
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
					withEntities(caption));
			} else if (kind == qint32(StoredKind::Geo)) {
				// caption carries the venue title here; an empty title AND
				// address means the record is a plain point.
				const auto point = Data::LocationPoint(
					lat,
					lon,
					Data::LocationPoint::NoAccessHash);
				history->addNewLocalMessage(
					makeFields(),
					withEntities(text),
					(caption.isEmpty() && address.isEmpty())
						? MTP_messageMediaGeo(point.toMTP())
						: MTP_messageMediaVenue(
							point.toMTP(),
							MTP_string(caption),
							MTP_string(address),
							MTP_string(), // provider: not persisted
							MTP_string(), // venue_id: not persisted
							MTP_string())); // venue_type: no secret field
			} else if (kind == qint32(StoredKind::Contact)) {
				history->addNewLocalMessage(
					makeFields(),
					withEntities(text),
					MTP_messageMediaContact(
						MTP_string(phone),
						MTP_string(firstName),
						MTP_string(lastName),
						MTP_string(), // vcard: no secret-layer field
						MTP_long(contactUserId)));
			} else {
				history->addNewLocalMessage(
					makeFields(),
					withEntities(text),
					MTP_messageMediaEmpty());
			}
			if (randomId) {
				registerRandomId(chat->id, id, randomId);
			}
			// Restore the self-destruct timer: a still-armed message (destroyAt in
			// the future -- already-expired ones were skipped above) resumes its
			// countdown from the remaining time; an unread ttl message re-arms its
			// pending timer for the next read.
			const auto restored = _session->data().message(
				FullMsgId(chat->id, id));
			if (ttlDestroyAt > 0) {
				if (restored) {
					restored->setSecretChatSelfDestructAt(ttlDestroyAt);
				}
			} else if (ttlSeconds > 0) {
				noteMessageTtl(chat->id, id, ttlSeconds);
			}
			if (out && read) {
				maxReadOutId = id;
			} else if (!out && !read && restored) {
				// Not viewed before the restart: unread again, so the read path
				// (Histories::readInboxTill) fires on open and tells the partner.
				restored->markClientSideAsUnread();
				++unread;
			}
		}
		if (history && maxReadOutId) {
			// Rebuild the outbox-read-till so restored outgoing bubbles that the
			// partner had already read show ✓✓ instead of a single ✓.
			history->outboxRead(*maxReadOutId);
		}
		if (chat) {
			ensureInDialogs(chat);
			if (unread) {
				history->setUnreadCount(unread);
			}
			// Redo the interrupted downloads through the live receive path,
			// once the unread count above is settled so their bubbles add to
			// it instead of being overwritten by it.
			for (const auto &download : downloads) {
				processDecryptedMessage(
					chat,
					download.message,
					download.date,
					&download.file,
					download.read,
					download.ttlDestroyAt);
			}
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
			// If the partner also deleted the history and we are viewing the
			// chat, leave the now dead conversation (the open section otherwise
			// keeps it on screen -- and force-keeps its row in the list -- until
			// we navigate away). A plain cancel keeps the history readable.
			if (!data.is_history_deleted()) {
				return chat;
			}
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
	// A discard already won for this id (never resurrect a dead chat), or the
	// key is already agreed (a replayed update must not regress it).
	if (chat->state() == SecretChatState::Discarded
		|| chat->state() == SecretChatState::Ready) {
		return chat;
	}
	// A chat requested from another device of ours: we hold no exponent for
	// it, so it can never become Ready here. Never list it (Android drops such
	// chats too); a persisted Requested chat keeps its exponent in _pending.
	if (chat->state() == SecretChatState::Empty
		&& !_pending.contains(chat->secretChatId())) {
		return nullptr;
	}
	chat->setAccessHash(data.vaccess_hash().v);
	const auto self = _session->userId().bare;
	const auto adminId = BareId(data.vadmin_id().v);
	const auto participantId = BareId(data.vparticipant_id().v);
	const auto amCreator = (adminId == self);
	chat->setIsCreator(amCreator);
	chat->setUser(_session->data().user(
		UserId(amCreator ? participantId : adminId)));
	chat->setState(SecretChatState::Requested);
	chat->setLoadedStatus(PeerData::LoadedStatus::Full);
	ensureInDialogs(chat);
	return chat;
}

SecretChatData *EncryptedChats::applyRequested(
		const MTPDencryptedChatRequested &data) {
	const auto chat = _session->data().secretChat(secretChatIdFromWire(data.vid().v));
	// Only a brand-new id may be requested: a duplicate request for a live
	// (or discarded) chat must not overwrite its handshake state / re-fire
	// accept() with a fresh key (Android accepts only when the chat is unknown).
	if (chat->state() != SecretChatState::Empty) {
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
	// Persist g_a so a restart before acceptEncryption completes can retry.
	writeLocal();

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
	const auto self = _session->userId().bare;
	const auto adminId = BareId(data.vadmin_id().v);
	const auto participantId = BareId(data.vparticipant_id().v);
	const auto amCreator = (adminId == self);

	// As the requester we now have the other party's g_b -> compute the key.
	auto becameReady = false;
	if (!chat->hasKey()) {
		const auto i = _pending.find(chat->secretChatId());
		if (i == _pending.end() || i->second.randomPower.empty()) {
			// No exponent (a chat established on another device, or one whose
			// handshake material we lost): it can never decrypt, so never list
			// a dead keyless row for it.
			LOG(("Secret Chat Error: encryptedChat %1 without an exponent, "
				"ignored.").arg(chat->secretChatId()));
			return nullptr;
		}
		const auto computed = MTP::SecretChat::ComputeKey(
			bytes::make_span(data.vg_a_or_b().v),
			i->second.randomPower,
			_dhConfig.p);
		if (computed
			&& computed->fingerprint == uint64(data.vkey_fingerprint().v)) {
			chat->setAccessHash(data.vaccess_hash().v);
			chat->setKey(computed->key, computed->fingerprint);
			chat->setState(SecretChatState::Ready);
			_pending.erase(i);
			becameReady = true;
			DEBUG_LOG(("Secret Chat: chat %1 Ready, fingerprint %2."
				).arg(chat->secretChatId()
				).arg(QString::number(computed->fingerprint, 16)));
		} else {
			LOG(("Secret Chat Error: Key fingerprint mismatch."));
			chat->setAccessHash(data.vaccess_hash().v);
			discard(chat, false);
			return chat;
		}
	}
	chat->setAccessHash(data.vaccess_hash().v);
	chat->setIsCreator(amCreator);
	const auto otherId = UserId(amCreator ? participantId : adminId);
	chat->setUser(_session->data().user(otherId));
	chat->setLoadedStatus(PeerData::LoadedStatus::Full);
	ensureInDialogs(chat);
	if (becameReady) {
		sendNotifyLayer(chat);
		flushPendingSends(chat);
		flushPendingMessages(chat);
		writeLocal();
	}
	return chat;
}

void EncryptedChats::sendText(
		not_null<SecretChatData*> chat,
		const TextWithEntities &text,
		MsgId replyToMsgId,
		bool silent) {
	// Never attach decryptedMessageMediaWebPage: the Android peer resolves a
	// pending preview with account.getWebPagePreview, naming the link to the
	// server in plaintext -- exactly what this client refuses to do for secret
	// chats. Android itself sends that media only when it generated a preview
	// on its own side, so a plain text is what a preview-less sender emits.
	sendDecryptedMessage(
		chat,
		text,
		decrypted::MTP_decryptedMessageMediaEmpty(),
		MTP_messageMediaEmpty(),
		replyToMsgId,
		silent);
}

void EncryptedChats::sendDecryptedMessage(
		not_null<SecretChatData*> chat,
		const TextWithEntities &text,
		const decrypted::MTPDecryptedMessageMedia &wireMedia,
		const MTPMessageMedia &localMedia,
		MsgId replyToMsgId,
		bool silent) {
	if (queuePendingSend(chat, [=] {
		sendDecryptedMessage(
			chat,
			text,
			wireMedia,
			localMedia,
			replyToMsgId,
			silent);
	})) {
		return;
	}
	const auto randomId = base::RandomValue<uint64>();
	auto randomBytes = bytes::vector(16);
	bytes::set_random(randomBytes);

	using MessageFlags = decrypted::MTPDdecryptedMessage::Flags;
	auto flags = MessageFlags();
	// A default-constructed boxed media asserts in type(): text-only sends
	// pass decryptedMessageMediaEmpty instead, which stays off the wire.
	if (wireMedia.type() != decrypted::mtpc_decryptedMessageMediaEmpty) {
		flags |= decrypted::MTPDdecryptedMessage::Flag::f_media;
	}
	if (silent) {
		flags |= decrypted::MTPDdecryptedMessage::Flag::f_silent;
	}
	const auto replyToRandomId = ApplyReplyToRandomId(
		flags,
		randomIdByMsg(chat->id, replyToMsgId));
	const auto entities = ApplyEntities(flags, text.entities);
	const auto message = decrypted::MTP_decryptedMessage(
		MTP_flags(flags),
		MTP_long(randomId),
		MTP_int(chat->ttl()),
		MTP_string(text.text),
		wireMedia,
		entities,
		MTP_string(),
		MTP_long(replyToRandomId),
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
	const auto itemId = addDecryptedMessage(
		chat,
		text,
		date,
		true,
		randomId,
		chat->ttl(),
		replyToRandomId ? replyToMsgId : MsgId(0),
		localMedia);
	const auto requestId = _mtp.request(MTPmessages_SendEncrypted(
		SendEncryptedFlags(silent),
		inputChat(chat),
		MTP_long(randomId),
		MTP_bytes(encrypted)
	)).done([=](const MTPmessages_SentEncryptedMessage &result) {
		DEBUG_LOG(("Secret Chat: sent message to chat %1, out_seq raw %2."
			).arg(chat->secretChatId()).arg(chat->rawOutSeqNo()));
		markSecretSent(itemId, SentDate(result));
	}).fail([=](const MTP::Error &error) {
		LOG(("Secret Chat Error: sendEncrypted failed: %1"
			).arg(error.type()));
		markSecretSendFailed(itemId);
	}).afterRequest(sendAfter(chat->secretChatId())).send();
	setSendAfter(chat->secretChatId(), requestId);

	// out_seq_no advanced; persist so ordering survives a restart.
	writeLocal();

	chat->countKeyUseOut();
	maybeStartRekey(chat);
}

void EncryptedChats::sendLocation(
		not_null<SecretChatData*> chat,
		float64 lat,
		float64 lon,
		MsgId replyToMsgId,
		bool silent) {
	// The secret layer orders the pair lat, long, while api.tl geoPoint is
	// long, lat. Build the local side through LocationPoint so the order is
	// never transcribed by hand.
	const auto point = Data::LocationPoint(
		lat,
		lon,
		Data::LocationPoint::NoAccessHash);
	sendDecryptedMessage(
		chat,
		TextWithEntities(),
		decrypted::MTP_decryptedMessageMediaGeoPoint(
			MTP_double(lat),
			MTP_double(lon)),
		MTP_messageMediaGeo(point.toMTP()),
		replyToMsgId,
		silent);
}

void EncryptedChats::sendVenue(
		not_null<SecretChatData*> chat,
		const Data::InputVenue &venue,
		MsgId replyToMsgId,
		bool silent) {
	const auto point = Data::LocationPoint(
		venue.lat,
		venue.lon,
		Data::LocationPoint::NoAccessHash);
	sendDecryptedMessage(
		chat,
		TextWithEntities(),
		decrypted::MTP_decryptedMessageMediaVenue(
			MTP_double(venue.lat),
			MTP_double(venue.lon),
			MTP_string(venue.title),
			MTP_string(venue.address),
			MTP_string(venue.provider),
			MTP_string(venue.id)),
		MTP_messageMediaVenue(
			point.toMTP(),
			MTP_string(venue.title),
			MTP_string(venue.address),
			MTP_string(venue.provider),
			MTP_string(venue.id),
			MTP_string()), // venue_type: no secret-layer field
		replyToMsgId,
		silent);
}

void EncryptedChats::sendContact(
		not_null<SecretChatData*> chat,
		const QString &phone,
		const QString &firstName,
		const QString &lastName,
		UserId userId,
		MsgId replyToMsgId,
		bool silent) {
	sendDecryptedMessage(
		chat,
		TextWithEntities(),
		decrypted::MTP_decryptedMessageMediaContact(
			MTP_string(phone),
			MTP_string(firstName),
			MTP_string(lastName),
			// user_id is int32 in the secret layer and int64 in the modern
			// messageMediaContact, so the wire value truncates here exactly
			// as the Android client's does. It is only a hint for the peer.
			MTP_int(int32(userId.bare))),
		MTP_messageMediaContact(
			MTP_string(phone),
			MTP_string(firstName),
			MTP_string(lastName),
			MTP_string(), // vcard: no secret-layer field
			MTP_long(userId.bare)),
		replyToMsgId,
		silent);
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
	// Local MsgId of the message this media replies to (if the compose bar had a
	// reply set for this chat); 0 otherwise. Carried into the wire message and
	// the local echo below, mirroring the text send path.
	const auto replyToMsgId = LocalReplyToMsgId(
		file->to.replyTo.messageId,
		historyPeerId);

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
	auto waveform = QByteArray();
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
						// Already in the 5-bit wire encoding.
						waveform = a.vwaveform().value_or_empty();
					}
				}, [](const auto &) {});
			}
		}, [](const auto &) {});
		asVideo = isVideo && !isAnimated && !isRound;
		// A round video note travels as a document carrying documentAttributeVideo
		// with the round_message flag (mirrors mobile: isRoundVideoDocument path).
		asRoundVideo = isVideo && !isAnimated && isRound;
	}
	// Build a small unencrypted preview thumb (<=90px JPEG) from the poster
	// frame so the peer's video / GIF bubble shows a poster immediately, like
	// mobile does (Android attaches one to every document kind that has it).
	auto videoThumb = PreparedThumb();
	if (!asPhotoRequested && !file->thumb.isNull()) {
		videoThumb = MakePhotoThumb(file->thumb);
	}
	const auto silent = file->to.options.silent;
	const auto groupedId = file->album ? file->album->groupId : uint64(0);

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
			// Per-account dir, wiped on logout (Storage::Account::reset); the
			// file is named by id only so the original name never leaks on disk.
			const auto dir = session->local().secretFilesPath();
			QDir().mkpath(dir);
			const auto path = dir + QString::number(fileId, 16);
			if (writeSecretFileEncrypted(path, plain)) {
				savedPath = path;
			}
		}

		const auto md5 = bigFile
			? QString()
			: QString::fromLatin1(prepared->filemd5);
		const auto mime = file->filemime;
		const auto filename = file->filename;
		const auto caption = PrepareConfirmedFileCaption(
		_session->data().history(chat->id),
		_session,
		file);
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
			// Upload cancelled (bubble destroyed) while the worker was still
			// encrypting: nothing was queued yet, so drop the encrypted copy
			// here instead of uploading + sending a file nobody asked for.
			// A photo has no bubble before this point (it is built below).
			if (!asPhoto && !session->data().message(itemId)) {
				if (!savedPath.isEmpty()) {
					QFile::remove(savedPath);
				}
				return;
			}
			const auto ttl = EffectiveMediaTtl(
				chat->ttl(),
				asVoice
					? voiceDuration
					: (asVideo || asRoundVideo)
					? videoDuration
					: 0);
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
				.replyToMsgId = replyToMsgId,
				.asPhoto = asPhoto,
				.width = asPhoto ? photoWidth : videoWidth,
				.height = asPhoto ? photoHeight : videoHeight,
				.thumbWidth = asPhoto ? thumb.width : videoThumb.width,
				.thumbHeight = asPhoto ? thumb.height : videoThumb.height,
				.thumb = asPhoto ? thumb.bytes : videoThumb.bytes,
				.asVideo = asVideo,
				.asRoundVideo = asRoundVideo,
				.asVoice = asVoice,
				.duration = asVoice ? voiceDuration : videoDuration,
				.waveform = waveform,
				.bigFile = bigFile,
				.cipherSize = cipherSize,
				.silent = silent,
				.groupedId = groupedId,
				.ttl = ttl,
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
					auto flags = MessageFlags(MessageFlag::Local
						| MessageFlag::Outgoing
						| MessageFlag::BeingSent);
					const auto replyTo = resolveLocalReply(
						historyPeerId,
						replyToMsgId,
						ttl,
						flags);
					// Short-ttl photo: covered on our side too (mobile
					// clients blur the sender's copy as well).
					const auto coverTtl = SecretChatMediaCoverTtl(
						ttl,
						photo,
						nullptr);
					history->addNewLocalMessage({
						.id = itemId.msg,
						.flags = flags,
						.from = session->userPeerId(),
						.replyTo = replyTo,
						.date = base::unixtime::now(),
						.groupedId = groupedId,
						.mediaSpoiler = (coverTtl > 0),
						.mediaTtlSeconds = coverTtl,
					}, photo, caption);
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
			noteMessageTtl(historyPeerId, itemId.msg, ttl);
			scheduleMessagesWrite();

			ensureUploadSubscribed();
			session->uploader().upload(itemId, prepared);
		});
	});
}

void EncryptedChats::sendExistingDocument(
		not_null<SecretChatData*> chat,
		not_null<DocumentData*> document,
		const TextWithEntities &caption,
		bool afterSetRefetch,
		MsgId replyToMsgId,
		Data::FileOrigin origin,
		bool silent) {
	DEBUG_LOG(("Secret Chat: sendExistingDocument id %1 mime %2 sticker %3."
		).arg(document->id
		).arg(document->mimeString()
		).arg(document->sticker() ? 1 : 0));
	if (queuePendingSend(chat, [=] {
		sendExistingDocument(
			chat,
			document,
			caption,
			afterSetRefetch,
			replyToMsgId,
			origin,
			silent);
	})) {
		return;
	}
	// We can only reference a document that actually lives on the server.
	if (!document->hasRemoteLocation()) {
		// A file forwarded out of another secret chat has no server id, only a
		// local encrypted copy -- re-upload its bytes instead of dropping it.
		// Only a sticker really needs the server reference.
		if (!document->sticker()) {
			uploadExistingDocument(
				chat,
				document,
				caption,
				replyToMsgId,
				origin,
				silent);
		} else {
			LOG(("Secret Chat Error: sticker has no remote location, "
				"not sent."));
		}
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
		uploadExistingDocument(
			chat,
			document,
			caption,
			replyToMsgId,
			origin,
			silent);
		return;
	}
	if (!shortName.isEmpty()) {
		// A sticker whose set short name is resolved: reference the public sticker
		// document directly (stickers are fetchable without a file_reference).
		sendExternalDocument(
			chat,
			document,
			caption,
			shortName,
			replyToMsgId,
			silent);
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
				sendExistingDocument(
					chat,
					document,
					caption,
					true,
					replyToMsgId,
					origin,
					silent);
			});
			return;
		}
		sendStickerAsDocument(chat, document, caption, replyToMsgId, silent);
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
			sendStickerAsDocument(chat, document, caption, replyToMsgId, silent);
		} else {
			sendExternalDocument(
				chat,
				document,
				caption,
				name,
				replyToMsgId,
				silent);
		}
	}).fail([=](const MTP::Error &error) {
		// The set is gone / inaccessible -> upload the actual file instead so
		// the sticker still arrives.
		LOG(("Secret Chat: getStickerSet failed (%1), uploading sticker file."
			).arg(error.type()));
		sendStickerAsDocument(chat, document, caption, replyToMsgId, silent);
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
		const TextWithEntities &caption,
		const QString &shortName,
		MsgId replyToMsgId,
		bool silent) {
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

	auto flags = decrypted::MTPDdecryptedMessage::Flags()
		| decrypted::MTPDdecryptedMessage::Flag::f_media;
	if (silent) {
		flags |= decrypted::MTPDdecryptedMessage::Flag::f_silent;
	}
	const auto replyToRandomId = ApplyReplyToRandomId(
		flags,
		randomIdByMsg(chat->id, replyToMsgId));
	const auto entities = ApplyEntities(flags, caption.entities);
	const auto message = decrypted::MTP_decryptedMessage(
		MTP_flags(flags),
		MTP_long(randomId),
		MTP_int(chat->ttl()),
		MTP_string(caption.text),
		media,
		entities,
		MTP_string(),
		MTP_long(replyToRandomId),
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

	// Optimistic local bubble (mirrors the incoming external-document path).
	const auto itemId = addLocalDocumentBubble(
		chat,
		document,
		caption,
		replyToMsgId);
	registerRandomId(chat->id, itemId.msg, randomId);
	noteMessageTtl(chat->id, itemId.msg, chat->ttl());
	scheduleMessagesWrite();

	// An external document carries no encrypted blob, so it rides the plain
	// messages.sendEncrypted transport (not sendEncryptedFile).
	const auto requestId = _mtp.request(MTPmessages_SendEncrypted(
		SendEncryptedFlags(silent),
		inputChat(chat),
		MTP_long(randomId),
		MTP_bytes(encrypted)
	)).done([=](const MTPmessages_SentEncryptedMessage &result) {
		DEBUG_LOG(("Secret Chat: sent sticker to chat %1, out_seq raw %2."
			).arg(chat->secretChatId()).arg(chat->rawOutSeqNo()));
		markSecretSent(itemId, SentDate(result));
	}).fail([=](const MTP::Error &error) {
		LOG(("Secret Chat Error: sendEncrypted (sticker) failed: %1"
			).arg(error.type()));
		markSecretSendFailed(itemId);
	}).afterRequest(sendAfter(chat->secretChatId())).send();
	setSendAfter(chat->secretChatId(), requestId);

	// out_seq_no advanced; persist so ordering survives a restart.
	writeLocal();

	chat->countKeyUseOut();
	maybeStartRekey(chat);
}

void EncryptedChats::sendStickerAsDocument(
		not_null<SecretChatData*> chat,
		not_null<DocumentData*> document,
		const TextWithEntities &caption,
		MsgId replyToMsgId,
		bool silent) {
	DEBUG_LOG(("Secret Chat: sendStickerAsDocument id %1 (set unresolved -> "
		"upload; animated renders as File on the peer).").arg(document->id));
	if (!chat->hasKey() || chat->state() != SecretChatState::Ready) {
		return;
	}
	const auto view = document->createMediaView();
	view->checkStickerLarge(); // ensure the sticker file is loading

	// Show the bubble right away (with the sticker's loading state) so a burst of
	// sends appears on click instead of all at once once the bytes resolve.
	const auto itemId = addLocalDocumentBubble(
		chat,
		document,
		caption,
		replyToMsgId);

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
			uploadStickerDocument(
				resolved,
				document,
				plain,
				caption,
				itemId,
				replyToMsgId,
				silent);
		}
		return true;
	};
	sendWhenDownloaded(trySend);
}

FullMsgId EncryptedChats::addLocalDocumentBubble(
		not_null<SecretChatData*> chat,
		not_null<DocumentData*> document,
		const TextWithEntities &caption,
		MsgId replyToMsgId) {
	// Local bubble: the original document, so it renders (and plays) inline on
	// desktop straight away (and, for a re-uploaded existing document, shows its
	// loading state while we fetch the bytes).
	ensureInDialogs(chat);
	const auto history = _session->data().history(chat->id);
	auto flags = MessageFlags(MessageFlag::Local
		| MessageFlag::Outgoing
		| MessageFlag::BeingSent);
	const auto replyTo = resolveLocalReply(
		chat->id,
		replyToMsgId,
		chat->ttl(),
		flags);
	// Short-ttl GIF / video: covered on our side too, like the file echo.
	const auto coverTtl = SecretChatMediaCoverTtl(
		chat->ttl(),
		nullptr,
		document);
	const auto item = history->addNewLocalMessage({
		.id = _session->data().nextLocalMessageId(),
		.flags = flags,
		.from = _session->userPeerId(),
		.replyTo = replyTo,
		.date = base::unixtime::now(),
		.mediaSpoiler = (coverTtl > 0),
		.mediaTtlSeconds = coverTtl,
	}, document, caption);
	return item->fullId();
}

void EncryptedChats::markSecretSent(FullMsgId itemId, TimeId date) {
	if (const auto item = _session->data().message(itemId)) {
		if (item->isSending()) {
			item->markSecretSent(date);
		}
	}
}

void EncryptedChats::markSecretSendFailed(FullMsgId itemId) {
	// Show the red "failed" mark, like a regular message the server rejected.
	// Nothing retries: the out_seq_no was consumed, so a peer that asks for it
	// gets a tombstone (see resendTombstone). The on-disk copy stays: the
	// bubble stays in the chat (and in the persisted blob) with that path, so
	// deleting it here would only leave a broken media after a restart -- it
	// goes when the item itself goes (ensureItemRemovedWatch).
	const auto item = _session->data().message(itemId);
	if (item && item->isSending()) {
		item->sendFailed();
	}
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
		const TextWithEntities &caption,
		int width,
		int height,
		Fn<void(OutgoingFile&)> applyKind,
		FullMsgId existing,
		MsgId replyToMsgId,
		bool silent) {
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
		: addLocalDocumentBubble(chat, document, caption, replyToMsgId);

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
		.replyToMsgId = replyToMsgId,
		.width = width,
		.height = height,
		.bigFile = bigFile,
		.silent = silent,
	};
	// Poster for the peer's bubble (GIF, forwarded video, sticker file); a
	// voice note has no image and stays without one, like Android.
	const auto thumb = MakePhotoThumb(DocumentThumbImage(document));
	file.thumb = thumb.bytes;
	file.thumbWidth = thumb.width;
	file.thumbHeight = thumb.height;
	applyKind(file);
	file.ttl = EffectiveMediaTtl(
		chat->ttl(),
		(file.asVoice || file.asRoundVideo || file.asVideo)
			? file.duration
			: 0);
	const auto ttl = file.ttl;
	_outgoingFiles[itemId] = std::move(file);
	registerRandomId(chat->id, itemId.msg, randomId);
	noteMessageTtl(chat->id, itemId.msg, ttl);
	scheduleMessagesWrite();

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
		const TextWithEntities &caption,
		FullMsgId existing,
		MsgId replyToMsgId,
		bool silent) {
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
		existing,
		replyToMsgId,
		silent);
}

void EncryptedChats::uploadExistingDocument(
		not_null<SecretChatData*> chat,
		not_null<DocumentData*> document,
		const TextWithEntities &caption,
		MsgId replyToMsgId,
		Data::FileOrigin origin,
		bool silent) {
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
	document->save(origin, QString(), LoadFromCloudOrLocal, true);

	// Show the bubble right away (with the document's loading state) so a burst
	// of sends appears on click instead of all at once once the bytes resolve.
	const auto itemId = addLocalDocumentBubble(
		chat,
		document,
		caption,
		replyToMsgId);

	const auto chatId = chat->secretChatId();
	const auto trySend = [=]() -> bool {
		const auto media = document->activeMediaView();
		const auto plain = media ? media->bytes() : QByteArray();
		if (plain.isEmpty()) {
			return false;
		}
		if (const auto resolved = _session->data().secretChatLoaded(
				secretChatIdFromWire(chatId))) {
			uploadDocumentFile(
				resolved,
				document,
				plain,
				caption,
				itemId,
				replyToMsgId,
				silent);
		}
		return true;
	};
	sendWhenDownloaded(trySend);
}

void EncryptedChats::uploadDocumentFile(
		not_null<SecretChatData*> chat,
		not_null<DocumentData*> document,
		const QByteArray &plain,
		const TextWithEntities &caption,
		FullMsgId existing,
		MsgId replyToMsgId,
		bool silent) {
	uploadEncryptedDocument(
		chat,
		document,
		plain,
		caption,
		document->dimensions.width(),
		document->dimensions.height(),
		[&](OutgoingFile &file) {
			file.duration = int(document->duration() / 1000);
			// A video with no known dimensions would go out as a 0x0
			// decryptedMessageMediaVideo, which renders broken on the peer;
			// keep it a plain file instead. The GIF path guards its own
			// attribute in fileUploadDone.
			const auto sized = (document->dimensions.width() > 0)
				&& (document->dimensions.height() > 0);
			switch (DocumentShape(document)) {
			case StoredMedia::Round: file.asRoundVideo = sized; break;
			case StoredMedia::Gif: file.asAnimation = true; break;
			case StoredMedia::Video: file.asVideo = sized; break;
			case StoredMedia::Voice:
				file.asVoice = true;
				if (const auto voice = document->voice()) {
					file.waveform = documentWaveformEncode5bit(
						voice->waveform);
				}
				break;
			// An image or a plain file goes out as a plain file.
			case StoredMedia::Image:
			case StoredMedia::File: break;
			}
		},
		existing,
		replyToMsgId,
		silent);
}

void EncryptedChats::fileUploadDone(const Storage::UploadSecureDone &done) {
	const auto i = _outgoingFiles.find(done.fullId);
	if (i == _outgoingFiles.end()) {
		return;
	}
	const auto info = i->second;
	_outgoingFiles.erase(i);

	// Bubble destroyed (upload cancelled) after the uploader already had the
	// parts: never send a message for it.
	if (!_session->data().message(done.fullId)) {
		if (!info.localPath.isEmpty()) {
			QFile::remove(info.localPath);
		}
		return;
	}

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
				MTP_string(info.caption.text));
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
				MTP_string(info.caption.text));
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
			// which send voice as a document carrying this attribute), plus the
			// waveform when known so the bubble is not flat.
			using Flag = decrypted::MTPDdocumentAttributeAudio::Flag;
			attributes.push_back(decrypted::MTP_documentAttributeAudio(
				MTP_flags(Flag::f_voice
					| (info.waveform.isEmpty() ? Flag() : Flag::f_waveform)),
				MTP_int(info.duration),
				MTP_string(),
				MTP_string(),
				MTP_bytes(info.waveform)));
		} else {
			attributes.push_back(decrypted::MTP_documentAttributeFilename(
				MTP_string(info.filename)));
		}
		return decrypted::MTP_decryptedMessageMediaDocument(
			MTP_bytes(info.thumb),
			MTP_int(info.thumbWidth),
			MTP_int(info.thumbHeight),
			MTP_string(info.mime),
			MTP_long(info.size),
			keyBytes,
			ivBytes,
			MTP_vector<decrypted::MTPDocumentAttribute>(attributes),
			MTP_string(info.caption.text));
	}();

	using MessageFlags = decrypted::MTPDdecryptedMessage::Flags;
	auto flags = MessageFlags()
		| decrypted::MTPDdecryptedMessage::Flag::f_media;
	if (info.silent) {
		flags |= decrypted::MTPDdecryptedMessage::Flag::f_silent;
	}
	if (info.groupedId) {
		flags |= decrypted::MTPDdecryptedMessage::Flag::f_grouped_id;
	}
	const auto replyToRandomId = ApplyReplyToRandomId(
		flags,
		randomIdByMsg(chat->id, info.replyToMsgId));
	const auto entities = ApplyEntities(flags, info.caption.entities);
	const auto message = decrypted::MTP_decryptedMessage(
		MTP_flags(flags),
		MTP_long(info.randomId),
		MTP_int(info.ttl),
		MTP_string(info.caption.text),
		media,
		entities,
		MTP_string(),
		MTP_long(replyToRandomId),
		MTP_long(info.groupedId));

	auto randomBytes = bytes::vector(16);
	bytes::set_random(randomBytes);
	const auto outSeqNo = chat->nextOutSeqNo();
	const auto layer = decrypted::MTP_decryptedMessageLayer(
		MTP_bytes(randomBytes),
		MTP_int(decrypted::details::kCurrentLayer),
		MTP_int(chat->currentInSeqNo()),
		MTP_int(outSeqNo),
		message);

	const auto serialized = MTP::SecretChat::SerializeObject(
		decrypted::MTPDecryptedMessageLayer(layer));
	// A file message is cached for resend only once the server answers: its
	// layer omits the file reference, which rides the outer inputEncryptedFile,
	// and the reference we can resend with (id + access_hash) comes back in
	// messages.sentEncryptedFile. See the done handler below.
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
		MTP_flags(info.silent
			? MTPmessages_SendEncryptedFile::Flag::f_silent
			: MTPmessages_SendEncryptedFile::Flag()),
		inputChat(chat),
		MTP_long(info.randomId),
		MTP_bytes(data),
		inputFile
	)).done([=](const MTPmessages_SentEncryptedMessage &result) {
		DEBUG_LOG(("Secret Chat: sent file to chat %1, out_seq raw %2."
			).arg(chat->secretChatId()).arg(chat->rawOutSeqNo()));
		// Now that the file has a server id, the message can answer a resend
		// request in full instead of falling back to a delete tombstone.
		// A message deleted while the send was in flight has nothing left
		// to resend: a later resend request gets a tombstone for its seq.
		result.match([&](const MTPDmessages_sentEncryptedFile &sent) {
			if (!_session->data().message(done.fullId)) {
				return;
			}
			sent.vfile().match([&](const MTPDencryptedFile &f) {
				rememberSentLayer(
					chat->secretChatId(),
					outSeqNo,
					serialized,
					info.randomId,
					/*isService=*/false,
					f.vid().v,
					f.vaccess_hash().v);
				writeLocal();
			}, [](const MTPDencryptedFileEmpty &) {
			});
		}, [](const MTPDmessages_sentEncryptedMessage &) {
		});
		markSecretSent(done.fullId, SentDate(result));
	}).fail([=](const MTP::Error &error) {
		LOG(("Secret Chat Error: sendEncryptedFile failed: %1"
			).arg(error.type()));
		markSecretSendFailed(done.fullId);
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
		scheduleMessagesWrite();
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
		markSecretSendFailed(itemId);
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

void EncryptedChats::sendExistingPhoto(
		not_null<SecretChatData*> chat,
		not_null<PhotoData*> photo,
		const SendAction &action,
		Data::FileOrigin origin,
		TextWithTags caption) {
	if (queuePendingSend(chat, [=] {
		sendExistingPhoto(chat, photo, action, origin, caption);
	})) {
		return;
	}
	// The picker fires as soon as a thumbnail is around, so the large size
	// usually still has to be downloaded. Keep a media view alive so the loaded
	// bytes survive until trySend reads them.
	const auto view = photo->createMediaView();
	view->wanted(Data::PhotoSize::Large, origin);

	const auto session = _session;
	const auto chatId = chat->secretChatId();
	const auto sendAction = action;
	const auto trySend = [=]() -> bool {
		auto plain = view->imageBytes(Data::PhotoSize::Large);
		if (plain.isEmpty()) {
			// A photo can resolve to a decoded image with no bytes kept (the
			// picker itself needs only the image), so re-encode it.
			const auto image = view->image(Data::PhotoSize::Large);
			if (!image) {
				return false;
			}
			auto buffer = QBuffer(&plain);
			buffer.open(QIODevice::WriteOnly);
			image->original().save(&buffer, "JPG", 87);
			buffer.close();
			if (plain.isEmpty()) {
				return false;
			}
		}
		if (!session->data().secretChatLoaded(secretChatIdFromWire(chatId))) {
			// The chat was discarded while the photo was downloading.
			return true;
		}
		session->api().sendFile(
			plain,
			SendMediaType::Photo,
			sendAction,
			caption);
		return true;
	};
	sendWhenDownloaded(trySend);
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
	startSelfDestructTimers(chat, false, tillDate, base::unixtime::now());
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
		if (!chat || chat->state() == SecretChatState::Discarded) {
			LOG(("Secret Chat Error: Message for unknown / closed chat %1."
				).arg(chatId));
			return;
		} else if (!chat->hasKey()) {
			// Our accept (or the peer's) is still in flight: hold the update
			// and replay it once the key lands (Android: pendingSecretMessages).
			// Persisted right away: the checkpoint advance below ACKs it, and
			// the server never resends an ACKed update, so a quit before the
			// key lands would otherwise lose it for good.
			const auto state = chat->state();
			if (state == SecretChatState::Requested
				|| state == SecretChatState::Waiting) {
				_pendingMessages[chat->secretChatId()].emplace_back(
					message,
					qts);
				writeLocal();
			} else {
				LOG(("Secret Chat Error: Message for keyless chat %1."
					).arg(chatId));
			}
			return;
		}
		auto decrypted = MTP::SecretChat::Decrypt(
			bytes::make_span(payload.v),
			chat->key(),
			chat->keyFingerprint(),
			chat->amCreator());
		auto usedNewKey = false;
		auto usedPreviousKey = false;
		if (decrypted) {
			// The peer has switched to the committed key: the retired one
			// can never be needed again (Android drops its future key here).
			if (chat->hasPreviousKey()) {
				chat->clearPreviousKey();
				writeLocal();
			}
		} else {
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
			// Or the peer still encrypts with the key we just retired (its
			// messages were in flight when our commitKey crossed them).
			if (!decrypted && chat->hasPreviousKey()) {
				decrypted = MTP::SecretChat::Decrypt(
					bytes::make_span(payload.v),
					chat->previousKey(),
					chat->previousKeyFingerprint(),
					chat->amCreator());
				usedPreviousKey = decrypted.has_value();
			}
			if (!decrypted) {
				LOG(("Secret Chat Error: Could not decrypt message."));
				return;
			}
		}
		// Parse the fixed header first: the seq numbers must be accounted for
		// even when the body carries a constructor we do not know (a newer
		// layer's media), or every later message looks like a gap and the
		// chat ends up discarded. Reject a header the mobile clients reject.
		const auto header = MTP::SecretChat::ParseLayerHeader(*decrypted);
		if (!header) {
			LOG(("Secret Chat Error: Could not parse decrypted layer."));
			return;
		} else if (!MTP::SecretChat::ValidateLayerHeader(
				*header,
				chat->amCreator())) {
			LOG(("Secret Chat Error: bad layer header in chat %1 "
				"(random_bytes %2, in %3, out %4)."
				).arg(chatId
				).arg(header->randomBytes.size()
				).arg(header->inSeqNo
				).arg(header->outSeqNo));
			return;
		}
		auto layer = decrypted::MTPDecryptedMessageLayer();
		if (!MTP::SecretChat::DeserializeObject(layer, *decrypted)) {
			// Keep the seq bookkeeping by substituting an empty service
			// message for the unreadable body (iOS drops just the body too).
			LOG(("Secret Chat Error: unsupported body in chat %1, out_seq %2."
				).arg(chatId).arg(header->outSeqNo));
			layer = decrypted::MTP_decryptedMessageLayer(
				MTP_bytes(header->randomBytes),
				MTP_int(header->layer),
				MTP_int(header->inSeqNo),
				MTP_int(header->outSeqNo),
				decrypted::MTP_decryptedMessageService(
					MTP_long(0),
					decrypted::MTP_decryptedMessageActionNoop()));
		}
		// A message under the just-committed or the retired key belongs to
		// another key's window, not the current key's PFS counter.
		const auto countKeyUse = !usedNewKey && !usedPreviousKey;
		layer.match([&](const decrypted::MTPDdecryptedMessageLayer &data) {
			receiveDecryptedLayer(chat, data, date, encryptedFile, countKeyUse);
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
		const MTPEncryptedFile *encryptedFile,
		bool countKeyUse) {
	const auto remoteOut = data.vout_seq_no().v;
	const auto haveIn = chat->inSeqNo();
	DEBUG_LOG(("Secret Chat: decrypted message for chat %1, "
		"remote out_seq %2, our in_seq %3."
		).arg(chat->secretChatId()).arg(remoteOut).arg(haveIn));
	if (remoteOut <= haveIn) {
		// Already accepted (the server re-delivers until ACKed and the startup
		// catch-up replays from our checkpoint), so ignore the duplicate.
		return;
	}
	// Count one decrypted incoming message against the current key and maybe
	// rotate (mirrors SecretChatHelper.java:818,1609) -- only for a message
	// actually new to us, so a getDifference replay cannot inflate the counter.
	if (countKeyUse) {
		chat->countKeyUseIn();
		maybeStartRekey(chat);
	}
	if (haveIn != remoteOut - 2) {
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
	// handleResend never goes below the peer's in_seq_no, so everything the
	// peer confirmed can leave the resend cache (keeps the persisted copy small).
	if (const auto i = _sentLayers.find(chat->secretChatId())
		; i != _sentLayers.end()) {
		auto &cache = i->second;
		cache.erase(cache.begin(), cache.lower_bound(chat->peerInSeqNo()));
	}
	// The advanced in_seq_no is persisted by setQts, after the message blob:
	// written here, before the message reaches disk, a crash in between
	// would leave a sequence that rejects the server's replay as a duplicate.
	processDecryptedMessage(chat, data.vmessage(), date, encryptedFile);
	drainHoles(chat);
}

void EncryptedChats::processDecryptedMessage(
		not_null<SecretChatData*> chat,
		const decrypted::MTPDecryptedMessage &message,
		TimeId date,
		const MTPEncryptedFile *encryptedFile,
		bool restoredRead,
		TimeId restoredDestroyAt) {
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
		// If this message replies to one we still have locally (its random_id
		// is indexed), resolve it so the bubble renders the quote.
		auto replyToMsgId = MsgId(0);
		if (const auto r = fields.vreply_to_random_id()) {
			replyToMsgId = findByRandomId(chat->id, uint64(r->v));
		}
		// Entities belong to `message`; a legacy media caption that replaces
		// an empty message below carries none.
		auto text = TextWithEntities{
			StripRtlOverride(qs(fields.vmessage())) };
		const auto silent = fields.is_silent();
		if (const auto entities = fields.ventities()) {
			text.entities = EntitiesFromDecrypted(_session, entities->v);
		}
		auto startedDownload = false;
		// Media the local echo can carry as-is, with nothing to download:
		// geo point, venue and contact. Stays empty for every other kind.
		auto localMedia = MTPMessageMedia(MTP_messageMediaEmpty());
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
				// The media shape decides when a self-destruct timer starts:
				// on open for a covered photo / video, on playback for a voice
				// or round video, on read for the rest (see IncomingFile).
				auto duration = 0;
				auto isVideo = false;
				auto isRound = false;
				auto isVoice = false;
				for (const auto &attribute : attributes) {
					attribute.match([&](const MTPDdocumentAttributeVideo &d) {
						isVideo = true;
						isRound = d.is_round_message();
						duration = int(d.vduration().v);
					}, [&](const MTPDdocumentAttributeAudio &d) {
						isVoice = d.is_voice();
						duration = d.vduration().v;
					}, [](const auto &) {
					});
				}
				const auto ttl = EffectiveMediaTtl(fields.vttl().v, duration);
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
					state->caption = Linkified(text);
					state->date = date;
					state->randomId = uint64(fields.vrandom_id().v);
					state->ttl = ttl;
					const auto mode = SelfDestructMode(
						ttl,
						asPhoto,
						isVideo,
						isRound,
						isVoice);
					state->coverOnOpen = mode.cover;
					state->contentUnread = mode.unread;
					state->replyToMsgId = replyToMsgId;
					state->silent = silent;
					state->groupedId = uint64(
						fields.vgrouped_id().value_or_empty());
					state->thumbBytes = thumb;
					state->thumbWidth = thumbWidth;
					state->thumbHeight = thumbHeight;
					state->read = restoredRead;
					state->ttlDestroyAt = restoredDestroyAt;
					state->message = message;
					state->file = *encryptedFile;
					_incomingFiles[state->chatId].push_back(state);
					// The record is message state: dirty from here, so a
					// chat-state write (a service action later in the same
					// batch) flushes it before persisting the sequence it
					// belongs to. Covers the placeholder below too, which is
					// serialized as this download.
					scheduleMessagesWrite();
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
			// Shared by the current document media and its layer 45..142
			// shape (decryptedMessageMediaDocument32, size:int): the two
			// differ only in the width of `size`.
			const auto handleDocument = [&](const auto &doc) {
				if (text.empty()) {
					text.text = StripRtlOverride(qs(doc.vcaption()));
				}
				auto attributes = ConvertDecryptedAttributes(
					doc.vattributes());
				auto mime = qs(doc.vmime_type());
				if (IsAnimatedStickerMime(mime)) {
					// Peer-supplied Lottie / WebM bytes cannot be verified
					// against a server set, so a document that claims to
					// be a sticker renders as a file, never as a sticker.
					// Both the attributes AND the mime have to go: the
					// decoders are chosen from the mime alone. A webm with
					// no sticker attribute is a plain video and stays one.
					const auto set = StripStickerAttributes(attributes);
					if (set || IsLottieStickerMime(mime)) {
						mime = QString::fromUtf8(kUnverifiedStickerMime);
					}
				}
				startMedia(
					bytes::make_span(doc.vkey().v),
					bytes::make_span(doc.viv().v),
					doc.vsize().v,
					mime,
					DocumentName(doc.vattributes()),
					std::move(attributes),
					false,
					AcceptedThumb(
						BytesToQ(bytes::make_span(doc.vthumb().v)),
						doc.vthumb_w().v,
						doc.vthumb_h().v,
						20000),
					doc.vthumb_w().v,
					doc.vthumb_h().v);
				if (!startedDownload) {
					// Fingerprint mismatch / empty file / bad key-iv:
					// fall back to a visible placeholder rather than a
					// silent empty bubble.
					const auto name = DocumentName(doc.vattributes());
					const auto info = name.isEmpty()
						? qs(doc.vmime_type())
						: name;
					const auto placeholder = u"[file] "_q
						+ info
						+ u" ("_q
						+ QString::number(doc.vsize().v)
						+ u" bytes)"_q;
					text = TextWithEntities{ text.empty()
						? placeholder
						: (placeholder + u"\n"_q + text.text) };
				}
			};
			media->match([&](
					const decrypted::MTPDdecryptedMessageMediaDocument &doc) {
				handleDocument(doc);
			}, [&](
					const decrypted::MTPDdecryptedMessageMediaDocument32 &doc) {
				// What iOS emits until it has processed our notifyLayer (it
				// negotiates from layer 73), and what a client that never got
				// past layer 101 always emits. Unparsed, the whole body became
				// a Noop stub and the document was lost without a trace.
				handleDocument(doc);
			}, [&](
					const decrypted::MTPDdecryptedMessageMediaPhoto &photo) {
				// Photos are always JPEG. MediaFile has no isImage->Photo
				// path, so finishFileDownload rebuilds this as a local
				// PhotoData (asPhoto=true) to render inline.
				if (text.empty()) {
					text.text = StripRtlOverride(qs(photo.vcaption()));
				}
				startMedia(
					bytes::make_span(photo.vkey().v),
					bytes::make_span(photo.viv().v),
					photo.vsize().v,
					u"image/jpeg"_q,
					u"photo_"_q + QString::number(randomId) + u".jpg"_q,
					QVector<MTPDocumentAttribute>(),
					true,
					AcceptedThumb(
						BytesToQ(bytes::make_span(photo.vthumb().v)),
						photo.vthumb_w().v,
						photo.vthumb_h().v,
						6000),
					photo.vthumb_w().v,
					photo.vthumb_h().v);
			}, [&](
					const decrypted::MTPDdecryptedMessageMediaVideo &video) {
				if (text.empty()) {
					text.text = StripRtlOverride(qs(video.vcaption()));
				}
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
					AcceptedThumb(
						BytesToQ(bytes::make_span(video.vthumb().v)),
						video.vthumb_w().v,
						video.vthumb_h().v,
						6000),
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
				const auto makeDocument = [&](
						const QVector<MTPDocumentAttribute> &attributes,
						const QString &mime) {
					// iOS sends date 0 here, and Data::Session drops every
					// field of a document dated 0 (mime, attributes, remote
					// location): fall back to the message date.
					return MTP_document(
						MTP_flags(0),
						ext.vid(),
						ext.vaccess_hash(),
						MTP_bytes(), // file_reference (stickers need none)
						MTP_int(ext.vdate().v ? ext.vdate().v : date),
						MTP_string(mime),
						MTP_long(ext.vsize().v),
						MTPVector<MTPPhotoSize>(),
						MTPVector<MTPVideoSize>(),
						ext.vdc_id(),
						MTP_vector<MTPDocumentAttribute>(attributes));
				};
				// An animated / video sticker is decoded from the referenced
				// bytes, so it is rendered as a sticker only once its set
				// confirms the document id (Android: stickerVerified). Until
				// then (or without a set) it is a plain file row: the sticker
				// attributes AND the mime are dropped, because the Lottie /
				// WebM decoders are picked from the mime alone.
				auto verify = std::optional<MTPInputStickerSet>();
				// The un-neutralized document, kept only to re-apply it once
				// the set confirms the id (so it is built only then).
				auto full = std::optional<MTPDocument>();
				const auto mime = qs(ext.vmime_type());
				// A document id we already hold as a real sticker came from
				// the server through a trusted path; neutralizing it would
				// only corrupt the user's own copy.
				const auto known = _session->data().document(ext.vid().v);
				const auto trusted = known->hasRemoteLocation()
					&& known->sticker();
				auto useMime = mime;
				if (!trusted && IsAnimatedStickerMime(mime)) {
					auto original = makeDocument(attributes, mime);
					const auto set = StripStickerAttributes(attributes);
					// A webm with no sticker attribute is a plain video:
					// nothing to verify, nothing to neutralize.
					if (set || IsLottieStickerMime(mime)) {
						if (set && set->type() != mtpc_inputStickerSetEmpty) {
							verify = *set;
							full = std::move(original);
						}
						useMime = QString::fromUtf8(kUnverifiedStickerMime);
					}
				}
				const auto document = _session->data().processDocument(
					makeDocument(attributes, useMime));
				ensureInDialogs(chat);
				const auto history = _session->data().history(chat->id);
				auto flags = MessageFlags(MessageFlag::Local
					| MessageFlag::ClientSideUnread);
				if (silent) {
					flags |= MessageFlag::Silent;
				}
				const auto replyTo = resolveLocalReply(
					chat->id,
					replyToMsgId,
					fields.vttl().v,
					flags);
				const auto item = history->addNewLocalMessage({
					.id = _session->data().nextLocalMessageId(),
					.flags = flags,
					.from = chat->user()->id,
					.replyTo = replyTo,
					.date = date,
				}, document, Linkified(text));
				registerRandomId(chat->id, item->id, randomId);
				noteMessageTtl(chat->id, item->id, fields.vttl().v);
				scheduleMessagesWrite();
				if (verify) {
					verifyExternalSticker(item->fullId(), *full, *verify);
				}
				startedDownload = true; // message already created
			}, [&](
					const decrypted::MTPDdecryptedMessageMediaGeoPoint &geo) {
				// HistoryItem::CreateMedia turns messageMediaGeo into a
				// MediaLocation, so the bubble needs nothing else. Note the
				// swap: the secret layer is lat, long; geoPoint is long, lat.
				localMedia = MTP_messageMediaGeo(
					Data::LocationPoint(
						geo.vlat().v,
						geo.vlong().v,
						Data::LocationPoint::NoAccessHash).toMTP());
			}, [&](
					const decrypted::MTPDdecryptedMessageMediaVenue &venue) {
				localMedia = MTP_messageMediaVenue(
					Data::LocationPoint(
						venue.vlat().v,
						venue.vlong().v,
						Data::LocationPoint::NoAccessHash).toMTP(),
					venue.vtitle(),
					venue.vaddress(),
					venue.vprovider(),
					venue.vvenue_id(),
					MTP_string()); // venue_type: no secret-layer field
			}, [&](
					const decrypted::MTPDdecryptedMessageMediaContact &c) {
				localMedia = MTP_messageMediaContact(
					c.vphone_number(),
					c.vfirst_name(),
					c.vlast_name(),
					MTP_string(), // vcard: no secret-layer field
					MTP_long(c.vuser_id().v));
			}, [&](
					const decrypted::MTPDdecryptedMessageMediaWebPage &page) {
				// Nothing to do: the message always carries the URL as its
				// text, and resolving the page would mean naming a link from
				// an end-to-end chat in a plaintext account.getWebPagePreview
				// request. The text alone renders correctly.
			}, [&](const decrypted::MTPDdecryptedMessageMediaEmpty &) {
				// Android sends this on a plain text: nothing to render.
			}, [&](const auto &) {
				if (text.empty()) {
					text = TextWithEntities{ u"[unsupported media]"_q };
				}
			});
		}
		if (!startedDownload) {
			addDecryptedMessage(
				chat,
				Linkified(text),
				date,
				false,
				uint64(fields.vrandom_id().v),
				fields.vttl().v,
				replyToMsgId,
				localMedia,
				silent);
		}
	}, [&](const decrypted::MTPDdecryptedMessageService &service) {
		service.vaction().match([&](
				const decrypted::MTPDdecryptedMessageActionSetMessageTTL &a) {
			// Same bounds as the mobile clients: off .. one year.
			const auto seconds = std::clamp(
				a.vttl_seconds().v,
				0,
				365 * 24 * 60 * 60);
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
				const auto randomId = uint64(rid.v);
				// A file still downloading has no message yet, so the id
				// lookup below misses it: cancel by random_id as well, or the
				// delete is lost and the media lands after it.
				cancelIncomingFiles(
					chat->secretChatId(),
					[&](const IncomingFile &file) {
						return (file.randomId == randomId);
					});
				const auto msgId = findByRandomId(chat->id, randomId);
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
			scheduleMessagesWrite();
		}, [&](
				const decrypted::MTPDdecryptedMessageActionFlushHistory &a) {
			_session->data().history(chat->id)->clear(
				History::ClearType::ClearHistory);
			historyCleared(chat);
		}, [&](
				const decrypted::MTPDdecryptedMessageActionScreenshotMessages &a) {
			addScreenshotNotice(chat, date);
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
		bool isService,
		uint64 fileId,
		uint64 fileAccessHash) {
	auto &cache = _sentLayers[chatId];
	auto entry = SentLayer();
	entry.serialized = bytes::make_vector(serialized);
	entry.randomId = randomId;
	entry.isService = isService;
	entry.fileId = fileId;
	entry.fileAccessHash = fileAccessHash;
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
	// Every buffered message behind the same gap lands here with the same
	// range; ask the peer once (Android requestedHoles). Ranges the peer has
	// already filled are dropped first so a later, genuinely new gap that
	// starts past them is still requested.
	auto &requested = _requestedHoles[chat->secretChatId()];
	for (auto i = requested.begin(); i != requested.end();) {
		if (i->second <= chat->inSeqNo()) {
			i = requested.erase(i);
		} else {
			++i;
		}
	}
	for (const auto &[start, end] : requested) {
		if (start <= startSeqNo && endSeqNo <= end) {
			return;
		}
	}
	requested[startSeqNo] = endSeqNo;
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
	// Only our own out_seq_no parity can be resent (see nextOutSeqNo: the
	// creator sends odd, the participant even). A start on the peer's parity
	// would make the whole loop below walk the peer's seq numbers, resending
	// nothing and answering with tombstones for messages that were never ours
	// (Android resendMessages bumps an even start for the admin).
	if ((startSeqNo % 2) != (chat->amCreator() ? 1 : 0)) {
		++startSeqNo;
	}
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
		: layer.fileId
		// Re-attach the file the message carried: it already lives on the
		// server, so it is referenced by id + access_hash, not re-uploaded.
		? _mtp.request(MTPmessages_SendEncryptedFile(
			MTP_flags(0),
			inputChat(chat),
			MTP_long(layer.randomId),
			MTP_bytes(data),
			MTP_inputEncryptedFile(
				MTP_long(layer.fileId),
				MTP_long(layer.fileAccessHash))
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
	// file whose send never came back). Occupy its out_seq_no with an empty delete
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

void EncryptedChats::cancelIncomingFiles(
		int32 chatId,
		Fn<bool(const IncomingFile&)> match) {
	const auto i = _incomingFiles.find(chatId);
	if (i == _incomingFiles.end()) {
		return;
	}
	auto &list = i->second;
	for (auto j = list.begin(); j != list.end();) {
		if (!match || match(**j)) {
			// The parts still on the wire hold their own reference and drop
			// out on the cancelled flag; the persisted blob forgets it now.
			(*j)->cancelled = true;
			j = list.erase(j);
		} else {
			++j;
		}
	}
	if (list.empty()) {
		_incomingFiles.erase(i);
	}
}

void EncryptedChats::dropIncomingFile(not_null<IncomingFile*> state) {
	const auto i = _incomingFiles.find(state->chatId);
	if (i == _incomingFiles.end()) {
		return;
	}
	auto &list = i->second;
	list.erase(
		ranges::remove_if(list, [&](const auto &entry) {
			return (entry.get() == state.get());
		}),
		list.end());
	if (list.empty()) {
		_incomingFiles.erase(i);
	}
}

void EncryptedChats::startFileDownload(std::shared_ptr<IncomingFile> state) {
	if (state->cancelled) {
		return;
	}
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
		if (state->cancelled) {
			return;
		}
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
		if (state->cancelled || ++state->attempts > kDownloadRetries) {
			return;
		}
		base::call_delayed(kDownloadRetryDelay * state->attempts, _session, [=] {
			startFileDownload(state);
		});
	}).toDC(MTP::downloadDcId(state->dcId, 0)).send();
}

HistoryItemCommonFields EncryptedChats::incomingFields(
		not_null<SecretChatData*> chat,
		const IncomingFile &state) {
	// A redone download whose placeholder was already viewed must not notify
	// again nor count as unread (restoreMessages never sets ClientSideUnread
	// at creation for the same reason).
	auto flags = MessageFlags(MessageFlag::Local);
	if (!state.read) {
		flags |= MessageFlag::ClientSideUnread;
	}
	const auto replyTo = resolveLocalReply(
		chat->id,
		state.replyToMsgId,
		state.ttl,
		flags);
	// Media whose timer starts on open / playback is marked unread-media
	// (the read path leaves it alone; ApiWrap::markContentsRead arms it),
	// and a covered one also gets the upstream view-once cover via the
	// media ttl.
	if (state.contentUnread) {
		flags |= MessageFlag::MediaIsUnread;
	}
	if (state.silent) {
		flags |= MessageFlag::Silent;
	}
	return HistoryItemCommonFields{
		.id = _session->data().nextLocalMessageId(),
		.flags = flags,
		.from = chat->user()->id,
		.replyTo = replyTo,
		.date = state.date,
		.groupedId = state.groupedId,
		.mediaSpoiler = state.coverOnOpen,
		.mediaTtlSeconds = state.coverOnOpen ? state.ttl : 0,
	};
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
	const auto caption = state->caption;

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
		incomingFields(chat, *state),
		document,
		caption);
	if (!item) {
		return;
	}
	state->pendingItemId = item->id;
	if (state->randomId) {
		registerRandomId(chat->id, item->id, state->randomId);
	}
	// A countdown that was already running resumes from its deadline (the
	// item's TTL timer cancels this download when it fires); otherwise the
	// pending ttl arms on the next read / open as usual.
	if (state->ttlDestroyAt > 0) {
		item->setSecretChatSelfDestructAt(state->ttlDestroyAt);
	} else {
		noteMessageTtl(chat->id, item->id, state->ttl);
	}
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
	// The download is over on every exit but a failed local write: the
	// message is persisted from its bubble from now on, or is gone for good.
	auto keep = false;
	const auto drop = gsl::finally([&] {
		if (!keep) {
			dropIncomingFile(state.get());
		}
	});
	if (state->cancelled) {
		// The message went away while the parts were in flight: writing the
		// file now would put back what removeSecretFile just deleted, and the
		// bubble built below would resurrect the message itself.
		return;
	}
	const auto chat = _session->data().secretChatLoaded(
		secretChatIdFromWire(state->chatId));
	if (!chat || chat->state() == SecretChatState::Discarded) {
		// The chat was closed while the download was in flight: adding the
		// message now would put the dead row back into the list.
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

	// The placeholder bubble is gone (destroyed by a path that does not cancel
	// the transfer, e.g. a History::clear): drop the file instead of building a
	// second bubble for a message the user has already seen disappear.
	if (state->pendingItemId
		&& !_session->data().message(
			FullMsgId(chat->id, state->pendingItemId))) {
		return;
	}

	const auto name = state->filename.isEmpty()
		? (u"file_"_q + QString::number(state->fileId))
		: state->filename;
	// Per-account dir, wiped on logout (Storage::Account::reset); named by
	// id only so the original name never leaks on disk.
	const auto dir = _session->local().secretFilesPath();
	QDir().mkpath(dir);
	const auto path = dir + QString::number(state->fileId, 16);
	if (!writeSecretFileEncrypted(path, plain)) {
		// A full disk / unwritable dir is recoverable: keep the record (it
		// is persisted as a pending download and redone on the next launch)
		// and retry the write from the ciphertext already in memory, with
		// the same bounds as a failed part request. Past the last attempt
		// the ciphertext stays in memory until a restart (at most
		// kMaxIncomingFileSize).
		LOG(("Secret Chat Error: could not write decrypted file."));
		keep = true;
		if (++state->attempts <= kDownloadRetries) {
			base::call_delayed(kDownloadRetryDelay * state->attempts, _session, [=] {
				finishFileDownload(state);
			});
		}
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
			scheduleMessagesWrite();
			return;
		}
	}

	ensureInDialogs(chat);
	const auto history = _session->data().history(chat->id);
	const auto caption = state->caption;

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
			item = history->addNewLocalMessage(
				incomingFields(chat, *state),
				photo,
				caption);
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
		item = history->addNewLocalMessage(
			incomingFields(chat, *state),
			document,
			caption);
	}
	if (state->randomId) {
		registerRandomId(chat->id, item->id, state->randomId);
	}
	if (state->ttlDestroyAt > 0) {
		item->setSecretChatSelfDestructAt(state->ttlDestroyAt);
	} else {
		noteMessageTtl(chat->id, item->id, state->ttl);
	}
	scheduleMessagesWrite();
}

void EncryptedChats::messagesRead(
		int32 chatId,
		TimeId maxDate,
		TimeId date) {
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
		scheduleMessagesWrite();
	}
	// The partner read our outgoing messages up to maxDate: start their
	// self-destruct timers (the sender's timer starts on the recipient's read).
	// The read happened at the update's server date, not when we received it
	// (a catch-up after being offline can deliver it much later), so count the
	// ttl from there (Android createTaskForSecretChat: max(max_date, date)).
	startSelfDestructTimers(chat, true, maxDate, std::max(maxDate, date));
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
		scheduleMessagesWrite();
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

FullMsgId EncryptedChats::addDecryptedMessage(
		not_null<SecretChatData*> chat,
		const TextWithEntities &text,
		TimeId date,
		bool outgoing,
		uint64 randomId,
		int32 ttlSeconds,
		MsgId replyToMsgId,
		const MTPMessageMedia &media,
		bool silent) {
	ensureInDialogs(chat);
	const auto history = _session->data().history(chat->id);
	const auto from = outgoing
		? _session->userPeerId()
		: chat->user()->id;
	auto flags = MessageFlags(MessageFlag::Local);
	if (silent) {
		flags |= MessageFlag::Silent; // popup without sound
	}
	if (outgoing) {
		// Shows the clock until messages.sendEncrypted confirms (or the red
		// mark if it fails); see markSecretSent / markSecretSendFailed.
		flags |= MessageFlag::Outgoing | MessageFlag::BeingSent;
	} else {
		// Marks the incoming message unread so the read path fires when the
		// chat is viewed (we then tell the partner via readHistory). Cleared
		// in Histories::readInboxTill once read.
		flags |= MessageFlag::ClientSideUnread;
	}
	// Text echoes stay copyable -> ttlSeconds 0 (no NoForwards).
	const auto replyTo = resolveLocalReply(chat->id, replyToMsgId, 0, flags);
	const auto item = history->addNewLocalMessage({
		.id = _session->data().nextLocalMessageId(),
		.flags = flags,
		.from = from,
		.replyTo = replyTo,
		.date = date,
	}, text, media);
	if (randomId) {
		registerRandomId(chat->id, item->id, randomId);
	}
	noteMessageTtl(chat->id, item->id, ttlSeconds);
	scheduleMessagesWrite();
	return item->fullId();
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

TextWithEntities EncryptedChats::screenshotNoticeText(
		not_null<SecretChatData*> chat) const {
	// Same name fallback chain as ttlNoticeText.
	const auto user = chat->user();
	auto from = user ? user->shortName() : QString();
	if (from.isEmpty() && user) {
		from = user->name();
	}
	if (from.isEmpty()) {
		from = chat->name();
	}
	return TextWithEntities{
		tr::lng_action_took_screenshot(tr::now, lt_from, from) };
}

not_null<HistoryItem*> EncryptedChats::buildScreenshotServiceMessage(
		not_null<SecretChatData*> chat,
		TimeId date,
		MsgId id) {
	// Incoming only: this client cannot detect its own screenshots. Local
	// without ClientSideUnread, so it fires no notification (like the TTL
	// notice); Android stores it as a dated service message too.
	const auto history = _session->data().history(chat->id);
	const auto item = history->makeMessage(
		HistoryItemCommonFields{
			.id = id,
			.flags = MessageFlags(MessageFlag::Local),
			.from = chat->user()->id,
			.date = date,
		},
		PreparedServiceText{ screenshotNoticeText(chat) });
	watchTtlNoticeNames(chat);
	return history->addNewLocalMessage(item);
}

void EncryptedChats::addScreenshotNotice(
		not_null<SecretChatData*> chat,
		TimeId date) {
	ensureInDialogs(chat);
	const auto item = buildScreenshotServiceMessage(
		chat,
		date,
		_session->data().nextLocalMessageId());
	// A random_id puts it in the persist index (serializeMessages walks that
	// map); it never gets a ttl entry, so no self-destruct guard is needed.
	registerRandomId(chat->id, item->id, base::RandomValue<uint64>());
	_screenshotNotices[chat->id].emplace(item->id);
	scheduleMessagesWrite();
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
	if (const auto s = _screenshotNotices.find(chat->id);
			s != _screenshotNotices.end()) {
		for (const auto msgId : s->second) {
			if (const auto item = _session->data().message(
					FullMsgId(chat->id, msgId))) {
				item->updateServiceText(
					PreparedServiceText{ screenshotNoticeText(chat) });
			}
		}
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
	scheduleMessagesWrite();
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
	scheduleMessagesWrite();
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
		TimeId tillDate,
		TimeId readTime) {
	const auto i = _messageTtls.find(chat->id);
	if (i == _messageTtls.end()) {
		return;
	}
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
		// Short-lived media that arms on open / playback (covered photo or
		// video, voice, round video with ttl <= 60) waits for contentRead()
		// (ours) or the partner's readMessages action (handleReadMessages),
		// not the chat read: the mobile clients exclude it from the max_date
		// read too, else our copy would burn before the partner ever opened
		// it. Anything longer-lived arms on the read, like text.
		if (SelfDestructMode(ttl, item).deferArm) {
			++j;
			continue;
		}
		// Read at readTime -> destroy ttl seconds after it (a past deadline
		// destroys at once). The owner's message-TTL timer owns it from here,
		// so drop the pending entry.
		item->setSecretChatSelfDestructAt(readTime + ttl);
		j = i->second.erase(j);
		armed = true;
	}
	if (i->second.empty()) {
		_messageTtls.erase(i);
	}
	if (armed) {
		scheduleMessagesWrite();
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

std::optional<uint64> EncryptedChats::randomIdByMsg(
		PeerId chatId,
		MsgId msgId) const {
	const auto i = _messageRandomIds.find(chatId);
	if (i != _messageRandomIds.end()) {
		const auto j = i->second.find(msgId);
		if (j != i->second.end()) {
			return j->second;
		}
	}
	return std::nullopt;
}

FullReplyTo EncryptedChats::resolveLocalReply(
		PeerId chatId,
		MsgId replyToMsgId,
		int32 ttlSeconds,
		MessageFlags &flags) const {
	// Self-destruct media must not be savable/shareable (matches the receive
	// path + Android client); NoForwards makes forbidsSaving() true. Text echoes
	// stay copyable -> callers pass ttlSeconds 0.
	if (ttlSeconds > 0) {
		flags |= MessageFlag::NoForwards;
	}
	auto result = FullReplyTo();
	if (replyToMsgId
		&& _session->data().message(FullMsgId(chatId, replyToMsgId))) {
		result.messageId = FullMsgId(chatId, replyToMsgId);
		flags |= MessageFlag::HasReplyInfo;
	}
	return result;
}

void EncryptedChats::deleteMessages(
		not_null<SecretChatData*> chat,
		const std::vector<MsgId> &localIds) {
	auto randomIds = QVector<MTPlong>();
	for (const auto &msgId : localIds) {
		// Read the random_id without dropping the registration: destroy() below
		// runs the itemRemoved hook, and that hook deletes the encrypted on-disk
		// copy only for a message it still tracks. Unregistering first left the
		// file (and its plaintext cache mirror) behind.
		if (const auto randomId = randomIdByMsg(chat->id, msgId)) {
			randomIds.push_back(MTP_long(*randomId));
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
	scheduleMessagesWrite();
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
	if (chat->state() != SecretChatState::Ready) {
		return;
	}
	// Recorded before the send, which persists the chat.
	chat->setAnnouncedLayer(decrypted::details::kCurrentLayer);
	sendServiceAction(
		chat,
		decrypted::MTP_decryptedMessageActionNotifyLayer(
			MTP_int(decrypted::details::kCurrentLayer)));
}

void EncryptedChats::notifyLayerIfOutdated() {
	_session->data().enumerateSecretChats([&](
			not_null<SecretChatData*> chat) {
		if (chat->state() == SecretChatState::Ready
			&& chat->announcedLayer() < decrypted::details::kCurrentLayer) {
			sendNotifyLayer(chat);
		}
	});
}

void EncryptedChats::flushRestoredPendingMessages() {
	_session->data().enumerateSecretChats([&](
			not_null<SecretChatData*> chat) {
		if (chat->hasKey() && chat->state() == SecretChatState::Ready) {
			flushPendingMessages(chat);
		}
	});
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
		// the smaller exchange_id is aborted; the larger one wins. Android
		// orders the ids as signed 64-bit values, so compare the same way, or
		// two ids of opposite sign make both sides keep their own request.
		if (int64(i->second.exchangeId) > int64(exchangeId)) {
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
	// Same restart hole as processUpdate(): a rekey we started before the
	// restart is restored, but the prime it needs to finish is not, and
	// without it the exchange is aborted instead of committed.
	if (!dhConfigReady()) {
		const auto copy = bytes::make_vector(gB);
		ensureDhConfig([=] {
			const auto still = _session->data().secretChatLoaded(
				secretChatIdFromWire(id));
			if (still && still->state() != SecretChatState::Discarded) {
				handleAcceptKey(still, exchangeId, copy, fingerprint);
			}
		});
		return;
	}
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
	// Confirm under the new key right away (Android sends a Noop after the
	// commit) so the requesting side can drop its retired key.
	sendServiceAction(chat, decrypted::MTP_decryptedMessageActionNoop());
}

void EncryptedChats::handleAbortKey(
		not_null<SecretChatData*> chat,
		uint64 exchangeId) {
	const auto i = _rekeys.find(chat->secretChatId());
	if (i != _rekeys.end() && i->second.exchangeId == exchangeId) {
		_rekeys.erase(i); // Drop the pending exchange; it will never commit.
		writeLocal();
		DEBUG_LOG(("Secret Chat: rekey aborted for chat %1, exchange_id %2."
			).arg(chat->secretChatId()).arg(exchangeId));
	}
}

void EncryptedChats::commitNewKey(
		not_null<SecretChatData*> chat,
		bytes::const_span newKey,
		uint64 fingerprint) {
	// Keep the retired key: the peer's messages already in flight (or sent
	// before our commitKey reaches it) still use it. newMessage() falls back
	// to it and drops it on the first message under the new key.
	chat->setPreviousKey(chat->key(), chat->keyFingerprint());
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
		const auto randomId = unregisterRandomId(peerId, msgId);
		if (!randomId) {
			return; // Not a tracked secret-chat message.
		}
		const auto chatId = int32(peerToSecretChat(peerId).bare);
		// Self-destructed / deleted / history cleared: the encrypted on-disk
		// copy (and its cache mirror) must not outlive the message.
		removeSecretFile(item);
		// Nor may a download still on the wire write that copy back.
		cancelIncomingFiles(chatId, [&](const IncomingFile &file) {
			return (file.pendingItemId == msgId);
		});
		// Nor may the resend cache keep (and persist) its text and file keys.
		retireSentLayers(chatId, [&](const SentLayer &layer) {
			return (layer.randomId == *randomId);
		});
		if (const auto k = _messageTtls.find(peerId); k != _messageTtls.end()) {
			k->second.remove(msgId);
			if (k->second.empty()) {
				_messageTtls.erase(k);
			}
		}
		scheduleMessagesWrite();
	}, _lifetime);
}

void EncryptedChats::flushPendingMessages(not_null<SecretChatData*> chat) {
	const auto i = _pendingMessages.find(chat->secretChatId());
	if (i == _pendingMessages.end()) {
		return;
	}
	// The list stays in place, and in every chat-state write a service
	// action below triggers, until all of it is processed: a quit mid-way
	// then leaves the whole list for the startup replay (its duplicates are
	// dropped by seq / random_id) instead of losing the unprocessed tail.
	// Only called once the key is set, so newMessage never re-parks into it.
	const auto held = i->second;
	for (const auto &[message, qts] : held) {
		newMessage(message, qts);
	}
	_pendingMessages.remove(chat->secretChatId());
	// The checkpoint already passed these qts when they were parked, so the
	// setQts inside newMessage wrote nothing: persist the replayed messages
	// and the advanced sequence (and the now-empty parked list) here.
	writeMessagesLocal();
	writeLocal();
}

const EncryptedChats::Pending *EncryptedChats::pending(int32 chatId) const {
	const auto i = _pending.find(chatId);
	return (i != _pending.end()) ? &i->second : nullptr;
}

void EncryptedChats::restorePending(int32 chatId, Pending pending) {
	_pending[chatId] = std::move(pending);
}

const EncryptedChats::Rekey *EncryptedChats::rekey(int32 chatId) const {
	const auto i = _rekeys.find(chatId);
	return (i != _rekeys.end()) ? &i->second : nullptr;
}

void EncryptedChats::restoreRekey(int32 chatId, Rekey rekey) {
	_rekeys[chatId] = std::move(rekey);
}

const std::map<int32, EncryptedChats::SentLayer> *EncryptedChats::sentLayers(
		int32 chatId) const {
	const auto i = _sentLayers.find(chatId);
	return (i != _sentLayers.end() && !i->second.empty())
		? &i->second
		: nullptr;
}

void EncryptedChats::restoreSentLayers(
		int32 chatId,
		std::map<int32, SentLayer> layers) {
	if (!layers.empty()) {
		_sentLayers[chatId] = std::move(layers);
	}
}

auto EncryptedChats::pendingMessages(int32 chatId) const
-> const std::vector<PendingMessage>* {
	const auto i = _pendingMessages.find(chatId);
	return (i != _pendingMessages.end() && !i->second.empty())
		? &i->second
		: nullptr;
}

void EncryptedChats::restorePendingMessages(
		int32 chatId,
		std::vector<PendingMessage> messages) {
	if (!messages.empty()) {
		_pendingMessages[chatId] = std::move(messages);
	}
}

void EncryptedChats::retireSentLayers(
		int32 chatId,
		Fn<bool(const SentLayer&)> match) {
	const auto i = _sentLayers.find(chatId);
	if (i == _sentLayers.end()) {
		return;
	}
	auto retired = false;
	for (auto j = i->second.begin(); j != i->second.end();) {
		if (!j->second.isService && match(j->second)) {
			j = i->second.erase(j);
			retired = true;
		} else {
			++j;
		}
	}
	if (retired) {
		scheduleLocalWrite();
	}
}

void EncryptedChats::historyCleared(not_null<SecretChatData*> chat) {
	cancelIncomingFiles(chat->secretChatId());
	retireSentLayers(chat->secretChatId(), [](const SentLayer &) {
		return true;
	});
	clearChatRandomIds(chat->id);
	_messageTtls.remove(chat->id);
	_ttlNotices.remove(chat->id);
	_screenshotNotices.remove(chat->id);
	scheduleMessagesWrite();
}

void EncryptedChats::contentRead(
		not_null<SecretChatData*> chat,
		const std::vector<not_null<HistoryItem*>> &items) {
	// The media was opened / played: its self-destruct starts now, on both
	// sides. Tell the partner which messages (by random_id) so their copies'
	// timers start too (mirrors Android's sendSecretMessageRead / the
	// media-consumed readMessages action).
	const auto now = base::unixtime::now();
	const auto k = _messageTtls.find(chat->id);
	auto randomIds = QVector<MTPlong>();
	randomIds.reserve(items.size());
	for (const auto &item : items) {
		if (const auto randomId = randomIdByMsg(chat->id, item->id)) {
			randomIds.push_back(MTP_long(*randomId));
		}
		if (k == _messageTtls.end()) {
			continue;
		} else if (const auto j = k->second.find(item->id);
				j != k->second.end()) {
			item->setSecretChatSelfDestructAt(now + j->second);
			k->second.erase(j);
			scheduleMessagesWrite();
		}
	}
	if (k != _messageTtls.end() && k->second.empty()) {
		_messageTtls.erase(k);
	}
	if (!randomIds.isEmpty()) {
		sendServiceAction(
			chat,
			decrypted::MTP_decryptedMessageActionReadMessages(
				MTP_vector<MTPlong>(std::move(randomIds))));
	}
}

void EncryptedChats::removeSecretFile(not_null<const HistoryItem*> item) {
	const auto media = item->media();
	if (!media) {
		return;
	}
	auto path = QString();
	if (const auto document = media->document()) {
		path = document->secretEncryptedPath();
		if (!path.isEmpty()) {
			// The plaintext mirror in the cache database must go with it.
			_session->data().cache().remove(document->cacheKey());
		}
	} else if (const auto photo = media->photo()) {
		path = photo->secretEncryptedPath();
	}
	if (!path.isEmpty()) {
		QFile::remove(path);
	}
}

void EncryptedChats::verifyExternalSticker(
		FullMsgId itemId,
		const MTPDocument &document,
		const MTPInputStickerSet &set) {
	// Fetch the set and render the sticker only if the peer's document id is
	// really a member of it; otherwise the message stays a plain file row.
	_mtp.request(MTPmessages_GetStickerSet(
		set,
		MTP_int(0) // hash
	)).done([=](const MTPmessages_StickerSet &result) {
		const auto item = _session->data().message(itemId);
		if (!item) {
			return;
		}
		const auto id = document.match([](const auto &data) {
			return data.vid().v;
		});
		result.match([&](const MTPDmessages_stickerSet &data) {
			_session->data().stickers().feedSetFull(data);
			for (const auto &member : data.vdocuments().v) {
				const auto memberId = member.match([](const auto &d) {
					return d.vid().v;
				});
				if (memberId != id) {
					continue;
				}
				// Verified: give the shared DocumentData its sticker attributes
				// back and re-render the bubble as a sticker.
				_session->data().processDocument(document);
				item->history()->owner().requestItemViewRefresh(item);
				scheduleMessagesWrite();
				return;
			}
			LOG(("Secret Chat Error: sticker %1 is not in its set, kept as "
				"a file.").arg(id));
		}, [](const MTPDmessages_stickerSetNotModified &) {
		});
	}).fail([=](const MTP::Error &error) {
		LOG(("Secret Chat: sticker set verification failed (%1)."
			).arg(error.type()));
	}).send();
}

void EncryptedChats::ensureInDialogs(not_null<SecretChatData*> chat) {
	if (chat->state() == SecretChatState::Discarded) {
		// Never resurrect a closed EMPTY chat into the dialog list: a late or
		// replayed update for an already-discarded chat must not re-add its
		// row. A cancelled chat that still has messages keeps its row (see
		// History::shouldBeInChatList), including after a restart, where
		// restoreMessages brings us here with the history already filled.
		const auto loaded = _session->data().historyLoaded(chat->id);
		if (!loaded || !loaded->lastMessage()) {
			return;
		}
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
		// chat a date so it shows in the list before the first message: the
		// key creation time (persisted, so a restored empty chat keeps its
		// row position across launches), else now.
		history->setChatListTimeId(chat->keyCreationDate()
			? chat->keyCreationDate()
			: base::unixtime::now());
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
