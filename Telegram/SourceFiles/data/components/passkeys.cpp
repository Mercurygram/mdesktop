/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "data/components/passkeys.h"

#include "apiwrap.h"
#include "data/data_passkey_deserialize.h"
#include "main/main_session.h"
#include "platform/platform_webauthn.h"

namespace Data {

Passkeys::Passkeys(not_null<Main::Session*> session)
: _session(session) {
}

Passkeys::~Passkeys() = default;

void Passkeys::initRegistration(
		Fn<void(const Data::Passkey::RegisterData&)> done) {
	_session->api().request(MTPaccount_InitPasskeyRegistration(
	)).done([=](const MTPaccount_PasskeyRegistrationOptions &result) {
		const auto &data = result.data();
		const auto jsonData = data.voptions().data().vdata().v;
		if (const auto p = Data::Passkey::DeserializeRegisterData(jsonData)) {
			done(*p);
		}
	}).send();
}

void Passkeys::registerPasskey(
		const Platform::WebAuthn::RegisterResult &result,
		Fn<void()> done) {
	const auto credentialIdBase64 = QString::fromUtf8(
		result.credentialId.toBase64(QByteArray::Base64UrlEncoding));
	_session->api().request(MTPaccount_RegisterPasskey(
		MTP_inputPasskeyCredentialPublicKey(
			MTP_string(credentialIdBase64),
			MTP_string(credentialIdBase64),
			MTP_inputPasskeyResponseRegister(
				MTP_dataJSON(MTP_bytes(result.clientDataJSON)),
				MTP_bytes(result.attestationObject)))
	)).done([=](const MTPPasskey &result) {
		done();
	}).send();
}

} // namespace Data
