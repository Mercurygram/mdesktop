/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "window/window_peer_menu.h"

class PeerData;
class SecretChatData;

namespace Ui {
class BoxContent;
class PopupMenu;
} // namespace Ui

namespace Window {

class SessionController;

// Secret-chat entries of the peer / history menus. Kept out of
// window_peer_menu.cpp on purpose: that file churns upstream every release
// and every MG line in it is a rebase conflict waiting to happen.

// "Start secret chat" for a user profile.
void AddStartSecretChatAction(
	not_null<SessionController*> controller,
	PeerData *peer,
	const PeerMenuCallback &addAction);

// Main menu "New Secret Chat": a contacts picker whose click starts a
// secret chat with the chosen user instead of opening the regular chat.
[[nodiscard]] object_ptr<Ui::BoxContent> PrepareNewSecretChatBox(
	not_null<SessionController*> controller);

// Self-destruct timer presets (off, 1..15 s, 30 s, 1 min, 1 h, 1 d, 1 w),
// the current one checked. Shared by the peer menu submenu and the composer
// timer button.
void FillSecretChatTtlMenu(
	not_null<Ui::PopupMenu*> menu,
	not_null<SecretChatData*> chat);

// Key fingerprint, self-destruct timer, report spam and delete for a
// secret-chat peer. No-op for any other peer.
void AddSecretChatActions(
	not_null<SessionController*> controller,
	PeerData *peer,
	const PeerMenuCallback &addAction);

} // namespace Window
