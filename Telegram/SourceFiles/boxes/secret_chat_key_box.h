/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

class SecretChatData;

namespace Ui {
class GenericBox;
} // namespace Ui

// Shows the secret-chat key visualization (identicon) so the user can compare
// it with the image on the partner's device and confirm the end-to-end key.
void SecretChatKeyBox(
	not_null<Ui::GenericBox*> box,
	not_null<SecretChatData*> chat);
