/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "data/components/passkeys.h"

#include "main/main_session.h"

namespace Data {

Passkeys::Passkeys(not_null<Main::Session*> session)
: _session(session) {
}

Passkeys::~Passkeys() = default;

} // namespace Data
