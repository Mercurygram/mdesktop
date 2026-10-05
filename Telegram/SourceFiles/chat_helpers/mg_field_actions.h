/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

class PeerData;

namespace Main {
class SessionShow;
} // namespace Main

namespace Ui {
class InputField;
} // namespace Ui

namespace MG {

// Adds "Mention" (turns the selected text into a mention of a contact) and
// "Translate" (replaces the selected text with its translation) to the
// context menu of a message composer. Neither is offered in a secret chat:
// a translation would send the text to a server in the clear.
void AddFieldSelectionActions(
	std::shared_ptr<Main::SessionShow> show,
	not_null<Ui::InputField*> field,
	Fn<PeerData*()> peer);

} // namespace MG
