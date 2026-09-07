/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

// Included rather than left to stdafx.h: the unit test builds this pair alone,
// without the application's precompiled header.
#include <QtCore/QByteArray>
#include <QtCore/QString>

#include <algorithm>
#include <optional>
#include <vector>

namespace MG {

// The JSON document Mercurygram uses for its own folders, both as the on-disk
// copy of this device's set and as the copy kept in Saved Messages so several
// devices share one set. The Android client and folder-aware bots read and
// write the same bytes; the format is specified in MercurygramFolders.md at
// the root of the Android repository (Mercurygram/Mercurygram), which is the
// single source for it. Bump kVersion there and here together.
//
// Peers are Bot API marked ids (user id, basic group -id, channel
// -1000000000000 - id) rather than anything client-specific, so the format
// stays readable from Telethon and from the phone. This pair holds no
// application type at all, which is what lets the format be unit-tested
// without a Main::Session.
constexpr auto kFolderBlobFormat = "mercurygram-folders";
constexpr auto kFolderBlobVersion = 1;

// A folder id below zero is the whole discriminator for a Mercurygram folder:
// Telegram hands out 0 (All chats) and >= 2, so the sign can never collide.
[[nodiscard]] inline bool IsMercurygramFolderId(int id) {
	return (id < 0);
}

// The next free server folder id: 0 is All chats and 1 is never handed out, so
// the search starts at 2 and skips the ids already taken. Mercurygram ids are
// below zero, so they are skipped by the range and never returned.
[[nodiscard]] inline int NextServerFolderId(const std::vector<int> &taken) {
	auto id = 2;
	while (std::find(begin(taken), end(taken), id) != end(taken)) {
		++id;
	}
	return id;
}

// A custom emoji in the folder title: the only entity kind a title can hold.
// The document id is global, so it means the same emoji on every device.
struct TitleEntity {
	int offset = 0;
	int length = 0;
	qint64 documentId = 0;

	friend inline bool operator==(TitleEntity, TitleEntity) = default;
};

struct FolderBlobEntry {
	int id = 0;
	QString title;
	std::vector<TitleEntity> titleEntities;
	bool titleNoanimate = false;
	QString emoticon; // The folder icon, empty for the default one.
	int color = -1; // -1 for no folder tag colour.

	// The dialogFilter rule flags, in the TL order the JSON names follow, which
	// is also the order of the low bits of Data::ChatFilter::Flag.
	unsigned int flags = 0;

	std::vector<qint64> include;
	std::vector<qint64> exclude;
	std::vector<qint64> pinned; // Ordered, and a subset of include.
};

struct FolderBlob {
	int updated = 0; // Unix seconds of the last write; last writer wins.
	std::vector<FolderBlobEntry> folders;
};

[[nodiscard]] QByteArray SerializeFolderBlob(const FolderBlob &blob);

// Empty result for anything that is not a folder blob of a version we know:
// any account can hold a file with that name in Saved Messages.
[[nodiscard]] std::optional<FolderBlob> ParseFolderBlob(const QByteArray &json);

struct MarkedPeer {
	enum class Kind {
		User,
		Chat,
		Channel,
	};
	Kind kind = Kind::User;
	qint64 bare = 0;

	friend inline bool operator==(MarkedPeer, MarkedPeer) = default;
};

[[nodiscard]] std::optional<MarkedPeer> FromMarkedPeerId(qint64 marked);
[[nodiscard]] qint64 ToMarkedPeerId(MarkedPeer peer);

} // namespace MG
