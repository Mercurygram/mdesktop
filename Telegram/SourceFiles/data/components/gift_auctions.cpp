/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "data/components/gift_auctions.h"

#include "data/data_session.h"
#include "main/main_session.h"

namespace Data {

GiftAuctions::GiftAuctions(not_null<Main::Session*> session)
: _session(session) {
}

GiftAuctions::~GiftAuctions() = default;

} // namespace Data
