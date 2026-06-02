/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/bytes.h"
#include "mtproto/core_types.h"

#include <tl/tl_boxed.h>

#include <optional>

namespace MTP::SecretChat {

// The shared Diffie-Hellman secret for a secret chat is always 2048 bit.
inline constexpr auto kKeySize = 256;

// MTProto 2.0 secret chat encryption.
//
// The plaintext payload is: int32 length(of the serialized object) + the
// serialized decryptedMessageLayer + 12..1024 random padding bytes, to a
// multiple of 16, then encrypted with AES-256-IGE. The wire `data:bytes` is:
// key_fingerprint(8, LE) + msg_key(16) + ciphertext.
//
// `x` in the key derivation is 0 for messages sent by the secret chat creator
// (admin) and 8 for messages sent by the other participant -- this is the only
// difference from the data-center key derivation in mtproto_auth_key.cpp.

// Lower 64 bits of SHA1(authKey) (bytes 12..19, little-endian) -- this is the
// key_fingerprint carried in encryptedChat and prefixed to every message.
[[nodiscard]] uint64 ComputeKeyFingerprint(bytes::const_span authKey);

// The key visualization shown to both users for verification:
// sha1(authKey)[0..16) + sha256(authKey)[0..20) = 36 bytes, the key_hash the
// mobile clients compute from the handshake key and keep unchanged across
// PFS rekeys (TDLib SecretChatActor::calc_key_hash, Android calcAuthKeyHash).
[[nodiscard]] bytes::vector ComputeKeyHash(bytes::const_span authKey);

// Serialize a boxed decrypted-layer TL object to a 4-byte aligned byte buffer.
template <typename Boxed>
[[nodiscard]] bytes::vector SerializeObject(const Boxed &object) {
	auto counter = ::tl::details::LengthCounter();
	object.write(counter);
	auto buffer = mtpBuffer();
	buffer.reserve(counter.length);
	object.write(buffer);
	const auto size = buffer.size() * sizeof(mtpPrime);
	auto result = bytes::vector(size);
	if (size > 0) {
		memcpy(result.data(), buffer.constData(), size);
	}
	return result;
}

// Deserialize a boxed decrypted-layer TL object from raw bytes. The TL stream
// is self-delimiting, so any trailing padding is ignored. Returns false on a
// malformed stream.
template <typename Boxed>
[[nodiscard]] bool DeserializeObject(
		Boxed &object,
		bytes::const_span serialized) {
	const auto words = serialized.size() / sizeof(mtpPrime);
	if (words == 0) {
		return false;
	}
	auto buffer = mtpBuffer(words);
	memcpy(buffer.data(), serialized.data(), words * sizeof(mtpPrime));
	auto from = buffer.constData();
	const auto end = from + words;
	return object.read(from, end);
}

// The fixed prefix of a serialized decryptedMessageLayer, parsed on its own
// before the message body: the seq numbers must be accounted for even when
// the body carries a constructor we cannot deserialize (a newer layer's
// media), otherwise every later message looks like a gap and the chat is
// discarded. Mirrors iOS, which records the seq before decoding contents.
struct LayerHeader {
	bytes::vector randomBytes;
	int32 layer = 0;
	int32 inSeqNo = 0;
	int32 outSeqNo = 0;
};
[[nodiscard]] std::optional<LayerHeader> ParseLayerHeader(
	bytes::const_span serialized);

// Wire sanity checks the mobile clients apply to an incoming layer:
// random_bytes >= 15 bytes, a non-negative out_seq_no of the peer's parity
// (the creator sends odd), and an in_seq_no (the peer's view of OUR out
// seq) of our parity, or negative before our first message.
[[nodiscard]] bool ValidateLayerHeader(
	const LayerHeader &header,
	bool amCreator);

// Encrypt an already-serialized payload for sending. `amCreator` is whether we
// are the creator (admin) of this secret chat. Returns the full `data:bytes`.
[[nodiscard]] bytes::vector Encrypt(
	bytes::const_span serialized,
	bytes::const_span authKey,
	uint64 keyFingerprint,
	bool amCreator);

// Decrypt a received `data:bytes`. `amCreator` is whether we are the creator
// (admin); the sender is therefore the other participant. Verifies the key
// fingerprint and msg_key, then strips the length prefix and trailing padding.
// Returns the serialized object (ready for DeserializeObject) or std::nullopt
// if anything fails to validate.
[[nodiscard]] std::optional<bytes::vector> Decrypt(
	bytes::const_span encrypted,
	bytes::const_span authKey,
	uint64 keyFingerprint,
	bool amCreator);

// Secret-chat media files are encrypted independently of the message: a fresh
// random 32-byte key + 32-byte iv per file, AES-256-IGE over the content padded
// with random bytes to a multiple of 16. The real (unpadded) size is carried in
// decryptedMessageMediaDocument/Photo.size; key/iv travel in the same media.

struct EncryptedFile {
	bytes::vector bytes; // padded ciphertext, ready to upload
	bytes::array<32> key = { { gsl::byte{} } };
	bytes::array<32> iv = { { gsl::byte{} } };
	int32 keyFingerprint = 0;
};

// key_fingerprint per the encrypted-file spec: digest = md5(key + iv), folded
// as int32(digest[0..4]) XOR int32(digest[4..8]) (little-endian).
[[nodiscard]] int32 FileKeyFingerprint(
	bytes::const_span key,
	bytes::const_span iv);

// Encrypt a file's plaintext with a fresh random key/iv (see EncryptedFile).
[[nodiscard]] EncryptedFile EncryptFileContent(bytes::const_span plain);

// Decrypt downloaded encrypted-file bytes with the per-file key/iv. Returns the
// padded plaintext (caller truncates to the known size) or std::nullopt if the
// input is not a positive multiple of 16.
[[nodiscard]] std::optional<bytes::vector> DecryptFileContent(
	bytes::const_span encrypted,
	bytes::const_span key,
	bytes::const_span iv);

} // namespace MTP::SecretChat
