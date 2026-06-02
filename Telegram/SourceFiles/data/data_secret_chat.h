/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "data/data_peer.h"
#include "mtproto/mtproto_auth_key.h"

class UserData;

enum class SecretChatState : uchar {
	Empty,      // encryptedChatEmpty -- just allocated.
	Requested,  // We requested, waiting for the other side to accept.
	Waiting,    // The other side requested, waiting for us to accept.
	Ready,      // encryptedChat -- key agreed, ready to message.
	Discarded,  // encryptedChatDiscarded -- closed.
};

// End-to-end encrypted (secret) chat.
//
// Secret chats are not real MTProto peers (they are addressed by
// inputEncryptedChat, not InputPeer), but we model them as a PeerData leaf so
// they get a History and a dialog list entry. The actual conversation partner
// is the referenced UserData.
class SecretChatData final : public PeerData {
public:
	static constexpr auto kKeySize = MTP::AuthKey::kSize; // 256.

	SecretChatData(not_null<Data::Session*> owner, PeerId id);

	[[nodiscard]] int32 secretChatId() const {
		return peerToSecretChat(id).bare;
	}

	void setUser(not_null<UserData*> user);
	[[nodiscard]] UserData *user() const {
		return _user;
	}

	void setState(SecretChatState state);
	[[nodiscard]] SecretChatState state() const {
		return _state;
	}

	void setAccessHash(uint64 accessHash) {
		_accessHash = accessHash;
	}
	[[nodiscard]] uint64 accessHash() const {
		return _accessHash;
	}

	// admin_id == our own id -> we created the chat. This selects the `x`
	// offset in the key derivation and the seq-no parity.
	void setIsCreator(bool creator) {
		_amCreator = creator;
	}
	[[nodiscard]] bool amCreator() const {
		return _amCreator;
	}

	void setKey(bytes::const_span key, uint64 fingerprint);
	[[nodiscard]] bytes::const_span key() const;
	[[nodiscard]] bool hasKey() const {
		return _hasKey;
	}
	[[nodiscard]] uint64 keyFingerprint() const {
		return _keyFingerprint;
	}

	// Perfect-forward-secrecy book-keeping: the current key's age and how many
	// messages it has encrypted (out) / decrypted (in). The two directions are
	// counted separately so the rekey trigger can mirror the mobile clients,
	// which rotate at out >= 100 OR in >= 120 (the asymmetric thresholds let the
	// sender's own out-trigger fire first; the in-trigger is a fallback for when
	// the peer fails to initiate). setKey() resets both (a fresh key is
	// established at handshake and at every rekey commit); setKeyUsage() restores
	// the persisted values on load so the weekly trigger survives a restart.
	void countKeyUseOut() {
		++_keyUseCountOut;
	}
	void countKeyUseIn() {
		++_keyUseCountIn;
	}
	void setKeyUsage(TimeId creationDate, int32 useCountOut, int32 useCountIn) {
		_keyCreationDate = creationDate;
		_keyUseCountOut = useCountOut;
		_keyUseCountIn = useCountIn;
	}
	[[nodiscard]] TimeId keyCreationDate() const {
		return _keyCreationDate;
	}
	[[nodiscard]] int32 keyUseCountOut() const {
		return _keyUseCountOut;
	}
	[[nodiscard]] int32 keyUseCountIn() const {
		return _keyUseCountIn;
	}

	void setLayer(int32 layer) {
		_layer = layer;
	}
	[[nodiscard]] int32 layer() const {
		return _layer;
	}

	void setTtl(int32 ttl) {
		_ttl = ttl;
	}
	[[nodiscard]] int32 ttl() const {
		return _ttl;
	}

	// Sequence numbers, per the MTProto secret chat spec (verified against
	// official mobile clients). The seq-no parity is the OPPOSITE of the crypto
	// x=0/8: the out parity is 1 for the chat creator, 0 otherwise, so our
	// messages carry out_seq_no = sentCount*2 + outParity.
	//
	// The incoming side mirrors the Android client: instead of a received count
	// we remember the last accepted wire out_seq_no of the peer (inSeqNo), so
	// duplicates (remote out_seq_no <= inSeqNo) and gaps (!= inSeqNo + 2) are
	// detectable. The in_seq_no we report is inSeqNo, or inSeqNo + 2 before the
	// first real message -- matching the -2 (creator) / -1 (participant) sentinel
	// the mobile clients seed (see SecretChatHelper.java).
	[[nodiscard]] int32 nextOutSeqNo();
	[[nodiscard]] int32 currentInSeqNo() const;

	// Last accepted peer out_seq_no (the -2 / -1 sentinel until the first
	// message). setInSeqNo() advances it once a message passes the gap checks.
	[[nodiscard]] int32 inSeqNo() const {
		return _seqStarted ? _inSeqNo : (_amCreator ? -2 : -1);
	}
	void setInSeqNo(int32 remoteOutSeqNo) {
		_inSeqNo = remoteOutSeqNo;
		_seqStarted = true;
	}

	// The peer's reported in_seq_no, i.e. the highest of *our* out_seq_no it
	// has acknowledged receiving (distinct from inSeqNo above, which tracks the
	// peer's out_seq_no). Kept in memory only: after a restart the sent cache is
	// empty anyway, so a 0 floor just disables the resend clamp safely. Mirrors
	// SecretChatHelper.java's separate chat.in_seq_no.
	[[nodiscard]] int32 peerInSeqNo() const {
		return _peerInSeqNo;
	}
	void setPeerInSeqNo(int32 peerInSeqNo) {
		_peerInSeqNo = peerInSeqNo;
	}

	[[nodiscard]] int32 rawOutSeqNo() const {
		return _sentCount;
	}
	[[nodiscard]] bool seqStarted() const {
		return _seqStarted;
	}
	// Restore persisted seq-no state: the outgoing counter and the last accepted
	// peer out_seq_no (seqStarted == false keeps the -2 / -1 sentinel for a fresh
	// chat that has not received a message yet).
	void setRawSeqNo(int32 sentCount, int32 inSeqNo, bool seqStarted) {
		_sentCount = sentCount;
		_inSeqNo = inSeqNo;
		_seqStarted = seqStarted;
	}

private:
	void mirrorUser(not_null<UserData*> user);

	UserData *_user = nullptr;
	rpl::lifetime _userLifetime;
	uint64 _accessHash = 0;
	uint64 _keyFingerprint = 0;
	MTP::AuthKey::Data _key = { { gsl::byte{} } };
	TimeId _keyCreationDate = 0;
	int32 _keyUseCountOut = 0;
	int32 _keyUseCountIn = 0;
	int32 _layer = 8;
	int32 _ttl = 0;
	int32 _sentCount = 0;
	int32 _inSeqNo = 0;
	int32 _peerInSeqNo = 0;
	SecretChatState _state = SecretChatState::Empty;
	bool _amCreator = false;
	bool _hasKey = false;
	bool _seqStarted = false;

};

// Fixed green name/icon tint for secret chats (matches mobile), theme-
// independent on purpose -- not a themeable palette key.
[[nodiscard]] QColor SecretChatNameFg();
