/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/

// Standalone unit tests for the secret-chat (MTProto end-to-end) crypto core.
//
// These cover the deterministic, host-runnable pieces -- key fingerprint,
// message encryption / decryption (including the creator vs participant `x`
// offset), tamper detection, and the decrypted-layer TL serialization
// round-trip. No network, no Main::Session: just the pure functions.
//
// Catch2 is not vendored in this repository (only the external desktop-app
// libraries pull it in), so this uses a tiny self-contained harness. Built
// only with -DDESKTOP_APP_TEST_APPS=ON; run the produced ./test_secret_chat.

#include "mtproto/secret_chat/secret_chat_encryption.h"

#include "base/bytes.h"
#include "base/openssl_help.h"

#include "storage/storage_encrypted_file.h"
#include "storage/storage_encryption.h"

#include "secret_scheme.h"

#include <QtCore/QByteArray>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>

#include <iostream>
#include <string>

namespace base::assertion {

// For Expects() / Ensures() inside the tested code to link.
void log(const char *message, const char *file, int line) {
	std::cerr << "Assertion: " << message
		<< " (" << file << ":" << line << ")" << std::endl;
}

} // namespace base::assertion

namespace {

int gChecks = 0;
int gFailures = 0;

void Check(bool condition, const std::string &what) {
	++gChecks;
	if (!condition) {
		++gFailures;
		std::cerr << "  FAIL: " << what << std::endl;
	}
}

// A deterministic 256-byte auth key (byte i == i & 0xFF).
[[nodiscard]] bytes::vector MakeKey(uchar seed = 0) {
	auto result = bytes::vector(MTP::SecretChat::kKeySize);
	for (auto i = 0; i != int(result.size()); ++i) {
		result[i] = static_cast<gsl::byte>((i + seed) & 0xFF);
	}
	return result;
}

// A 4-byte-aligned plaintext payload (Encrypt requires TL-word alignment).
[[nodiscard]] bytes::vector MakePayload(int words) {
	auto result = bytes::vector(words * 4);
	bytes::set_random(result);
	return result;
}

void TestFingerprint() {
	std::cout << "fingerprint" << std::endl;

	const auto key = MakeKey();
	// SHA1(bytes 0..255) low64 (bytes 12..19, little-endian), computed with an
	// independent reference (Python hashlib).
	const auto expected = uint64(0xc8df57a46e58d132ULL);
	Check(
		MTP::SecretChat::ComputeKeyFingerprint(key) == expected,
		"known fingerprint vector matches");
}

void TestRoundTrip() {
	std::cout << "encrypt / decrypt round-trip" << std::endl;

	const auto key = MakeKey();
	const auto fp = MTP::SecretChat::ComputeKeyFingerprint(key);
	const auto payload = MakePayload(8);

	// Creator sends (x=0); the other participant decrypts (also x=0 on the
	// receive side). Both directions must round-trip.
	for (const auto creatorSends : { true, false }) {
		const auto encrypted = MTP::SecretChat::Encrypt(
			payload,
			key,
			fp,
			creatorSends);
		// The receiver is the opposite role.
		const auto decrypted = MTP::SecretChat::Decrypt(
			encrypted,
			key,
			fp,
			!creatorSends);
		Check(decrypted.has_value(), "decrypt succeeds for opposite role");
		if (decrypted) {
			Check(
				decrypted->size() >= payload.size(),
				"decrypted payload includes the plaintext");
			Check(
				bytes::compare(
					bytes::make_span(*decrypted).subspan(0, payload.size()),
					payload) == 0,
				"decrypted prefix equals the original plaintext");
		}
	}
}

void TestWrongRoleFails() {
	std::cout << "wrong-role decryption is rejected" << std::endl;

	const auto key = MakeKey();
	const auto fp = MTP::SecretChat::ComputeKeyFingerprint(key);
	const auto payload = MakePayload(8);

	const auto encrypted = MTP::SecretChat::Encrypt(payload, key, fp, true);
	// Decrypting with the SAME role uses the wrong `x` -> msg_key mismatch.
	const auto decrypted = MTP::SecretChat::Decrypt(encrypted, key, fp, true);
	Check(!decrypted.has_value(), "same-role decrypt is rejected");
}

void TestTamperFails() {
	std::cout << "tampered ciphertext and bad fingerprint are rejected"
		<< std::endl;

	const auto key = MakeKey();
	const auto fp = MTP::SecretChat::ComputeKeyFingerprint(key);
	const auto payload = MakePayload(8);

	auto encrypted = MTP::SecretChat::Encrypt(payload, key, fp, true);

	// Flip a bit in the ciphertext body (past the 24-byte prefix).
	auto tampered = encrypted;
	tampered[30] = static_cast<gsl::byte>(
		std::to_integer<uchar>(tampered[30]) ^ 0x40);
	Check(
		!MTP::SecretChat::Decrypt(tampered, key, fp, false).has_value(),
		"tampered ciphertext is rejected");

	// A mismatched key fingerprint is rejected before any decryption.
	Check(
		!MTP::SecretChat::Decrypt(encrypted, key, fp + 1, false).has_value(),
		"mismatched fingerprint is rejected");

	// Decrypting with a different key fails the msg_key check.
	const auto otherKey = MakeKey(1);
	Check(
		!MTP::SecretChat::Decrypt(encrypted, otherKey, fp, false).has_value(),
		"decryption with the wrong key is rejected");
}

void TestTlRoundTrip() {
	std::cout << "decrypted-layer TL serialize / deserialize round-trip"
		<< std::endl;

	auto randomBytes = bytes::vector(16);
	bytes::set_random(randomBytes);

	const auto text = QString::fromUtf8("hello, secret \xF0\x9F\x94\x92");
	const auto randomId = uint64(0x0102030405060708ULL);
	const auto inSeq = 4;
	const auto outSeq = 7;

	using MessageFlags = decrypted::MTPDdecryptedMessage::Flags;
	const auto message = decrypted::MTP_decryptedMessage(
		MTP_flags(MessageFlags()),
		MTP_long(randomId),
		MTP_int(0),
		MTP_string(text),
		decrypted::MTPDecryptedMessageMedia(),
		decrypted::MTPVector<decrypted::MTPMessageEntity>(),
		MTP_string(),
		MTP_long(0),
		MTP_long(0));
	const auto layer = decrypted::MTP_decryptedMessageLayer(
		MTP_bytes(QByteArray(
			reinterpret_cast<const char*>(randomBytes.data()),
			int(randomBytes.size()))),
		MTP_int(73),
		MTP_int(inSeq),
		MTP_int(outSeq),
		message);

	// The layer must be serialized boxed (constructor id on the wire).
	const auto serialized = MTP::SecretChat::SerializeObject(
		decrypted::MTPDecryptedMessageLayer(layer));
	Check(
		(serialized.size() % sizeof(mtpPrime)) == 0,
		"serialized layer is TL-word aligned");

	auto parsed = decrypted::MTPDecryptedMessageLayer();
	const auto ok = MTP::SecretChat::DeserializeObject(parsed, serialized);
	Check(ok, "layer deserializes");
	if (!ok) {
		return;
	}
	parsed.match([&](const decrypted::MTPDdecryptedMessageLayer &data) {
		Check(data.vin_seq_no().v == inSeq, "in_seq_no preserved");
		Check(data.vout_seq_no().v == outSeq, "out_seq_no preserved");
		Check(data.vlayer().v == 73, "layer number preserved");
		data.vmessage().match([&](
				const decrypted::MTPDdecryptedMessage &fields) {
			Check(
				qs(fields.vmessage()) == text,
				"message text preserved");
			Check(
				uint64(fields.vrandom_id().v) == randomId,
				"random_id preserved");
		}, [&](const decrypted::MTPDdecryptedMessageService &) {
			Check(false, "unexpected service message");
		});
	});
}

// The layer 45..142 document media (decryptedMessageMediaDocument32,
// size:int) is what iOS emits until it has processed our notifyLayer. It must
// keep its canonical wire id and parse, instead of turning the whole body into
// the Noop stub that silently drops the document.
void TestLegacyDocumentMedia() {
	std::cout << "layer-73 document media (size:int) keeps its id and parses"
		<< std::endl;

	Check(
		uint32(decrypted::mtpc_decryptedMessageMediaDocument32) == 0x7afe8ae2U,
		"legacy document media keeps the canonical wire id");

	const auto size = 1234;
	auto attributes = QVector<decrypted::MTPDocumentAttribute>();
	attributes.push_back(decrypted::MTP_documentAttributeFilename(
		MTP_string(QString::fromLatin1("a.bin"))));
	const auto media = decrypted::MTP_decryptedMessageMediaDocument32(
		MTP_bytes(QByteArray()),
		MTP_int(0),
		MTP_int(0),
		MTP_string(QString::fromLatin1("application/octet-stream")),
		MTP_int(size),
		MTP_bytes(QByteArray(32, 'k')),
		MTP_bytes(QByteArray(32, 'i')),
		MTP_vector<decrypted::MTPDocumentAttribute>(attributes),
		MTP_string(QString()));
	using MessageFlags = decrypted::MTPDdecryptedMessage::Flags;
	const auto message = decrypted::MTP_decryptedMessage(
		MTP_flags(MessageFlags()
			| decrypted::MTPDdecryptedMessage::Flag::f_media),
		MTP_long(1),
		MTP_int(0),
		MTP_string(QString()),
		media,
		decrypted::MTPVector<decrypted::MTPMessageEntity>(),
		MTP_string(QString()),
		MTP_long(0),
		MTP_long(0));
	auto randomBytes = bytes::vector(16);
	bytes::set_random(randomBytes);
	const auto layer = decrypted::MTP_decryptedMessageLayer(
		MTP_bytes(QByteArray(
			reinterpret_cast<const char*>(randomBytes.data()),
			int(randomBytes.size()))),
		MTP_int(73),
		MTP_int(0),
		MTP_int(1),
		message);
	const auto serialized = MTP::SecretChat::SerializeObject(
		decrypted::MTPDecryptedMessageLayer(layer));

	auto parsed = decrypted::MTPDecryptedMessageLayer();
	const auto ok = MTP::SecretChat::DeserializeObject(parsed, serialized);
	Check(ok, "layer with the legacy document media deserializes");
	if (!ok) {
		return;
	}
	auto seen = false;
	parsed.match([&](const decrypted::MTPDdecryptedMessageLayer &data) {
		data.vmessage().match([&](
				const decrypted::MTPDdecryptedMessage &fields) {
			const auto m = fields.vmedia();
			Check(m != nullptr, "legacy document media is present");
			if (!m) {
				return;
			}
			m->match([&](
					const decrypted::MTPDdecryptedMessageMediaDocument32 &doc) {
				seen = true;
				Check(doc.vsize().v == size, "legacy document size preserved");
				Check(
					doc.vkey().v.size() == 32 && doc.viv().v.size() == 32,
					"legacy document key / iv preserved");
			}, [](const auto &) {
			});
		}, [&](const decrypted::MTPDdecryptedMessageService &) {
			Check(false, "unexpected service message");
		});
	});
	Check(seen, "legacy document media constructor recognized");
}

[[nodiscard]] decrypted::MTPDecryptedMessageLayer MakeLayer(
		const QString &text,
		uint64 randomId,
		int inSeq,
		int outSeq) {
	auto randomBytes = bytes::vector(16);
	bytes::set_random(randomBytes);
	using MessageFlags = decrypted::MTPDdecryptedMessage::Flags;
	const auto message = decrypted::MTP_decryptedMessage(
		MTP_flags(MessageFlags()),
		MTP_long(randomId),
		MTP_int(0),
		MTP_string(text),
		decrypted::MTPDecryptedMessageMedia(),
		decrypted::MTPVector<decrypted::MTPMessageEntity>(),
		MTP_string(),
		MTP_long(0),
		MTP_long(0));
	return decrypted::MTPDecryptedMessageLayer(decrypted::MTP_decryptedMessageLayer(
		MTP_bytes(QByteArray(
			reinterpret_cast<const char*>(randomBytes.data()),
			int(randomBytes.size()))),
		MTP_int(73),
		MTP_int(inSeq),
		MTP_int(outSeq),
		message));
}

// The full send-path pipeline: serialize a layer, Encrypt it, Decrypt as the
// opposite role, then DeserializeObject. This is what crosses the wire to a
// mobile client; it must survive the int32 length prefix + padding framing.
// (A missing length prefix here is what broke real-device interop.)
void TestEndToEndPipeline() {
	std::cout << "end-to-end encrypt -> decrypt -> parse pipeline" << std::endl;

	const auto key = MakeKey();
	const auto fp = MTP::SecretChat::ComputeKeyFingerprint(key);
	const auto text = QString::fromUtf8("interop \xF0\x9F\x94\x90 check");
	const auto randomId = uint64(0xDEADBEEFCAFEF00DULL);

	for (const auto creatorSends : { true, false }) {
		const auto layer = MakeLayer(text, randomId, 0, creatorSends ? 0 : 1);
		const auto serialized = MTP::SecretChat::SerializeObject(layer);
		const auto encrypted = MTP::SecretChat::Encrypt(
			serialized,
			key,
			fp,
			creatorSends);
		const auto decrypted = MTP::SecretChat::Decrypt(
			encrypted,
			key,
			fp,
			!creatorSends);
		Check(decrypted.has_value(), "pipeline decrypt succeeds");
		if (!decrypted) {
			continue;
		}
		// Decrypt must return exactly the serialized object (prefix + padding
		// stripped), so the buffer round-trips byte-for-byte.
		Check(
			decrypted->size() == serialized.size(),
			"decrypted size equals the serialized object size");
		auto parsed = decrypted::MTPDecryptedMessageLayer();
		const auto ok = MTP::SecretChat::DeserializeObject(parsed, *decrypted);
		Check(ok, "pipeline layer parses");
		if (!ok) {
			continue;
		}
		parsed.match([&](const decrypted::MTPDdecryptedMessageLayer &data) {
			data.vmessage().match([&](
					const decrypted::MTPDdecryptedMessage &fields) {
				Check(qs(fields.vmessage()) == text, "pipeline text preserved");
				Check(
					uint64(fields.vrandom_id().v) == randomId,
					"pipeline random_id preserved");
			}, [&](const decrypted::MTPDdecryptedMessageService &) {
				Check(false, "unexpected service message in pipeline");
			});
		});
	}
}

// The resend service action (decryptedMessageActionResend) is what we send to
// ask a peer to retransmit a missing out_seq_no range, and what we answer when
// the peer asks us. Its wire format must survive the full crypto pipeline, so a
// gap recovery actually carries the right [start, end] range across to mobile.
void TestResendActionPipeline() {
	std::cout << "resend service action encrypt -> decrypt -> parse"
		<< std::endl;

	const auto key = MakeKey();
	const auto fp = MTP::SecretChat::ComputeKeyFingerprint(key);
	const auto randomId = uint64(0x1122334455667788ULL);
	const auto startSeq = 4;
	const auto endSeq = 10;

	auto randomBytes = bytes::vector(16);
	bytes::set_random(randomBytes);
	const auto service = decrypted::MTP_decryptedMessageService(
		MTP_long(randomId),
		decrypted::MTP_decryptedMessageActionResend(
			MTP_int(startSeq),
			MTP_int(endSeq)));
	const auto layer = decrypted::MTPDecryptedMessageLayer(
		decrypted::MTP_decryptedMessageLayer(
			MTP_bytes(QByteArray(
				reinterpret_cast<const char*>(randomBytes.data()),
				int(randomBytes.size()))),
			MTP_int(73),
			MTP_int(0),
			MTP_int(1),
			service));

	const auto serialized = MTP::SecretChat::SerializeObject(layer);
	const auto encrypted = MTP::SecretChat::Encrypt(serialized, key, fp, true);
	const auto decrypted = MTP::SecretChat::Decrypt(encrypted, key, fp, false);
	Check(decrypted.has_value(), "resend action decrypts");
	if (!decrypted) {
		return;
	}
	auto parsed = decrypted::MTPDecryptedMessageLayer();
	const auto ok = MTP::SecretChat::DeserializeObject(parsed, *decrypted);
	Check(ok, "resend layer parses");
	if (!ok) {
		return;
	}
	parsed.match([&](const decrypted::MTPDdecryptedMessageLayer &data) {
		data.vmessage().match([&](const decrypted::MTPDdecryptedMessage &) {
			Check(false, "unexpected content message");
		}, [&](const decrypted::MTPDdecryptedMessageService &fields) {
			fields.vaction().match([&](
					const decrypted::MTPDdecryptedMessageActionResend &a) {
				Check(
					a.vstart_seq_no().v == startSeq,
					"resend start_seq_no preserved");
				Check(
					a.vend_seq_no().v == endSeq,
					"resend end_seq_no preserved");
			}, [&](const auto &) {
				Check(false, "unexpected action constructor");
			});
		});
	});
}

// --- At-rest encryption for secret-chat media files ------------------------
//
// Secret-chat media is stored ENCRYPTED on disk (in the per-account
// secret_files/ dir) with the local key and decrypted to memory on demand.
// These mirror the EncryptedChats::writeSecretFileEncrypted and
// readSecretFileEncrypted helpers (which are session-bound and can't be
// called here): the exact Storage::File usage plus truncation to the known
// plaintext size. Storage::File pads the ciphertext to a 16-byte block with
// random bytes and does not store the real size, so the read side must
// truncate to the caller-known size.

[[nodiscard]] Storage::EncryptionKey MakeStorageKey(uchar seed = 0) {
	return Storage::EncryptionKey(MakeKey(seed));
}

// Mirror of writeSecretFileEncrypted: encrypt `plain` to `path` (the buffer is
// encrypted in place by writeWithPadding, so a detached copy is used).
[[nodiscard]] bool WriteEncrypted(
		const QString &path,
		const Storage::EncryptionKey &key,
		const QByteArray &plain) {
	auto file = Storage::File();
	if (file.open(path, Storage::File::Mode::Write, key)
			!= Storage::File::Result::Success) {
		return false;
	}
	auto copy = plain;
	const auto span = bytes::make_detached_span(copy);
	const auto ok = span.empty() || file.writeWithPadding(span);
	file.flush();
	file.close();
	return ok;
}

// Mirror of readSecretFileEncrypted: decrypt `path`, truncate to plaintextSize
// (pass -1 to keep the padded bytes, as the photo path does for JPEGs).
[[nodiscard]] QByteArray ReadEncrypted(
		const QString &path,
		const Storage::EncryptionKey &key,
		int64 plaintextSize) {
	auto file = Storage::File();
	if (file.open(path, Storage::File::Mode::Read, key)
			!= Storage::File::Result::Success) {
		return QByteArray();
	}
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

[[nodiscard]] QByteArray RandomBytes(int size) {
	auto raw = bytes::vector(size);
	if (size > 0) {
		bytes::set_random(raw);
	}
	return QByteArray(
		reinterpret_cast<const char*>(raw.data()),
		int(raw.size()));
}

void TestAtRestRoundTrip() {
	std::cout << "media at-rest encrypt / decrypt round-trip" << std::endl;

	auto dir = QTemporaryDir();
	Check(dir.isValid(), "temp dir created");
	if (!dir.isValid()) {
		return;
	}
	const auto key = MakeStorageKey();

	// Sizes around the 16-byte block boundary, empty, and larger-than-memory-cap
	// (kMaxFileInMemory is 10 MB; 11 MB exercises the large path).
	const int sizes[] = { 0, 1, 15, 16, 17, 100, 4096, 11 * 1024 * 1024 };
	for (const auto size : sizes) {
		const auto plain = RandomBytes(size);
		const auto path = dir.filePath(
			QStringLiteral("file_%1.enc").arg(size));
		Check(WriteEncrypted(path, key, plain), "write encrypted ok");

		const auto restored = ReadEncrypted(path, key, size);
		Check(
			restored == plain,
			"round-trip equals original (size "
				+ std::to_string(size) + ")");

		// The on-disk bytes must not be the plaintext. Only meaningful once
		// the plaintext is a full block: a 1-byte random value shows up by
		// chance somewhere in the ciphertext about half the time.
		if (size >= 16) {
			auto raw = QFile(path);
			Check(raw.open(QIODevice::ReadOnly), "open raw ciphertext");
			const auto onDisk = raw.readAll();
			Check(!onDisk.contains(plain), "plaintext absent from disk bytes");
		}
	}
}

void TestAtRestPaddingTruncation() {
	std::cout << "at-rest padding tail is truncated to the known size"
		<< std::endl;

	auto dir = QTemporaryDir();
	if (!dir.isValid()) {
		Check(false, "temp dir created");
		return;
	}
	const auto key = MakeStorageKey();

	// 17 bytes pads up to 32 on disk; reading without a size (-1, the photo
	// path) returns the padded bytes, and truncating to 17 recovers the original.
	const auto plain = RandomBytes(17);
	const auto path = dir.filePath(QStringLiteral("pad.enc"));
	Check(WriteEncrypted(path, key, plain), "write encrypted ok");

	const auto padded = ReadEncrypted(path, key, -1);
	Check(padded.size() == 32, "padded read rounds up to a 16-byte block");
	Check(
		padded.left(plain.size()) == plain,
		"padded read still starts with the plaintext");

	const auto exact = ReadEncrypted(path, key, plain.size());
	Check(exact == plain, "size-truncated read recovers the exact plaintext");
}

void TestAtRestWrongKey() {
	std::cout << "at-rest read with the wrong key is rejected" << std::endl;

	auto dir = QTemporaryDir();
	if (!dir.isValid()) {
		Check(false, "temp dir created");
		return;
	}
	const auto plain = RandomBytes(128);
	const auto path = dir.filePath(QStringLiteral("wrongkey.enc"));
	Check(WriteEncrypted(path, MakeStorageKey(0), plain), "write encrypted ok");

	// A different local key must fail the header checksum (WrongKey) so the
	// helper returns empty rather than garbage.
	const auto restored = ReadEncrypted(path, MakeStorageKey(1), plain.size());
	Check(restored.isEmpty(), "decrypt with the wrong key yields nothing");

	// Sanity: the right key still recovers it.
	const auto good = ReadEncrypted(path, MakeStorageKey(0), plain.size());
	Check(good == plain, "decrypt with the right key recovers the plaintext");
}

} // namespace

// The layer header is parsed on its own so a body we cannot decode (a newer
// layer's media) still advances the seq bookkeeping instead of turning every
// later message into a gap.
void TestHeaderFirstParse() {
	std::cout << "layer header parsed before the body" << std::endl;

	const auto layer = MakeLayer(QString::fromUtf8("hi"), 42, 3, 4);
	auto serialized = MTP::SecretChat::SerializeObject(layer);
	const auto header = MTP::SecretChat::ParseLayerHeader(serialized);
	Check(header.has_value(), "header parses from a good layer");
	if (header) {
		Check(header->layer == 73, "layer preserved");
		Check(header->inSeqNo == 3, "in_seq_no preserved");
		Check(header->outSeqNo == 4, "out_seq_no preserved");
		Check(header->randomBytes.size() == 16, "random_bytes preserved");
	}

	// Corrupt the body constructor id (right after the header: random_bytes
	// (1 length byte + 16 + 3 padding = 20 bytes = 5 words), layer, in, out).
	auto corrupt = serialized;
	const auto bodyOffset = (1 + 5 + 3) * sizeof(mtpPrime);
	Check(corrupt.size() > bodyOffset + 4, "layer has a body");
	for (auto i = 0; i != 4; ++i) {
		corrupt[bodyOffset + i] = gsl::byte(0xEE);
	}
	auto parsed = decrypted::MTPDecryptedMessageLayer();
	Check(
		!MTP::SecretChat::DeserializeObject(parsed, corrupt),
		"full parse fails on an unknown body");
	const auto still = MTP::SecretChat::ParseLayerHeader(corrupt);
	Check(still.has_value(), "header still parses");
	if (still) {
		Check(still->outSeqNo == 4, "out_seq_no right despite the bad body");
	}

	// Not a layer at all.
	Check(
		!MTP::SecretChat::ParseLayerHeader(bytes::vector(8)).has_value(),
		"garbage is rejected");
}

void TestSeqValidation() {
	std::cout << "layer header validation" << std::endl;

	using MTP::SecretChat::LayerHeader;
	using MTP::SecretChat::ValidateLayerHeader;
	const auto make = [](int random, int in, int out) {
		return LayerHeader{
			.randomBytes = bytes::vector(random),
			.layer = 73,
			.inSeqNo = in,
			.outSeqNo = out,
		};
	};
	// We are the creator (odd out parity): the peer sends even out_seq_no
	// and echoes our odd ones in in_seq_no.
	Check(ValidateLayerHeader(make(16, 1, 0), true), "creator: good");
	Check(ValidateLayerHeader(make(15, -1, 2), true), "creator: 15 bytes ok");
	Check(ValidateLayerHeader(make(16, -2, 0), true), "negative in ok");
	Check(!ValidateLayerHeader(make(8, 1, 0), true), "short random_bytes");
	Check(!ValidateLayerHeader(make(16, 1, -2), true), "negative out");
	Check(!ValidateLayerHeader(make(16, 1, 1), true), "wrong out parity");
	Check(!ValidateLayerHeader(make(16, 2, 0), true), "wrong in parity");
	// Participant: the peer (creator) sends odd.
	Check(ValidateLayerHeader(make(16, 0, 1), false), "participant: good");
	Check(!ValidateLayerHeader(make(16, 0, 2), false), "participant: even");
}

// The api.tl sticker attribute (with an inputStickerSetID) that the Android
// client copies verbatim into an external document must round-trip.
void TestAttributeStickerFull() {
	std::cout << "documentAttributeStickerFull round-trip" << std::endl;

	using Flag = decrypted::MTPDdocumentAttributeStickerFull::Flag;
	auto attributes = QVector<decrypted::MTPDocumentAttribute>();
	attributes.push_back(decrypted::MTP_documentAttributeStickerFull(
		MTP_flags(Flag()),
		MTP_string(QString::fromUtf8("\xF0\x9F\x98\x80")),
		decrypted::MTP_inputStickerSetID(MTP_long(12345), MTP_long(67890)),
		decrypted::MTPMaskCoords()));
	attributes.push_back(decrypted::MTP_documentAttributeHasStickers());
	const auto media = decrypted::MTP_decryptedMessageMediaExternalDocument(
		MTP_long(1),
		MTP_long(2),
		MTP_int(3),
		MTP_string(QString::fromUtf8("application/x-tgsticker")),
		MTP_int(4),
		decrypted::MTP_photoSizeEmpty(MTP_string(QString::fromUtf8("s"))),
		MTP_int(5),
		MTP_vector<decrypted::MTPDocumentAttribute>(attributes));
	const auto serialized = MTP::SecretChat::SerializeObject(
		decrypted::MTPDecryptedMessageMedia(media));

	auto parsed = decrypted::MTPDecryptedMessageMedia();
	const auto ok = MTP::SecretChat::DeserializeObject(parsed, serialized);
	Check(ok, "external document with StickerFull deserializes");
	if (!ok) {
		return;
	}
	parsed.match([&](
			const decrypted::MTPDdecryptedMessageMediaExternalDocument &d) {
		Check(d.vattributes().v.size() == 2, "both attributes kept");
		auto found = false;
		for (const auto &attribute : d.vattributes().v) {
			attribute.match([&](
					const decrypted::MTPDdocumentAttributeStickerFull &s) {
				found = true;
				s.vstickerset().match([&](
						const decrypted::MTPDinputStickerSetID &set) {
					Check(set.vid().v == 12345, "set id preserved");
					Check(set.vaccess_hash().v == 67890, "set hash preserved");
				}, [&](const auto &) {
					Check(false, "set is not an inputStickerSetID");
				});
			}, [](const auto &) {
			});
		}
		Check(found, "StickerFull attribute found");
	}, [&](const auto &) {
		Check(false, "unexpected media type");
	});
	// The wire ids are the api.tl ones.
	Check(
		decrypted::mtpc_documentAttributeStickerFull == 0x6319d612,
		"StickerFull id matches api.tl");
	Check(
		decrypted::mtpc_inputStickerSetID == 0x9de7a269,
		"inputStickerSetID id matches api.tl");
	Check(
		decrypted::mtpc_documentAttributeCustomEmoji == 0xfd149899,
		"CustomEmoji id matches api.tl");
}

// After a rekey commit the retired key still decrypts a message the peer
// encrypted before it switched, while the new key decrypts the rest.
void TestPreviousKeyFallback() {
	std::cout << "previous key fallback after rekey" << std::endl;

	const auto oldKey = MakeKey(0);
	const auto newKey = MakeKey(7);
	const auto oldFp = MTP::SecretChat::ComputeKeyFingerprint(oldKey);
	const auto newFp = MTP::SecretChat::ComputeKeyFingerprint(newKey);
	const auto payload = MakePayload(6);
	const auto inFlight = MTP::SecretChat::Encrypt(payload, oldKey, oldFp, false);
	const auto fresh = MTP::SecretChat::Encrypt(payload, newKey, newFp, false);

	// The receiver (creator) tries the current key first, then the retired.
	const auto decryptWithFallback = [&](bytes::const_span data) {
		if (const auto ok = MTP::SecretChat::Decrypt(data, newKey, newFp, true)) {
			return std::make_pair(true, false);
		} else if (MTP::SecretChat::Decrypt(data, oldKey, oldFp, true)) {
			return std::make_pair(true, true);
		}
		return std::make_pair(false, false);
	};
	const auto a = decryptWithFallback(inFlight);
	Check(a.first && a.second, "in-flight message decrypts with the old key");
	const auto b = decryptWithFallback(fresh);
	Check(b.first && !b.second, "new message decrypts with the new key");
	Check(
		!MTP::SecretChat::Decrypt(inFlight, newKey, newFp, true).has_value(),
		"old-key message rejected by the new key");
}

// The verification identicon hashes the handshake key once (36 bytes:
// sha1[0..16) + sha256[0..20)); the vector was computed with Python hashlib.
void TestKeyHash() {
	std::cout << "key hash vector" << std::endl;

	const auto hash = MTP::SecretChat::ComputeKeyHash(MakeKey());
	const auto expected = QByteArray::fromHex(
		"4916d6bdb7f78e6803698cab32d1586e"
		"40aff2e9d2d8922e47afd4648e6967497158785f");
	Check(hash.size() == 36, "key hash is 36 bytes");
	Check(
		bytes::compare(hash, bytes::make_span(expected)) == 0,
		"key hash matches the reference vector");
}

// The Android client forwards its current api.tl entities into the secret
// layer: the collapsible blockquote (with a flags word), the formatted date
// and the mention-name shapes must parse, or the whole message is replaced
// by a noop and its text is lost.
void TestAndroidEntities() {
	std::cout << "current Android entity constructors parse" << std::endl;

	Check(
		uint32(decrypted::mtpc_messageEntityBlockquoteCollapsible) == 0xf1ccaaacU,
		"collapsible blockquote id matches api.tl");
	Check(
		uint32(decrypted::mtpc_messageEntityFormattedDate) == 0x904ac7c7U,
		"formatted date id matches api.tl");
	Check(
		uint32(decrypted::mtpc_inputMessageEntityMentionName) == 0x208e68c9U,
		"input mention name id matches api.tl");

	using QuoteFlag = decrypted::MTPDmessageEntityBlockquoteCollapsible::Flag;
	using QuoteFlags = decrypted::MTPDmessageEntityBlockquoteCollapsible::Flags;
	using DateFlag = decrypted::MTPDmessageEntityFormattedDate::Flag;
	using DateFlags = decrypted::MTPDmessageEntityFormattedDate::Flags;
	auto entities = QVector<decrypted::MTPMessageEntity>();
	entities.push_back(decrypted::MTP_messageEntityBlockquoteCollapsible(
		MTP_flags(QuoteFlags() | QuoteFlag::f_collapsed),
		MTP_int(0),
		MTP_int(5)));
	entities.push_back(decrypted::MTP_messageEntityFormattedDate(
		MTP_flags(DateFlags() | DateFlag::f_short_date),
		MTP_int(6),
		MTP_int(4),
		MTP_int(1700000000)));
	entities.push_back(decrypted::MTP_inputMessageEntityMentionName(
		MTP_int(11),
		MTP_int(3),
		decrypted::MTP_inputUser(MTP_long(42), MTP_long(43))));
	using MessageFlags = decrypted::MTPDdecryptedMessage::Flags;
	const auto message = decrypted::MTP_decryptedMessage(
		MTP_flags(MessageFlags()
			| decrypted::MTPDdecryptedMessage::Flag::f_entities),
		MTP_long(7),
		MTP_int(0),
		MTP_string(QString::fromLatin1("quote date you")),
		decrypted::MTPDecryptedMessageMedia(),
		MTP_vector<decrypted::MTPMessageEntity>(entities),
		MTP_string(),
		MTP_long(0),
		MTP_long(0));
	auto randomBytes = bytes::vector(16);
	bytes::set_random(randomBytes);
	const auto layer = decrypted::MTP_decryptedMessageLayer(
		MTP_bytes(QByteArray(
			reinterpret_cast<const char*>(randomBytes.data()),
			int(randomBytes.size()))),
		MTP_int(151),
		MTP_int(1),
		MTP_int(2),
		message);
	const auto serialized = MTP::SecretChat::SerializeObject(
		decrypted::MTPDecryptedMessageLayer(layer));
	const auto header = MTP::SecretChat::ParseLayerHeader(serialized);
	Check(
		header && MTP::SecretChat::ValidateLayerHeader(*header, true),
		"header of the Android message validates");

	auto parsed = decrypted::MTPDecryptedMessageLayer();
	Check(
		MTP::SecretChat::DeserializeObject(parsed, serialized),
		"body with the current Android entities deserializes");
	auto collapsed = false;
	auto count = 0;
	parsed.match([&](const decrypted::MTPDdecryptedMessageLayer &data) {
		data.vmessage().match([&](
				const decrypted::MTPDdecryptedMessage &fields) {
			if (const auto list = fields.ventities()) {
				count = list->v.size();
				for (const auto &entity : list->v) {
					entity.match([&](
							const decrypted::MTPDmessageEntityBlockquoteCollapsible &d) {
						collapsed = d.is_collapsed();
					}, [](const auto &) {
					});
				}
			}
		}, [&](const decrypted::MTPDdecryptedMessageService &) {
			Check(false, "unexpected service message");
		});
	});
	Check(count == 3, "all three entities survive the round-trip");
	Check(collapsed, "collapsed flag preserved");
}

int main(int argc, char *argv[]) {
	TestFingerprint();
	TestKeyHash();
	TestAndroidEntities();
	TestHeaderFirstParse();
	TestSeqValidation();
	TestAttributeStickerFull();
	TestPreviousKeyFallback();
	TestRoundTrip();
	TestWrongRoleFails();
	TestTamperFails();
	TestTlRoundTrip();
	TestLegacyDocumentMedia();
	TestEndToEndPipeline();
	TestResendActionPipeline();
	TestAtRestRoundTrip();
	TestAtRestPaddingTruncation();
	TestAtRestWrongKey();

	std::cout << "\n" << (gChecks - gFailures) << "/" << gChecks
		<< " checks passed" << std::endl;
	if (gFailures > 0) {
		std::cerr << gFailures << " check(s) FAILED" << std::endl;
		return 1;
	}
	std::cout << "OK" << std::endl;
	return 0;
}
