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

struct PasskeyEntry {
	QString id;
	QString name;
	TimeId date = 0;
	DocumentId softwareEmojiId = 0;
	TimeId lastUsageDate = 0;
};

class Passkeys final {
public:
	explicit Passkeys(not_null<Main::Session*> session);
	~Passkeys();

	void initRegistration(Fn<void(const Data::Passkey::RegisterData&)> done);
	void registerPasskey(
		const Platform::WebAuthn::RegisterResult &result,
		Fn<void()> done);
	[[nodiscard]] rpl::producer<> requestList();
	[[nodiscard]] const std::vector<PasskeyEntry> &list() const;
	[[nodiscard]] bool listKnown() const;

private:
	void loadList();

	const not_null<Main::Session*> _session;
	std::vector<PasskeyEntry> _passkeys;
	rpl::event_stream<> _listUpdated;
	crl::time _lastRequestTime = 0;
	mtpRequestId _listRequestId = 0;
	bool _listKnown = false;

};

} // namespace Data
