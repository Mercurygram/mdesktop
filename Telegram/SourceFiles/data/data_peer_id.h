/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

using BareId = uint64;

struct PeerIdZeroHelper {
};
using PeerIdZero = void(PeerIdZeroHelper::*)();

template <uint8 Shift>
struct ChatIdType {
	BareId bare = 0;

	static constexpr BareId kShift = Shift;
	static constexpr BareId kReservedBit = BareId(0x80);
	static_assert((Shift & kReservedBit) == 0, "Last bit is reserved.");

	constexpr ChatIdType() noexcept = default;
	//constexpr ChatIdType(PeerIdZero) noexcept { // UserId id = 0;
	//}
	constexpr ChatIdType(BareId value) noexcept : bare(value) {
	}
	constexpr ChatIdType(MTPlong value) noexcept : bare(value.v) {
	}

	friend inline constexpr auto operator<=>(
		ChatIdType,
		ChatIdType) = default;

	[[nodiscard]] constexpr explicit operator bool() const noexcept {
		return (bare != 0);
	}
	[[nodiscard]] constexpr bool operator!() const noexcept {
		return !bare;
	}

};

template <uchar Shift>
[[nodiscard]] inline constexpr bool operator==(
		ChatIdType<Shift> a,
		PeerIdZero) noexcept {
	return (a.bare == 0);
}

template <uchar Shift>
[[nodiscard]] inline constexpr bool operator==(
		PeerIdZero,
		ChatIdType<Shift> a) noexcept {
	return (0 == a.bare);
}

template <uchar Shift>
[[nodiscard]] inline constexpr bool operator!=(
		ChatIdType<Shift> a,
		PeerIdZero) noexcept {
	return (a.bare != 0);
}

template <uchar Shift>
[[nodiscard]] inline constexpr bool operator!=(
		PeerIdZero,
		ChatIdType<Shift> a) noexcept {
	return (0 != a.bare);
}

template <uchar Shift>
bool operator<(ChatIdType<Shift>, PeerIdZero) = delete;

template <uchar Shift>
bool operator<(PeerIdZero, ChatIdType<Shift>) = delete;

template <uchar Shift>
bool operator>(ChatIdType<Shift>, PeerIdZero) = delete;

template <uchar Shift>
bool operator>(PeerIdZero, ChatIdType<Shift>) = delete;

template <uchar Shift>
bool operator<=(ChatIdType<Shift>, PeerIdZero) = delete;

template <uchar Shift>
bool operator<=(PeerIdZero, ChatIdType<Shift>) = delete;

template <uchar Shift>
bool operator>=(ChatIdType<Shift>, PeerIdZero) = delete;

template <uchar Shift>
bool operator>=(PeerIdZero, ChatIdType<Shift>) = delete;

using UserId = ChatIdType<0>;
using ChatId = ChatIdType<1>;
using ChannelId = ChatIdType<2>;
// Secret (end-to-end encrypted) chats. These are not real MTProto peers --
// they are addressed by inputEncryptedChat, not InputPeer -- but they get a
// dedicated PeerId namespace so a History / dialog list entry can exist for
// them. The bare id is the int32 secret chat id from encryptedChat.
using SecretChatId = ChatIdType<3>;
using FakeChatId = ChatIdType<0x7F>;

struct PeerIdHelper {
	BareId value = 0;
	constexpr PeerIdHelper(BareId value) noexcept : value(value) {
	}
};

struct PeerId {
	BareId value = 0;
	static constexpr BareId kChatTypeMask = BareId(0xFFFFFFFFFFFFULL);

	constexpr PeerId() noexcept = default;
	constexpr PeerId(PeerIdZero) noexcept { // PeerId id = 0;
	}
	template <uchar Shift>
	constexpr PeerId(ChatIdType<Shift> id) noexcept
	: value(id.bare | (BareId(Shift) << 48)) {
	}
	// This instead of explicit PeerId(BareId) allows to use both
	// PeerId(uint64(..)) and PeerId(0).
	constexpr PeerId(PeerIdHelper value) noexcept : value(value.value) {
	}

	friend inline constexpr auto operator<=>(PeerId, PeerId) = default;

	template <typename SomeChatIdType, BareId = SomeChatIdType::kShift>
	[[nodiscard]] constexpr bool is() const noexcept {
		return ((value >> 48) & BareId(0xFF)) == SomeChatIdType::kShift;
	}

	template <typename SomeChatIdType, BareId = SomeChatIdType::kShift>
	[[nodiscard]] constexpr SomeChatIdType to() const noexcept {
		return is<SomeChatIdType>() ? (value & kChatTypeMask) : 0;
	}

	[[nodiscard]] constexpr explicit operator bool() const noexcept {
		return (value != 0);
	}
	[[nodiscard]] constexpr bool operator!() const noexcept {
		return !value;
	}

};

[[nodiscard]] inline constexpr bool operator==(
		PeerId a,
		PeerIdZero) noexcept {
	return (a.value == 0);
}

[[nodiscard]] inline constexpr bool operator==(
		PeerIdZero,
		PeerId a) noexcept {
	return (0 == a.value);
}

[[nodiscard]] inline constexpr bool operator!=(
		PeerId a,
		PeerIdZero) noexcept {
	return (a.value != 0);
}

[[nodiscard]] inline constexpr bool operator!=(
		PeerIdZero,
		PeerId a) noexcept {
	return (0 != a.value);
}

bool operator<(PeerId, PeerIdZero) = delete;
bool operator<(PeerIdZero, PeerId) = delete;
bool operator>(PeerId, PeerIdZero) = delete;
bool operator>(PeerIdZero, PeerId) = delete;
bool operator<=(PeerId, PeerIdZero) = delete;
bool operator<=(PeerIdZero, PeerId) = delete;
bool operator>=(PeerId, PeerIdZero) = delete;
bool operator>=(PeerIdZero, PeerId) = delete;

[[nodiscard]] inline constexpr bool peerIsUser(PeerId id) noexcept {
	return id.is<UserId>();
}

[[nodiscard]] inline constexpr bool peerIsChat(PeerId id) noexcept {
	return id.is<ChatId>();
}

[[nodiscard]] inline constexpr bool peerIsChannel(PeerId id) noexcept {
	return id.is<ChannelId>();
}

[[nodiscard]] inline constexpr bool peerIsSecretChat(PeerId id) noexcept {
	return id.is<SecretChatId>();
}

[[nodiscard]] inline constexpr PeerId peerFromUser(UserId userId) noexcept {
	return userId;
}

[[nodiscard]] inline constexpr PeerId peerFromChat(ChatId chatId) noexcept {
	return chatId;
}

[[nodiscard]] inline constexpr PeerId peerFromChannel(
		ChannelId channelId) noexcept {
	return channelId;
}

[[nodiscard]] inline constexpr PeerId peerFromSecretChat(
		SecretChatId secretChatId) noexcept {
	return secretChatId;
}

// encryptedChat.id is a *signed* int32 on the wire and is often negative.
// Building a SecretChatId directly from it would sign-extend int32 -> uint64,
// setting high bits that collide with the PeerId namespace byte (bits 48..55)
// and corrupt the id. Zero-extend through uint32 instead. SecretChatData::
// secretChatId() narrows the stored bare value back to the signed int32.
[[nodiscard]] inline constexpr SecretChatId secretChatIdFromWire(
		int32 id) noexcept {
	return SecretChatId(BareId(uint32(id)));
}

[[nodiscard]] inline constexpr PeerId peerFromUser(MTPlong userId) noexcept {
	return peerFromUser(userId.v);
}

[[nodiscard]] inline constexpr PeerId peerFromChat(MTPint chatId) noexcept {
	return peerFromChat(chatId.v);
}

[[nodiscard]] inline constexpr PeerId peerFromChannel(
		MTPint channelId) noexcept {
	return peerFromChannel(channelId.v);
}

[[nodiscard]] inline constexpr UserId peerToUser(PeerId id) noexcept {
	return id.to<UserId>();
}

[[nodiscard]] inline constexpr ChatId peerToChat(PeerId id) noexcept {
	return id.to<ChatId>();
}

[[nodiscard]] inline constexpr ChannelId peerToChannel(PeerId id) noexcept {
	return id.to<ChannelId>();
}

[[nodiscard]] inline constexpr SecretChatId peerToSecretChat(
		PeerId id) noexcept {
	return id.to<SecretChatId>();
}

[[nodiscard]] inline MTPlong peerToBareMTPInt(PeerId id) {
	return MTP_long(id.value & PeerId::kChatTypeMask);
}

[[nodiscard]] PeerId peerFromMTP(const MTPPeer &peer);
[[nodiscard]] MTPpeer peerToMTP(PeerId id);

// Supports both modern and legacy serializations.
[[nodiscard]] PeerId DeserializePeerId(quint64 serialized);
[[nodiscard]] quint64 SerializePeerId(PeerId id);

namespace std {

template <uchar Shift>
struct hash<ChatIdType<Shift>> : private hash<BareId> {
	size_t operator()(ChatIdType<Shift> value) const noexcept {
		return hash<BareId>::operator()(value.bare);
	}
};

template <>
struct hash<PeerId> : private hash<BareId> {
	size_t operator()(PeerId value) const noexcept {
		return hash<BareId>::operator()(value.value);
	}
};

} // namespace std
