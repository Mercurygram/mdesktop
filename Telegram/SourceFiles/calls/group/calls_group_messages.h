/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/timer.h"

namespace Calls {
class GroupCall;
} // namespace Calls

namespace Data {
class GroupCall;
} // namespace Data

namespace MTP {
class Sender;
struct Response;
} // namespace MTP

namespace Calls::Group {

struct Message {
	MsgId id = 0;
	TimeId date = 0;
	not_null<PeerData*> peer;
	TextWithEntities text;
	int stars = 0;
	bool failed = false;
};

struct MessageIdUpdate {
	MsgId localId = 0;
	MsgId realId = 0;
};

class Messages final {
public:
	Messages(not_null<GroupCall*> call, not_null<MTP::Sender*> api);

	void send(TextWithTags text, int stars);

	void received(const MTPDupdateGroupCallMessage &data);
	void received(const MTPDupdateGroupCallEncryptedMessage &data);
	void deleted(const MTPDupdateDeleteGroupCallMessages &data);
	void sent(const MTPDupdateMessageID &data);

	[[nodiscard]] rpl::producer<std::vector<Message>> listValue() const;
	[[nodiscard]] rpl::producer<MessageIdUpdate> idUpdates() const;

private:
	struct Pending {
		TextWithTags text;
		int stars = 0;
	};
	[[nodiscard]] bool ready() const;
	void sendPending();
	void pushChanges();
	void checkDestroying(bool afterChanges = false);

	void received(
		MsgId id,
		const MTPPeer &from,
		const MTPTextWithEntities &message,
		TimeId date,
		int stars,
		bool checkCustomEmoji = false);
	void sent(uint64 randomId, const MTP::Response &response);
	void sent(uint64 randomId, MsgId realId);
	void failed(uint64 randomId, const MTP::Response &response);

	const not_null<GroupCall*> _call;
	const not_null<MTP::Sender*> _api;

	MsgId _conferenceIdAutoIncrement = 0;
	base::flat_map<uint64, MsgId> _conferenceIdByRandomId;

	base::flat_map<uint64, MsgId> _sendingIdByRandomId;

	Data::GroupCall *_real = nullptr;

	std::vector<Pending> _pending;

	base::Timer _destroyTimer;
	std::vector<Message> _messages;
	rpl::event_stream<std::vector<Message>> _changes;
	rpl::event_stream<MessageIdUpdate> _idUpdates;

	TimeId _ttl = 0;

	rpl::lifetime _lifetime;

};

} // namespace Calls::Group
