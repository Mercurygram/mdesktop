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

class RecentSharedMediaGifts final {
public:
	explicit RecentSharedMediaGifts(not_null<Main::Session*> session);
	~RecentSharedMediaGifts();

	void request(
		not_null<PeerData*> peer,
		Fn<void(std::vector<DocumentId>)> done,
		bool onlyPinnedToTop = false);

private:
	struct GiftItem {
		DocumentId id;
		bool pinned = false;
	};

	[[nodiscard]] std::vector<DocumentId> filterGifts(
		const std::deque<GiftItem> &gifts,
		bool onlyPinnedToTop);

	struct Entry {
		std::deque<GiftItem> gifts;
		crl::time lastRequestTime = 0;
		mtpRequestId requestId = 0;
		std::vector<Fn<void()>> pendingCallbacks;
	};

	const not_null<Main::Session*> _session;

	base::flat_map<PeerId, Entry> _recent;

};

} // namespace Data
