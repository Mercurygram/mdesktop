/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Data {
class Session;
} // namespace Data

namespace MG {

// "All chats" takes as many pins as the archive (100, 200 with Premium)
// instead of the 5 / 10 Telegram syncs. The full order is kept on this
// device, in the account prefs, and put back over every list the server
// sends; the server only gets the first pins it accepts.

// How many pins of "All chats" the server takes.
[[nodiscard]] int ServerMainPinsLimit(not_null<Data::Session*> owner);

// Stores the current "All chats" pin order. Call after a change made here.
void SaveMainPins(not_null<Data::Session*> owner);

// Re-pins the stored order after the server replaced the pinned list.
void ApplyMainPins(not_null<Data::Session*> owner);

} // namespace MG
