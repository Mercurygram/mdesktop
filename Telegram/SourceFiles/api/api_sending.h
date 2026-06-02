/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "data/data_file_origin.h"

#include <QtCore/QString>

#include <memory>
#include <vector>

class History;
class HistoryItem;
class PhotoData;
class DocumentData;
class SecretChatData;
struct FilePrepareResult;

namespace Data {
struct InputVenue;
} // namespace Data

namespace Main {
class Session;
} // namespace Main

namespace Api {

struct MessageToSend;
struct SendAction;
struct MusicSelectionItem {
	not_null<DocumentData*> document;
	Data::FileOrigin origin;
};

void SendExistingDocument(
	MessageToSend &&message,
	not_null<DocumentData*> document,
	std::optional<MsgId> localMessageId = std::nullopt);

void SendMusicSelection(
	MessageToSend &&message,
	std::vector<MusicSelectionItem> items);

void SendExistingPhoto(
	MessageToSend &&message,
	not_null<PhotoData*> photo,
	std::optional<MsgId> localMessageId = std::nullopt);

// Whether ForwardToSecretChat() would find something to send: a whitelist, so
// a media type added upstream later is refused rather than silently dropped.
// Polls, todo lists, invoices, games, dice, giveaways, stories, wallpapers,
// gifts, calls and service messages all fail it.
[[nodiscard]] bool CanForwardToSecretChat(not_null<const HistoryItem*> item);

// Composer caption tags -> entities, prepared for sending (also used by the
// secret-chat upload path).
[[nodiscard]] TextWithEntities PrepareConfirmedFileCaption(
	not_null<History*> history,
	not_null<Main::Session*> session,
	const std::shared_ptr<FilePrepareResult> &file);

// The secret layer has no forward: a forwarded item is re-sent as a new
// encrypted message carrying a copy of its content, the way the Android client
// does it, so it arrives without a forward header. dropCaption mirrors the
// NoNamesAndCaptions forward option (the sender name has no wire form here).
// Returns false when the item carried nothing that could be sent.
bool ForwardToSecretChat(
	not_null<SecretChatData*> chat,
	not_null<HistoryItem*> item,
	SendAction action,
	bool dropCaption);

bool SendDice(MessageToSend &message);

// We can't create Data::LocationPoint() and use it
// for a local sending message, because we can't request
// map thumbnail in messages history without access hash.
void SendLocation(SendAction action, float64 lat, float64 lon);

void SendVenue(SendAction action, Data::InputVenue venue);

void FillMessagePostFlags(
	const SendAction &action,
	not_null<PeerData*> peer,
	MessageFlags &flags);

void SendConfirmedFile(
	not_null<Main::Session*> session,
	const std::shared_ptr<FilePrepareResult> &file);

} // namespace Api
