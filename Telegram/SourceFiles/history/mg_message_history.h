/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

class HistoryItem;

namespace Ui {
class PopupMenu;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace MG {

// "Keep deleted & edited messages": a message the server deletes stays in
// the chat, marked deleted, and an edit keeps the text it replaced. Only in
// memory, for the messages loaded at that moment: desktop has no local
// message store to keep them in across a restart. Self-destructing
// messages and secret chats are left alone.

// Called for a message the server deleted. True when it was kept instead,
// so the caller must not destroy it.
[[nodiscard]] bool KeepDeletedMessage(not_null<HistoryItem*> item);

[[nodiscard]] bool IsDeletedMessage(not_null<const HistoryItem*> item);

// Called with the message as it is right before a server edit applies.
void RememberEditedText(not_null<HistoryItem*> item);

// "Edit history", when earlier versions were kept.
void AddEditHistoryAction(
	not_null<Ui::PopupMenu*> menu,
	not_null<HistoryItem*> item,
	not_null<Window::SessionController*> controller);

} // namespace MG
