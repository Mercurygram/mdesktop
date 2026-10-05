/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "api/api_messages_search.h"

#include "apiwrap.h"
#include "core/mg_search_query.h"
#include "data/data_channel.h"
#include "data/data_histories.h"
#include "data/data_message_reaction_id.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"

namespace Api {
namespace {

constexpr auto kSearchPerPage = 50;

[[nodiscard]] MessageIdsList HistoryItemsFromTL(
		not_null<Data::Session*> data,
		const QVector<MTPMessage> &messages) {
	auto result = MessageIdsList();
	for (const auto &message : messages) {
		const auto peerId = PeerFromMessage(message);
		if (data->peerLoaded(peerId)) {
			if (DateFromMessage(message)) {
				const auto item = data->addNewMessage(
					message,
					MessageFlags(),
					NewMessageType::Existing);
				result.push_back(item->fullId());
			}
		} else {
			LOG(("API Error: a search results with not loaded peer %1"
				).arg(peerId.value));
		}
	}
	return result;
}

[[nodiscard]] QString RequestToToken(
		const MessagesSearch::Request &request) {
	auto result = request.query;
	if (request.from) {
		result += '\n' + QString::number(request.from->id.value);
	}
	for (const auto &tag : request.tags) {
		result += '\n';
		if (const auto customId = tag.custom()) {
			result += u"custom"_q + QString::number(customId);
		} else {
			result += u"emoji"_q + tag.emoji();
		}
	}
	switch (request.filter) {
	case SearchFilter::NoFilter: break;
	case SearchFilter::Pinned: result += u"\npinned"_q; break;
	}
	return result;
}

[[nodiscard]] MTPMessagesFilter PrepareFilter(SearchFilter filter) {
	switch (filter) {
	case SearchFilter::Pinned:
		return MTP_inputMessagesFilterPinned();
	case SearchFilter::NoFilter:
		return MTP_inputMessagesFilterEmpty();
	}
	return MTP_inputMessagesFilterEmpty();
}

} // namespace

std::vector<not_null<HistoryItem*>> SearchSecretChatMessages(
		not_null<History*> history,
		const QString &query) {
	auto result = std::vector<not_null<HistoryItem*>>();
	history->owner().enumerateMessages(history->peer->id, [&](
			not_null<HistoryItem*> item) {
		if (!item->isService()
			&& item->originalText().text.contains(
				query,
				Qt::CaseInsensitive)) {
			result.push_back(item);
		}
	});
	ranges::sort(result, ranges::greater(), &HistoryItem::position);
	return result;
}

MgSearch ParseMgSearch(
		not_null<History*> history,
		const QString &query) {
	const auto owner = &history->owner();
	const auto parsed = MG::ParseSearchQuery(query, [&](
			const QString &username) {
		return owner->peerByUsername(username) != nullptr;
	});
	auto result = MgSearch{
		.text = parsed.text,
		.from = (parsed.from.isEmpty()
			? nullptr
			: owner->peerByUsername(parsed.from)),
		.minDate = parsed.minDate,
		.maxDate = parsed.maxDate,
	};
	using Type = MG::SearchType;
	switch (parsed.type) {
	case Type::None: break;
	case Type::Photo: result.filter = MTP_inputMessagesFilterPhotos(); break;
	case Type::Video: result.filter = MTP_inputMessagesFilterVideo(); break;
	case Type::Voice: result.filter = MTP_inputMessagesFilterVoice(); break;
	case Type::Round:
		result.filter = MTP_inputMessagesFilterRoundVideo();
		break;
	case Type::Music: result.filter = MTP_inputMessagesFilterMusic(); break;
	case Type::Gif: result.filter = MTP_inputMessagesFilterGif(); break;
	case Type::Document:
		result.filter = MTP_inputMessagesFilterDocument();
		break;
	case Type::Link: result.filter = MTP_inputMessagesFilterUrl(); break;
	case Type::Contact:
		result.filter = MTP_inputMessagesFilterContacts();
		break;
	case Type::Geo: result.filter = MTP_inputMessagesFilterGeo(); break;
	case Type::Poll: result.filter = MTP_inputMessagesFilterPoll(); break;
	case Type::Mention:
		result.filter = MTP_inputMessagesFilterMyMentions();
		break;
	case Type::Pinned:
		result.filter = MTP_inputMessagesFilterPinned();
		break;
	}
	return result;
}

MessagesSearch::MessagesSearch(not_null<History*> history)
: _history(history) {
}

MessagesSearch::~MessagesSearch() {
	_history->owner().histories().cancelRequest(
		base::take(_searchInHistoryRequest));
}

void MessagesSearch::searchMessages(Request request) {
	_request = std::move(request);
	_offsetId = {};
	searchRequest();
}

void MessagesSearch::searchMore() {
	if (_searchInHistoryRequest || _requestId) {
		return;
	}
	searchRequest();
}

void MessagesSearch::searchRequest() {
	if (_history->peer->isSecretChat()) {
		// Nothing on the server (and no InputPeer to ask with): match the
		// local messages in one go, so total == size and nobody pages.
		auto found = FoundMessages{ 0, {}, RequestToToken(_request) };
		if (_request.filter == SearchFilter::NoFilter) {
			for (const auto &item : SearchSecretChatMessages(
					_history,
					_request.query)) {
				found.messages.push_back(item->fullId());
			}
			found.total = int(found.messages.size());
		}
		_messagesFounds.fire(std::move(found));
		return;
	}
	const auto nextToken = RequestToToken(_request);
	if (!_offsetId) {
		const auto it = _cacheOfStartByToken.find(nextToken);
		if (it != end(_cacheOfStartByToken)) {
			_requestId = 0;
			searchReceived(it->second, _requestId, nextToken);
			return;
		}
	}
	auto callback = [=](Fn<void()> finish) {
		using Flag = MTPmessages_Search::Flag;
		const auto mg = ParseMgSearch(_history, _request.query);
		// A sender picked in the UI wins over a typed from: operator.
		const auto from = _request.from ? _request.from : mg.from;
		const auto fromPeer = _history->peer->isUser() ? nullptr : from;
		const auto savedPeer = _history->peer->isSelf() ? from : nullptr;
		_requestId = _history->session().api().request(MTPmessages_Search(
			MTP_flags((fromPeer ? Flag::f_from_id : Flag())
				| (savedPeer ? Flag::f_saved_peer_id : Flag())
				| (_request.topMsgId ? Flag::f_top_msg_id : Flag())
				| (_request.tags.empty() ? Flag() : Flag::f_saved_reaction)),
			_history->peer->input(),
			MTP_string(mg.text),
			(fromPeer ? fromPeer->input() : MTP_inputPeerEmpty()),
			(savedPeer ? savedPeer->input() : MTP_inputPeerEmpty()),
			MTP_vector_from_range(_request.tags | ranges::views::transform(
				Data::ReactionToMTP
			)),
			MTP_int(_request.topMsgId), // top_msg_id
			(_request.filter == SearchFilter::NoFilter
				? mg.filter
				: PrepareFilter(_request.filter)),
			MTP_int(mg.minDate),
			MTP_int(mg.maxDate),
			MTP_int(_offsetId), // offset_id
			MTP_int(0), // add_offset
			MTP_int(kSearchPerPage),
			MTP_int(0), // max_id
			MTP_int(0), // min_id
			MTP_long(0) // hash
		)).done([=](const TLMessages &result, mtpRequestId id) {
			_searchInHistoryRequest = 0;
			searchReceived(result, id, nextToken);
			finish();
		}).fail([=](const MTP::Error &error, mtpRequestId id) {
			_searchInHistoryRequest = 0;

			if (_requestId == id) {
				_requestId = 0;
			}
			if (error.type() == u"SEARCH_QUERY_EMPTY"_q) {
				_messagesFounds.fire({ 0, MessageIdsList(), nextToken });
			}

			finish();
		}).send();
		return _requestId;
	};
	_searchInHistoryRequest = _history->owner().histories().sendRequest(
		_history,
		Data::Histories::RequestType::History,
		std::move(callback));
}

void MessagesSearch::searchReceived(
		const TLMessages &result,
		mtpRequestId requestId,
		const QString &nextToken) {
	if (requestId != _requestId) {
		return;
	}
	auto &owner = _history->owner();
	auto found = result.match([&](const MTPDmessages_messages &data) {
		if (_requestId != 0) {
			// Don't apply cached data!
			owner.processUsers(data.vusers());
			owner.processChats(data.vchats());
			_history->peer->processTopics(data.vtopics());
		}
		auto items = HistoryItemsFromTL(&owner, data.vmessages().v);
		const auto total = int(data.vmessages().v.size());
		return FoundMessages{ total, std::move(items), nextToken };
	}, [&](const MTPDmessages_messagesSlice &data) {
		if (_requestId != 0) {
			// Don't apply cached data!
			owner.processUsers(data.vusers());
			owner.processChats(data.vchats());
			_history->peer->processTopics(data.vtopics());
		}
		auto items = HistoryItemsFromTL(&owner, data.vmessages().v);
		// data.vnext_rate() is used only in global search.
		const auto total = int(data.vcount().v);
		return FoundMessages{ total, std::move(items), nextToken };
	}, [&](const MTPDmessages_channelMessages &data) {
		if (_requestId != 0) {
			// Don't apply cached data!
			owner.processUsers(data.vusers());
			owner.processChats(data.vchats());
			if (const auto channel = _history->peer->asChannel()) {
				channel->ptsReceived(data.vpts().v);
			} else {
				LOG(("API Error: "
					"received messages.channelMessages when no channel "
					"was passed!"));
			}
			_history->peer->processTopics(data.vtopics());
		}
		auto items = HistoryItemsFromTL(&owner, data.vmessages().v);
		const auto total = int(data.vcount().v);
		return FoundMessages{ total, std::move(items), nextToken };
	}, [](const MTPDmessages_messagesNotModified &data) {
		return FoundMessages{};
	});
	if (!_offsetId) {
		_cacheOfStartByToken.emplace(nextToken, result);
	}
	_requestId = 0;
	_offsetId = found.messages.empty()
		? MsgId()
		: found.messages.back().msg;
	_messagesFounds.fire(std::move(found));
}

rpl::producer<FoundMessages> MessagesSearch::messagesFounds() const {
	return _messagesFounds.events();
}

} // namespace Api
