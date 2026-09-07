/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/mg_folder_blob.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

#include <iterator>

namespace MG {
namespace {

// The channel half of the Bot API id space starts here: a channel is written
// as -1000000000000 - id, a basic group as -id.
constexpr auto kChannelMarkedBase = qint64(-1000000000000LL);

// The dialogFilter flag names, in the order of the low bits of
// Data::ChatFilter::Flag, so a reader can turn the set back into a native
// filter and reuse its own membership logic.
const char *const kFlagNames[] = {
	"contacts",
	"non_contacts",
	"groups",
	"broadcasts",
	"bots",
	"exclude_muted",
	"exclude_read",
	"exclude_archived",
};
constexpr auto kFlagCount = int(std::size(kFlagNames));

// The folder tag colour is a small palette index; the palette the readers use
// holds 64 entries. Below zero already means "no tag".
constexpr auto kMaxColorIndex = 63;

// A folder icon is one emoji and a folder title holds at most a handful of
// custom emojis, so anything past these bounds is not a folder this format
// describes.
constexpr auto kMaxEmoticonLength = 32;
constexpr auto kMaxTitleEntities = 32;

[[nodiscard]] QJsonArray SerializeIds(const std::vector<qint64> &ids) {
	auto result = QJsonArray();
	for (const auto id : ids) {
		result.append(QJsonValue(id));
	}
	return result;
}

[[nodiscard]] std::optional<std::vector<qint64>> ParseIds(
		const QJsonValue &value) {
	if (value.isUndefined() || value.isNull()) {
		return std::vector<qint64>();
	} else if (!value.isArray()) {
		return {};
	}
	auto result = std::vector<qint64>();
	for (const auto &entry : value.toArray()) {
		if (!entry.isDouble()) {
			return {};
		}
		result.push_back(qint64(entry.toDouble()));
	}
	return result;
}

// An entity a reader cannot place is left out rather than the document
// refused: the title it points into is the one the writer had, and a folder
// list with one bad entity is still a folder list. Offsets are UTF-16 units,
// as in TextWithEntities and in a Java String, so both clients count them the
// same way.
[[nodiscard]] std::vector<TitleEntity> ParseTitleEntities(
		const QJsonValue &value,
		int titleLength) {
	auto result = std::vector<TitleEntity>();
	if (!value.isArray()) {
		return result;
	}
	for (const auto &element : value.toArray()) {
		if (int(result.size()) >= kMaxTitleEntities) {
			break;
		} else if (!element.isObject()) {
			continue;
		}
		const auto object = element.toObject();
		auto entity = TitleEntity();
		entity.offset = object.value(QStringLiteral("offset")).toInt(-1);
		entity.length = object.value(QStringLiteral("length")).toInt();
		entity.documentId = object.value(
			QStringLiteral("document_id")).toString().toLongLong();
		if (entity.offset < 0
			|| entity.length <= 0
			|| entity.offset + entity.length > titleLength
			|| !entity.documentId) {
			continue;
		}
		result.push_back(entity);
	}
	return result;
}

} // namespace

QByteArray SerializeFolderBlob(const FolderBlob &blob) {
	auto folders = QJsonArray();
	for (const auto &folder : blob.folders) {
		auto flags = QJsonObject();
		for (auto i = 0; i != kFlagCount; ++i) {
			flags.insert(
				QString::fromLatin1(kFlagNames[i]),
				((folder.flags & (1U << i)) != 0));
		}
		auto entry = QJsonObject();
		entry.insert(QStringLiteral("id"), folder.id);
		entry.insert(QStringLiteral("title"), folder.title);
		if (!folder.titleEntities.empty()) {
			auto entities = QJsonArray();
			for (const auto &entity : folder.titleEntities) {
				auto object = QJsonObject();
				object.insert(QStringLiteral("offset"), entity.offset);
				object.insert(QStringLiteral("length"), entity.length);
				// A decimal string, not a number: a document id uses the
				// full 64 bits and a JSON number is a double here, which
				// would round the ones past 2^53 into a different emoji.
				object.insert(
					QStringLiteral("document_id"),
					QString::number(entity.documentId));
				entities.append(object);
			}
			entry.insert(QStringLiteral("title_entities"), entities);
		}
		if (folder.titleNoanimate) {
			entry.insert(QStringLiteral("title_noanimate"), true);
		}
		if (!folder.emoticon.isEmpty()) {
			entry.insert(QStringLiteral("emoticon"), folder.emoticon);
		}
		entry.insert(QStringLiteral("color"), folder.color);
		entry.insert(QStringLiteral("flags"), flags);
		entry.insert(QStringLiteral("include"), SerializeIds(folder.include));
		entry.insert(QStringLiteral("exclude"), SerializeIds(folder.exclude));
		entry.insert(QStringLiteral("pinned"), SerializeIds(folder.pinned));
		folders.append(entry);
	}
	auto root = QJsonObject();
	root.insert(QStringLiteral("format"), QString::fromLatin1(kFolderBlobFormat));
	root.insert(QStringLiteral("version"), kFolderBlobVersion);
	root.insert(QStringLiteral("updated"), blob.updated);
	root.insert(QStringLiteral("folders"), folders);
	return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

std::optional<FolderBlob> ParseFolderBlob(const QByteArray &json) {
	const auto document = QJsonDocument::fromJson(json);
	if (!document.isObject()) {
		return {};
	}
	const auto root = document.object();
	if (root.value(QStringLiteral("format")).toString()
		!= QString::fromLatin1(kFolderBlobFormat)) {
		return {}; // Some other JSON that happens to carry that file name.
	} else if (root.value(QStringLiteral("version")).toInt()
		!= kFolderBlobVersion) {
		return {};
	}
	const auto folders = root.value(QStringLiteral("folders"));
	if (!folders.isArray()) {
		return {};
	}
	auto result = FolderBlob();
	result.updated = root.value(QStringLiteral("updated")).toInt();
	for (const auto &value : folders.toArray()) {
		if (!value.isObject()) {
			return {};
		}
		const auto object = value.toObject();
		auto folder = FolderBlobEntry();
		folder.id = object.value(QStringLiteral("id")).toInt();
		if (folder.id >= -1) {
			// Server range, or the reserved -1 (see MercurygramFolders.md).
			return {};
		}
		folder.title = object.value(QStringLiteral("title")).toString();
		folder.titleEntities = ParseTitleEntities(
			object.value(QStringLiteral("title_entities")),
			folder.title.size());
		folder.titleNoanimate = object.value(
			QStringLiteral("title_noanimate")).toBool();
		folder.emoticon = object.value(QStringLiteral("emoticon")).toString();
		if (folder.emoticon.size() > kMaxEmoticonLength) {
			folder.emoticon = QString();
		}
		folder.color = object.value(QStringLiteral("color")).toInt(-1);
		if (folder.color > kMaxColorIndex) {
			// Anyone can put a file with that name in Saved Messages, and the
			// reader hands this straight to a palette lookup: out of range
			// reads as "no colour" rather than as some random palette slot.
			folder.color = -1;
		}
		const auto flags = object.value(QStringLiteral("flags")).toObject();
		for (auto i = 0; i != kFlagCount; ++i) {
			if (flags.value(QString::fromLatin1(kFlagNames[i])).toBool()) {
				folder.flags |= (1U << i);
			}
		}
		const auto include = ParseIds(object.value(QStringLiteral("include")));
		const auto exclude = ParseIds(object.value(QStringLiteral("exclude")));
		const auto pinned = ParseIds(object.value(QStringLiteral("pinned")));
		if (!include || !exclude || !pinned) {
			return {};
		}
		folder.include = *include;
		folder.exclude = *exclude;
		folder.pinned = *pinned;
		result.folders.push_back(std::move(folder));
	}
	return result;
}

std::optional<MarkedPeer> FromMarkedPeerId(qint64 marked) {
	if (marked > 0) {
		return MarkedPeer{ MarkedPeer::Kind::User, marked };
	} else if (marked <= kChannelMarkedBase) {
		return MarkedPeer{
			MarkedPeer::Kind::Channel,
			kChannelMarkedBase - marked,
		};
	} else if (marked < 0) {
		return MarkedPeer{ MarkedPeer::Kind::Chat, -marked };
	}
	return {};
}

qint64 ToMarkedPeerId(MarkedPeer peer) {
	switch (peer.kind) {
	case MarkedPeer::Kind::User: return peer.bare;
	case MarkedPeer::Kind::Chat: return -peer.bare;
	case MarkedPeer::Kind::Channel: return kChannelMarkedBase - peer.bare;
	}
	return 0;
}

} // namespace MG
