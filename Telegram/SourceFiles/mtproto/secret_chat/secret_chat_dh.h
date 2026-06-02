/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/bytes.h"
#include "mtproto/mtproto_auth_key.h"

#include <optional>

namespace MTP::SecretChat {

// A computed secret-chat key together with its fingerprint.
struct ComputedKey {
	AuthKey::Data key = { { gsl::byte{} } };
	uint64 fingerprint = 0;
};

// Validates the secret chat Diffie-Hellman configuration (prime + generator).
[[nodiscard]] bool ValidateDhConfig(int g, bytes::const_span primeBytes);

// Computes the shared secret-chat key from the other party's g_a / g_b and our
// own random power, left-padded to 256 bytes, together with its fingerprint.
// Returns std::nullopt if the received value fails the safety checks (this is
// the mandatory 1 < g < p-1, 2^{2048-64} <= g <= p-2^{2048-64} validation,
// performed inside MTP::CreateAuthKey).
[[nodiscard]] std::optional<ComputedKey> ComputeKey(
	bytes::const_span gOther,
	bytes::const_span randomPower,
	bytes::const_span primeBytes);

} // namespace MTP::SecretChat
