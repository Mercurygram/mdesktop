/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "mtproto/secret_chat/secret_chat_dh.h"

#include "mtproto/mtproto_dh_utils.h"
#include "mtproto/secret_chat/secret_chat_encryption.h"

namespace MTP::SecretChat {

bool ValidateDhConfig(int g, bytes::const_span primeBytes) {
	return IsPrimeAndGood(primeBytes, g);
}

std::optional<ComputedKey> ComputeKey(
		bytes::const_span gOther,
		bytes::const_span randomPower,
		bytes::const_span primeBytes) {
	const auto computed = CreateAuthKey(gOther, randomPower, primeBytes);
	if (computed.empty()) {
		return std::nullopt;
	}
	auto result = ComputedKey();
	AuthKey::FillData(result.key, computed);
	result.fingerprint = ComputeKeyFingerprint(result.key);
	return result;
}

} // namespace MTP::SecretChat
