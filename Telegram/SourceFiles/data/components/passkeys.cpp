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
namespace {

constexpr auto kTimeoutMs = 5000;

[[nodiscard]] PasskeyEntry FromTL(const MTPDpasskey &data) {
	return PasskeyEntry{
		.id = qs(data.vid()),
		.name = qs(data.vname()),
		.date = data.vdate().v,
		.softwareEmojiId = data.vsoftware_emoji_id().value_or(0),
		.lastUsageDate = data.vlast_usage_date().value_or(0),
	};
}

} // namespace

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
		_passkeys.emplace_back(FromTL(result.data()));
		_listUpdated.fire({});
		done();
	}).send();
}

void Passkeys::deletePasskey(
		const QString &id,
		Fn<void()> done,
		Fn<void(QString)> fail) {
	_session->api().request(MTPaccount_DeletePasskey(
		MTP_string(id)
	)).done([=] {
		_lastRequestTime = 0;
		_listKnown = false;
		loadList();
		done();
	}).fail([=](const MTP::Error &error) {
		fail(error.type());
	}).send();
}

rpl::producer<> Passkeys::requestList() {
	if (crl::now() - _lastRequestTime > kTimeoutMs) {
		if (!_listRequestId) {
			loadList();
		}
		return _listUpdated.events();
	} else {
		return _listUpdated.events_starting_with(rpl::empty_value());
	}
}

const std::vector<PasskeyEntry> &Passkeys::list() const {
	return _passkeys;
}

bool Passkeys::listKnown() const {
	return _listKnown;
}

void Passkeys::loadList() {
	_lastRequestTime = crl::now();
	_listRequestId = _session->api().request(MTPaccount_GetPasskeys(
	)).done([=](const MTPaccount_Passkeys &result) {
		_listRequestId = 0;
		_listKnown = true;
		const auto &data = result.data();
		_passkeys.clear();
		_passkeys.reserve(data.vpasskeys().v.size());
		for (const auto &passkey : data.vpasskeys().v) {
			_passkeys.emplace_back(FromTL(passkey.data()));
		}
		_listUpdated.fire({});
	}).fail([=] {
		_listRequestId = 0;
	}).send();
}

} // namespace Data
