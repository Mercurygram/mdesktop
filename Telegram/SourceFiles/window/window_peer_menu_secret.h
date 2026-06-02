/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "window/window_peer_menu.h"

class PeerData;

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

// Key fingerprint, self-destruct timer, report spam and delete for a
// secret-chat peer. No-op for any other peer.
void AddSecretChatActions(
	not_null<SessionController*> controller,
	PeerData *peer,
	const PeerMenuCallback &addAction);

} // namespace Window
