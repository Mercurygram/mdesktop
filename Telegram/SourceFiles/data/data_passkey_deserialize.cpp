/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/

#include "data/data_passkey_deserialize.h"

namespace Data::Passkey {

std::optional<RegisterData> DeserializeRegisterData(
		const QByteArray &jsonData) {
	return std::nullopt;
}

std::string SerializeClientDataCreate(const QByteArray &challenge) {
	return std::string();
}

std::string SerializeClientDataGet(const QByteArray &challenge) {
	return std::string();
}

std::optional<LoginData> DeserializeLoginData(
		const QByteArray &jsonData) {
	return std::nullopt;
}

} // namespace Data::Passkey
