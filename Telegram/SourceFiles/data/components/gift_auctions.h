/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "data/data_star_gift.h"

namespace Main {
class Session;
} // namespace Main

namespace Data {

struct GiftAuctionBidLevel {
	int64 amount = 0;
	int position = 0;
	TimeId date = 0;
};

struct StarGiftAuctionMyState {
	PeerData *to = nullptr;
	int64 minBidAmount = 0;
	int64 bid = 0;
	TimeId date = 0;
	int gotCount = 0;
	bool returned = false;
};

struct GiftAuctionState {
	StarGift gift;
	StarGiftAuctionMyState my;
	std::vector<GiftAuctionBidLevel> bidLevels;
	std::vector<not_null<UserData*>> topBidders;
	crl::time subscribedTill = 0;
	int64 minBidAmount = 0;
	int64 averagePrice = 0;
	TimeId startDate = 0;
	TimeId endDate = 0;
	TimeId nextRoundAt = 0;
	int currentRound = 0;
	int totalRounds = 0;
	int giftsLeft = 0;
	int version = 0;
};

class GiftAuctions final {
public:
	explicit GiftAuctions(not_null<Main::Session*> session);
	~GiftAuctions();

	//[[nodiscard]] rpl::producer<

private:
	const not_null<Main::Session*> _session;

	base::flat_map<uint64, std::unique_ptr<GiftAuctionState>> _map;

};

} // namespace Data
