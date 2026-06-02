/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "data/data_secret_chat.h"

#include "data/data_user.h"
#include "data/data_changes.h"
#include "data/data_session.h"
#include "main/main_session.h"
#include "apiwrap.h"
#include "api/api_encrypted_chats.h"
#include "base/unixtime.h"
#include "lang/lang_keys.h"
#include "ui/text/format_values.h" // Ui::FormatPhone

SecretChatData::SecretChatData(not_null<Data::Session*> owner, PeerId id)
: PeerData(owner, id) {
}

QColor SecretChatNameFg() {
	return QColor(0x4d, 0xcd, 0x5e);
}

void SecretChatData::setUser(not_null<UserData*> user) {
	_user = user;
	_userLifetime.destroy();

	// Secret chats have no name/photo of their own; mirror the partner user so
	// the dialog row and top bar render correctly. The user may be restored
	// minimal (no name) on startup, so keep mirroring as its data loads.
	mirrorUser(user);
	session().changes().peerUpdates(
		user,
		Data::PeerUpdate::Flag::Name | Data::PeerUpdate::Flag::Photo
	) | rpl::on_next([=](const Data::PeerUpdate &update) {
		mirrorUser(update.peer->asUser());
	}, _userLifetime);
}

void SecretChatData::mirrorUser(not_null<UserData*> user) {
	// A restored / mobile-initiated partner can be a minimal UserData that has
	// not loaded yet (no name). Mirror its name when present; otherwise fall
	// back to the phone, and finally to a generic label, so the dialog row never
	// shows a raw numeric id. The Name/Photo subscription re-runs this once the
	// real user data loads, replacing the fallback.
	auto name = user->name();
	if (name.isEmpty()) {
		name = user->phone().isEmpty()
			? tr::lng_secret_chat_request_title(tr::now)
			: Ui::FormatPhone(user->phone());
	}
	updateNameDelayed(name, QString(), user->username());
	// Re-render any self-destruct timer notices now that the name is current --
	// a restored notice can otherwise keep the "Secret chat" fallback baked in at
	// restore time (this fires on the same name-resolution that updates the row).
	session().api().encryptedChats().refreshTtlNotices(this);
	if (user->hasUserpic()) {
		setUserpic(
			user->userpicPhotoId(),
			user->userpicLocation(),
			user->userpicHasVideo());
	} else {
		clearUserpic();
	}
}

void SecretChatData::setState(SecretChatState state) {
	if (_state == state) {
		return;
	}
	_state = state;
	// The composer is disabled until the chat is Ready (see
	// HistoryWidget::updateCanSendMessage / computeSendRestriction), so notify the
	// UI to re-evaluate the send restriction when the state changes.
	session().changes().peerUpdated(this, Data::PeerUpdate::Flag::Rights);
}

void SecretChatData::setKey(bytes::const_span key, uint64 fingerprint) {
	Expects(key.size() == kKeySize);

	bytes::copy(_key, key);
	_keyFingerprint = fingerprint;
	_hasKey = true;

	// A freshly established key (handshake or rekey commit) starts its age and
	// usage from zero. readSecretChats() overrides these via setKeyUsage() with
	// the persisted values so the weekly trigger survives a restart.
	_keyCreationDate = base::unixtime::now();
	_keyUseCountOut = 0;
	_keyUseCountIn = 0;
}

bytes::const_span SecretChatData::key() const {
	Expects(_hasKey);

	return _key;
}

int32 SecretChatData::nextOutSeqNo() {
	// NB: the seq-no parity is the OPPOSITE of the crypto `x` (0/8). Verified
	// against official mobile clients: the chat creator sends odd out_seq_no
	// (participant sends even), so x = 1 for the creator here.
	const auto x = _amCreator ? 1 : 0;
	const auto result = _sentCount * 2 + x;
	++_sentCount;
	return result;
}

int32 SecretChatData::currentInSeqNo() const {
	// Report the last accepted peer out_seq_no. Before the first message the
	// sentinel is -2 (creator) / -1 (participant); the +2 then yields the 0 / 1
	// the mobile clients expect for the opening message (SecretChatHelper.java).
	const auto seq = inSeqNo();
	return (seq > 0) ? seq : (seq + 2);
}
