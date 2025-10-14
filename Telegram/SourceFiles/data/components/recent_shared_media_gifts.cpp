/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "data/components/recent_shared_media_gifts.h"

#include "api/api_premium.h"
#include "apiwrap.h"
#include "data/data_document.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "main/main_session.h"

namespace Data {
namespace {

constexpr auto kReloadThreshold = 60 * crl::time(1000);
constexpr auto kMaxGifts = 3;
constexpr auto kMaxPinnedGifts = 6;

} // namespace

RecentSharedMediaGifts::RecentSharedMediaGifts(
	not_null<Main::Session*> session)
: _session(session) {
}

RecentSharedMediaGifts::~RecentSharedMediaGifts() = default;

std::vector<Data::SavedStarGift> RecentSharedMediaGifts::filterGifts(
		const std::deque<Data::SavedStarGift> &gifts,
		bool onlyPinnedToTop) {
	auto result = std::vector<Data::SavedStarGift>();
	const auto maxCount = onlyPinnedToTop ? kMaxPinnedGifts : kMaxGifts;
	for (const auto &gift : gifts) {
		if (!onlyPinnedToTop || gift.pinned) {
			result.push_back(gift);
			if (result.size() >= maxCount) {
				break;
			}
		}
	}
	return result;
}

void RecentSharedMediaGifts::request(
		not_null<PeerData*> peer,
		Fn<void(std::vector<SavedStarGift>)> done,
		bool onlyPinnedToTop) {
	const auto it = _recent.find(peer->id);
	if (it != _recent.end()) {
		auto &entry = it->second;
		if (entry.lastRequestTime
			&& entry.lastRequestTime + kReloadThreshold > crl::now()) {
			done(filterGifts(entry.gifts, onlyPinnedToTop));
			return;
		}
		if (entry.requestId) {
			entry.pendingCallbacks.push_back([=] {
				const auto it = _recent.find(peer->id);
				if (it != _recent.end()) {
					done(filterGifts(it->second.gifts, onlyPinnedToTop));
				}
			});
			return;
		}
	}

	_recent[peer->id].requestId = peer->session().api().request(
		MTPpayments_GetSavedStarGifts(
			MTP_flags(0),
			peer->input,
			MTP_int(0), // collection_id
			MTP_string(QString()),
			MTP_int(kMaxPinnedGifts)
	)).done([=](const MTPpayments_SavedStarGifts &result) {
		const auto &data = result.data();
		const auto owner = &peer->owner();
		owner->processUsers(data.vusers());
		owner->processChats(data.vchats());
		auto &entry = _recent[peer->id];
		entry.lastRequestTime = crl::now();
		entry.requestId = 0;
		entry.gifts.clear();

		for (const auto &gift : data.vgifts().v) {
			if (auto parsed = Api::FromTL(peer, gift)) {
				entry.gifts.push_back(std::move(*parsed));
			}
		}

		done(filterGifts(entry.gifts, onlyPinnedToTop));
		for (const auto &callback : entry.pendingCallbacks) {
			callback();
		}
		entry.pendingCallbacks.clear();
	}).send();
}

void RecentSharedMediaGifts::clearLastRequestTime(
		not_null<PeerData*> peer) {
	const auto it = _recent.find(peer->id);
	if (it != _recent.end()) {
		it->second.lastRequestTime = 0;
	}
}

} // namespace Data
