/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Main {
class Session;
} // namespace Main

namespace Data {

class Passkeys final {
public:
	explicit Passkeys(not_null<Main::Session*> session);
	~Passkeys();

private:
	const not_null<Main::Session*> _session;

};

} // namespace Data
