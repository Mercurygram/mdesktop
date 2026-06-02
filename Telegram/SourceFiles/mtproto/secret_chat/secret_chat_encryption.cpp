/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "mtproto/secret_chat/secret_chat_encryption.h"

#include "mtproto/mtproto_auth_key.h"
#include "base/openssl_help.h"
#include "base/random.h"

#include <openssl/evp.h>

namespace MTP::SecretChat {
namespace {

// One-shot MD5 (used only for the per-file key fingerprint). Computed via EVP
// to avoid the deprecated low-level MD5() API on OpenSSL 3.
[[nodiscard]] bytes::array<16> Md5(bytes::const_span data) {
	auto result = bytes::array<16>();
	auto length = unsigned(0);
	EVP_Digest(
		data.data(),
		data.size(),
		reinterpret_cast<unsigned char*>(result.data()),
		&length,
		EVP_md5(),
		nullptr);
	return result;
}

constexpr auto kFingerprintSize = 8;
constexpr auto kMsgKeySize = 16;
constexpr auto kPrefixSize = kFingerprintSize + kMsgKeySize; // 24
constexpr auto kMinPadding = 12;
constexpr auto kMaxPadding = 1024;
constexpr auto kAlignment = 16;
constexpr auto kMsgKeyPart = 32; // bytes of the key hashed into msg_key
constexpr auto kKdfPart = 36; // bytes of the key hashed into aes_key / aes_iv

// AES-256-IGE key/iv derivation, identical to MTProto 2.0 (see
// AuthKey::prepareAES). `x` is 0 for messages from the chat creator, 8 from
// the other participant.
void DeriveKeyIv(
		bytes::const_span authKey,
		bytes::const_span msgKey,
		int x,
		bytes::span aesKey,
		bytes::span aesIV) {
	Expects(authKey.size() == kKeySize);
	Expects(msgKey.size() == kMsgKeySize);
	Expects(aesKey.size() == 32);
	Expects(aesIV.size() == 32);

	bytes::array<32> sha256_a;
	bytes::array<kMsgKeySize + kKdfPart> data_a;
	bytes::copy(data_a, msgKey);
	bytes::copy(
		bytes::make_span(data_a).subspan(kMsgKeySize),
		authKey.subspan(x, kKdfPart));
	openssl::Sha256To(sha256_a, data_a);

	bytes::array<32> sha256_b;
	bytes::array<kKdfPart + kMsgKeySize> data_b;
	bytes::copy(data_b, authKey.subspan(40 + x, kKdfPart));
	bytes::copy(
		bytes::make_span(data_b).subspan(kKdfPart),
		msgKey);
	openssl::Sha256To(sha256_b, data_b);

	// aes_key = a[0..8] + b[8..24] + a[24..32]
	bytes::copy(aesKey, bytes::make_span(sha256_a).subspan(0, 8));
	bytes::copy(aesKey.subspan(8), bytes::make_span(sha256_b).subspan(8, 16));
	bytes::copy(aesKey.subspan(24), bytes::make_span(sha256_a).subspan(24, 8));

	// aes_iv = b[0..8] + a[8..24] + b[24..32]
	bytes::copy(aesIV, bytes::make_span(sha256_b).subspan(0, 8));
	bytes::copy(aesIV.subspan(8), bytes::make_span(sha256_a).subspan(8, 16));
	bytes::copy(aesIV.subspan(24), bytes::make_span(sha256_b).subspan(24, 8));
}

// msg_key = substr(SHA256(substr(authKey, 88 + x, 32) + payload), 8, 16).
[[nodiscard]] bytes::array<kMsgKeySize> ComputeMsgKey(
		bytes::const_span authKey,
		bytes::const_span payload,
		int x) {
	Expects(authKey.size() == kKeySize);

	const auto full = openssl::Sha256(
		authKey.subspan(88 + x, kMsgKeyPart),
		payload);
	auto result = bytes::array<kMsgKeySize>();
	bytes::copy(result, bytes::make_span(full).subspan(8, kMsgKeySize));
	return result;
}

} // namespace

uint64 ComputeKeyFingerprint(bytes::const_span authKey) {
	Expects(authKey.size() == kKeySize);

	const auto hash = openssl::Sha1(authKey);
	auto result = uint64();
	bytes::copy(
		bytes::object_as_span(&result),
		bytes::make_span(hash).subspan(12, 8));
	return result;
}

bytes::vector Encrypt(
		bytes::const_span serialized,
		bytes::const_span authKey,
		uint64 keyFingerprint,
		bool amCreator) {
	Expects(authKey.size() == kKeySize);
	Expects(serialized.size() % sizeof(mtpPrime) == 0);

	// The secret-chat payload is prefixed with the 4-byte length of the
	// serialized object, then padded with 12..1024 random bytes so the total
	// (length + object + padding) is a multiple of 16.
	const auto headerSize = int(sizeof(int32));
	const auto unpadded = headerSize + serialized.size();
	auto paddingSize = kMinPadding;
	const auto remainder = (unpadded + paddingSize) % kAlignment;
	if (remainder != 0) {
		paddingSize += (kAlignment - remainder);
	}
	// Add a random whole number of 16-byte blocks so the ciphertext length no
	// longer reveals the exact payload length, matching the main-protocol path
	// (mtproto_serialized_request.cpp) and the reference clients which append
	// (2 + rand(0..2)) * 16 extra bytes. The total stays a multiple of 16 and
	// well within kMaxPadding, so Decrypt()'s padding bounds still hold.
	paddingSize += (2 + (base::RandomValue<uchar>() % 3)) * kAlignment;

	auto payload = bytes::vector(unpadded + paddingSize);
	const auto length = int32(serialized.size());
	bytes::copy(payload, bytes::object_as_span(&length));
	bytes::copy(bytes::make_span(payload).subspan(headerSize), serialized);
	bytes::set_random(bytes::make_span(payload).subspan(unpadded));

	// We are the sender: x = 0 if we created the chat, 8 otherwise.
	const auto x = amCreator ? 0 : 8;
	const auto msgKey = ComputeMsgKey(authKey, payload, x);

	bytes::array<32> aesKey, aesIV;
	DeriveKeyIv(authKey, msgKey, x, aesKey, aesIV);

	auto result = bytes::vector(kPrefixSize + payload.size());
	bytes::copy(result, bytes::object_as_span(&keyFingerprint));
	bytes::copy(bytes::make_span(result).subspan(kFingerprintSize), msgKey);
	aesIgeEncryptRaw(
		payload.data(),
		result.data() + kPrefixSize,
		payload.size(),
		aesKey.data(),
		aesIV.data());
	return result;
}

// Constant-time comparison of two equal-length byte spans, mirroring
// MTP::details::ConstTimeIsDifferent in session_private.cpp: the msg_key check
// is an authentication-tag comparison and must not short-circuit on the first
// differing byte the way bytes::compare()/memcmp would.
[[nodiscard]] bool ConstTimeIsDifferent(
		bytes::const_span a,
		bytes::const_span b) {
	Expects(a.size() == b.size());

	auto ca = reinterpret_cast<const char*>(a.data());
	auto cb = reinterpret_cast<const char*>(b.data());
	volatile auto different = false;
	for (const auto ce = ca + a.size(); ca != ce; ++ca, ++cb) {
		different = different | (*ca != *cb);
	}
	return different;
}

std::optional<bytes::vector> Decrypt(
		bytes::const_span encrypted,
		bytes::const_span authKey,
		uint64 keyFingerprint,
		bool amCreator) {
	Expects(authKey.size() == kKeySize);

	if (encrypted.size() < kPrefixSize + kAlignment) {
		return std::nullopt;
	}
	const auto cipherSize = encrypted.size() - kPrefixSize;
	if (cipherSize % kAlignment != 0) {
		return std::nullopt;
	}

	auto fingerprint = uint64();
	bytes::copy(
		bytes::object_as_span(&fingerprint),
		encrypted.subspan(0, kFingerprintSize));
	if (fingerprint != keyFingerprint) {
		return std::nullopt;
	}

	const auto msgKey = encrypted.subspan(kFingerprintSize, kMsgKeySize);

	// The sender is the other participant: x = 8 if we created the chat.
	const auto x = amCreator ? 8 : 0;
	bytes::array<32> aesKey, aesIV;
	DeriveKeyIv(authKey, msgKey, x, aesKey, aesIV);

	auto payload = bytes::vector(cipherSize);
	aesIgeDecryptRaw(
		encrypted.data() + kPrefixSize,
		payload.data(),
		cipherSize,
		aesKey.data(),
		aesIV.data());

	// Verify msg_key over the decrypted payload.
	const auto computed = ComputeMsgKey(authKey, payload, x);
	if (ConstTimeIsDifferent(computed, msgKey)) {
		return std::nullopt;
	}

	// Strip the 4-byte length prefix; the next `length` bytes are the
	// serialized object, the remainder is random padding.
	const auto headerSize = int(sizeof(int32));
	if (int(payload.size()) < headerSize) {
		return std::nullopt;
	}
	auto length = int32();
	bytes::copy(
		bytes::object_as_span(&length),
		bytes::make_span(payload).subspan(0, headerSize));
	// NB: test `length > size - headerSize`, not `headerSize + length > size`:
	// `length` is attacker-influenced (a malicious authenticated peer can set it
	// near INT_MAX), so the addition would be signed-overflow UB. payload.size()
	// is a multiple of 16 >= kAlignment, so `size - headerSize` never underflows.
	if (length < 0
		|| (length % int(sizeof(mtpPrime)) != 0)
		|| (length > int(payload.size()) - headerSize)) {
		return std::nullopt;
	}
	// MTProto 2.0 mandates 12..1024 bytes of random padding; reject frames that
	// fall outside that range, matching the main-protocol path in
	// session_private.cpp instead of accepting any (even zero) padding.
	const auto padding = int(payload.size()) - headerSize - length;
	if (padding < kMinPadding || padding > kMaxPadding) {
		return std::nullopt;
	}
	return bytes::vector(
		payload.begin() + headerSize,
		payload.begin() + headerSize + length);
}

int32 FileKeyFingerprint(bytes::const_span key, bytes::const_span iv) {
	Expects(key.size() == 32);
	Expects(iv.size() == 32);

	auto data = bytes::array<64>();
	bytes::copy(data, key);
	bytes::copy(bytes::make_span(data).subspan(32), iv);
	const auto digest = Md5(data);

	auto a = int32(), b = int32();
	bytes::copy(bytes::object_as_span(&a), bytes::make_span(digest).subspan(0, 4));
	bytes::copy(bytes::object_as_span(&b), bytes::make_span(digest).subspan(4, 4));
	return a ^ b;
}

EncryptedFile EncryptFileContent(bytes::const_span plain) {
	auto result = EncryptedFile();
	bytes::set_random(result.key);
	bytes::set_random(result.iv);

	auto padded = plain.size();
	if (const auto remainder = padded % kAlignment) {
		padded += (kAlignment - remainder);
	}
	auto input = bytes::vector(padded);
	bytes::copy(input, plain);
	if (padded > plain.size()) {
		bytes::set_random(bytes::make_span(input).subspan(plain.size()));
	}

	result.bytes = bytes::vector(padded);
	aesIgeEncryptRaw(
		input.data(),
		result.bytes.data(),
		padded,
		result.key.data(),
		result.iv.data());
	result.keyFingerprint = FileKeyFingerprint(result.key, result.iv);
	return result;
}

std::optional<bytes::vector> DecryptFileContent(
		bytes::const_span encrypted,
		bytes::const_span key,
		bytes::const_span iv) {
	Expects(key.size() == 32);
	Expects(iv.size() == 32);

	if (encrypted.empty() || (encrypted.size() % kAlignment != 0)) {
		return std::nullopt;
	}
	auto result = bytes::vector(encrypted.size());
	aesIgeDecryptRaw(
		encrypted.data(),
		result.data(),
		encrypted.size(),
		key.data(),
		iv.data());
	return result;
}

} // namespace MTP::SecretChat
