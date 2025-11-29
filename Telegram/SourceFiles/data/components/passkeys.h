/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Data::Passkey {
struct RegisterData;
} // namespace Data::Passkey
namespace Platform::WebAuthn {
struct RegisterResult;
} // namespace Platform::WebAuthn

namespace Main {
class Session;
} // namespace Main

namespace Data {

class Passkeys final {
public:
	explicit Passkeys(not_null<Main::Session*> session);
	~Passkeys();

	void initRegistration(Fn<void(const Data::Passkey::RegisterData&)> done);
	void registerPasskey(
		const Platform::WebAuthn::RegisterResult &result,
		Fn<void()> done);

private:
	const not_null<Main::Session*> _session;

};

} // namespace Data
