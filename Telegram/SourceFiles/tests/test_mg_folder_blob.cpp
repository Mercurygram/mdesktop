/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/

// Unit tests for the Mercurygram folders wire format. The Android client and
// bots read the same bytes (MercurygramFolders.md in the Android repository),
// so a change that survives a roundtrip here but renames a key would silently
// stop two devices from seeing the same folders.
//
// Built only with -DDESKTOP_APP_TEST_APPS=ON; run the produced
// ./test_mg_folder_blob.

#include "core/mg_folder_blob.h"

#include <QtCore/QString>

#include <iostream>

namespace {

int gChecks = 0;
int gFailures = 0;

void Check(bool condition, const QString &what) {
	++gChecks;
	if (!condition) {
		++gFailures;
		std::cerr << "  FAIL: " << what.toStdString() << std::endl;
	}
}

[[nodiscard]] MG::FolderBlob SampleBlob() {
	auto folder = MG::FolderBlobEntry();
	folder.id = -1234567;
	folder.title = "Work extra";
	folder.titleEntities = { { .offset = 0, .length = 4, .documentId
		= 5307685888880885312LL } };
	folder.titleNoanimate = true;
	folder.emoticon = QString::fromUtf8("\xF0\x9F\x90\xB1"); // A cat.
	folder.color = 3;
	folder.flags = 0x84; // Groups plus exclude_archived.
	folder.include = { -1001234567890LL, 123456789LL };
	folder.exclude = { -987654321LL };
	folder.pinned = { -1001234567890LL };

	auto second = MG::FolderBlobEntry();
	second.id = -42;
	second.title = QString::fromUtf8(
		"\xD0\xA4\xD1\x83\xD1\x82\xD0\xB1\xD0\xBE\xD0\xBB \xF0\x9F\x93\x81");

	auto result = MG::FolderBlob();
	result.updated = 1757181000;
	result.folders = { folder, second };
	return result;
}

void TestRoundtrip() {
	const auto blob = SampleBlob();
	const auto parsed = MG::ParseFolderBlob(MG::SerializeFolderBlob(blob));
	if (!parsed) {
		Check(false, "the serialized blob parses back");
		return;
	}
	Check(parsed->updated == blob.updated, "updated survives");
	Check(parsed->folders.size() == 2, "both folders survive");
	if (parsed->folders.size() != 2) {
		return;
	}
	const auto &first = parsed->folders[0];
	const auto &expected = blob.folders[0];
	Check(first.id == expected.id, "id survives");
	Check(first.title == expected.title, "title survives");
	Check(first.emoticon == expected.emoticon, "the folder icon survives");
	Check(
		first.titleNoanimate == expected.titleNoanimate,
		"title_noanimate survives");
	Check(
		first.titleEntities == expected.titleEntities,
		"title entities survive");
	Check(
		(first.titleEntities.size() == 1)
			&& (first.titleEntities[0].documentId == 5307685888880885312LL),
		"a document id past 2^53 survives to the digit");
	Check(first.color == expected.color, "colour survives");
	Check(first.flags == expected.flags, "flags survive");
	Check(first.include == expected.include, "include survives");
	Check(first.exclude == expected.exclude, "exclude survives");
	Check(first.pinned == expected.pinned, "pinned survives");
	Check(
		parsed->folders[1].title == blob.folders[1].title,
		"a non-Latin title survives");
	Check(
		parsed->folders[1].color == -1,
		"a folder with no colour reads back as -1");
	Check(
		parsed->folders[1].emoticon.isEmpty()
			&& !parsed->folders[1].titleNoanimate
			&& parsed->folders[1].titleEntities.empty(),
		"a folder with no icon and a plain title reads back with none");

	// Two devices decide whether the copy in Saved Messages is already up to
	// date by comparing these bytes, so the same set must serialize the same.
	Check(
		MG::SerializeFolderBlob(*parsed) == MG::SerializeFolderBlob(blob),
		"serializing is stable");
}

void TestFormat() {
	const auto json = MG::SerializeFolderBlob(SampleBlob());
	Check(
		json.contains("\"format\":\"mercurygram-folders\""),
		"the format marker is written");
	Check(json.contains("\"version\":1"), "the version is written");
	Check(
		json.contains("\"exclude_archived\":true"),
		"a flag is written under its TL name");
	Check(
		json.contains("\"non_contacts\":false"),
		"an unset flag is written too");
	Check(json.contains("\"emoticon\":\""), "the folder icon is written");
	Check(
		json.contains("\"title_noanimate\":true"),
		"title_noanimate is written");
	// A JSON number is a double in this reader, which would round a document
	// id past 2^53 into a different emoji, so it goes out as digits.
	Check(
		json.contains("\"document_id\":\"5307685888880885312\""),
		"a title entity document id is written as a decimal string");
	Check(
		!json.contains("\"title_entities\":[]"),
		"a folder with a plain title carries no entity list");
}

void TestRejects() {
	const auto marker = QByteArray("\"format\":\"mercurygram-folders\",");
	Check(!MG::ParseFolderBlob("not json at all"), "garbage is refused");
	Check(!MG::ParseFolderBlob("[]"), "a non-object document is refused");
	Check(
		!MG::ParseFolderBlob(
			"{\"version\":1,\"updated\":1,\"folders\":[]}"),
		"a document without the format marker is refused");
	Check(
		!MG::ParseFolderBlob(
			"{\"format\":\"something-else\","
			"\"version\":1,\"updated\":1,\"folders\":[]}"),
		"another tool's format marker is refused");
	Check(
		!MG::ParseFolderBlob(
			"{" + marker + "\"version\":2,\"updated\":1,\"folders\":[]}"),
		"an unknown version is refused");
	Check(
		!MG::ParseFolderBlob(
			"{" + marker + "\"version\":1,\"updated\":1,"
			"\"folders\":[{\"id\":3}]}"),
		"a server-range folder id is refused");
	Check(
		!MG::ParseFolderBlob(
			"{" + marker + "\"version\":1,\"updated\":1,"
			"\"folders\":[{\"id\":-1}]}"),
		"the reserved -1 folder id is refused");
	const auto empty = MG::ParseFolderBlob(
		"{" + marker + "\"version\":1,\"updated\":7,\"folders\":[]}");
	Check(empty && empty->folders.empty(), "an empty folder list is fine");
	Check(empty && empty->updated == 7, "an empty folder list keeps updated");

	// A palette index straight from the file would otherwise be truncated
	// into some random slot of the 64-colour palette.
	const auto color = MG::ParseFolderBlob(
		"{" + marker + "\"version\":1,\"updated\":1,"
		"\"folders\":[{\"id\":-5,\"color\":900}]}");
	Check(
		color && (color->folders.size() == 1)
			&& (color->folders[0].color == -1),
		"an out-of-range colour reads back as no colour");
}

void TestTitleEntities() {
	const auto marker = QByteArray("\"format\":\"mercurygram-folders\",");
	const auto parse = [&](const QByteArray &entities) {
		return MG::ParseFolderBlob(
			"{" + marker + "\"version\":1,\"updated\":1,"
			"\"folders\":[{\"id\":-5,\"title\":\"abcd\","
			"\"title_entities\":" + entities + "}]}");
	};
	const auto entities = [&](const QByteArray &json) {
		const auto blob = parse(json);
		Check(
			blob && (blob->folders.size() == 1),
			"a folder with title entities parses");
		return (blob && (blob->folders.size() == 1))
			? blob->folders[0].titleEntities
			: std::vector<MG::TitleEntity>();
	};
	const auto good = QByteArray(
		"[{\"offset\":0,\"length\":4,\"document_id\":\"12\"}]");
	Check(entities(good).size() == 1, "a title entity inside the title is kept");
	Check(
		entities("[{\"offset\":4,\"length\":1,\"document_id\":\"12\"}]")
			.empty(),
		"a title entity past the end of the title is dropped");
	Check(
		entities("[{\"offset\":-1,\"length\":2,\"document_id\":\"12\"}]")
			.empty(),
		"a negative title entity offset is dropped");
	Check(
		entities("[{\"offset\":3,\"length\":2,\"document_id\":\"12\"}]")
			.empty(),
		"a title entity one past the end of the title is dropped");
	Check(
		entities("[{\"offset\":0,\"length\":0,\"document_id\":\"12\"}]")
			.empty(),
		"an empty title entity is dropped");
	Check(
		entities("[{\"offset\":0,\"length\":4,\"document_id\":\"0\"}]")
			.empty(),
		"a title entity without a document id is dropped");
	Check(
		entities("[{\"offset\":0,\"length\":4,\"document_id\":\"nope\"}]")
			.empty(),
		"a title entity with a document id that is not a number is dropped");
	Check(
		entities("\"not an array\"").empty(),
		"title entities that are not an array read back as none");
	Check(entities("[7]").empty(), "a title entity that is not an object is dropped");

	// The document is refused by nothing here: a folder list with one bad
	// entity is still a folder list, and the icon is the only thing lost.
	const auto old = MG::ParseFolderBlob(
		"{" + marker + "\"version\":1,\"updated\":1,"
		"\"folders\":[{\"id\":-5,\"title\":\"abcd\"}]}");
	Check(
		old && (old->folders.size() == 1)
			&& old->folders[0].emoticon.isEmpty()
			&& !old->folders[0].titleNoanimate
			&& old->folders[0].titleEntities.empty(),
		"a document written before these keys still parses");

	// One emoji, whatever the sequence: anything longer is not an icon, and a
	// cut in the middle of a surrogate pair is not a string, so it is dropped
	// whole.
	const auto emoticon = MG::ParseFolderBlob(
		"{" + marker + "\"version\":1,\"updated\":1,"
		"\"folders\":[{\"id\":-5,\"emoticon\":\""
		+ QByteArray("x").repeated(64) + "\"}]}");
	Check(
		emoticon && (emoticon->folders.size() == 1)
			&& emoticon->folders[0].emoticon.isEmpty(),
		"an over-long folder icon reads back as none");
}

void TestMercurygramIds() {
	Check(
		!MG::IsMercurygramFolderId(0),
		"the All chats folder is a server one");
	Check(
		!MG::IsMercurygramFolderId(2),
		"the first server folder is a server one");
	Check(MG::IsMercurygramFolderId(-1), "a negative id is a Mercurygram one");
	Check(
		MG::NextServerFolderId({ 0, 2, 3, -42 }) == 4,
		"the next server id skips the taken ones and ignores negative ones");
}

void TestMarkedPeerIds() {
	using Kind = MG::MarkedPeer::Kind;
	const auto user = MG::MarkedPeer{ Kind::User, 123456789 };
	const auto chat = MG::MarkedPeer{ Kind::Chat, 987654321 };
	const auto channel = MG::MarkedPeer{ Kind::Channel, 1234567890 };
	Check(MG::ToMarkedPeerId(user) == 123456789LL, "a user id is itself");
	Check(MG::ToMarkedPeerId(chat) == -987654321LL, "a basic group is -id");
	Check(
		MG::ToMarkedPeerId(channel) == -1001234567890LL,
		"a channel is -1000000000000 - id");
	for (const auto peer : { user, chat, channel }) {
		const auto back = MG::FromMarkedPeerId(MG::ToMarkedPeerId(peer));
		Check(back && *back == peer, "a marked id converts back");
	}
	Check(!MG::FromMarkedPeerId(0), "zero is not a peer");
}

} // namespace

int main(int argc, char *argv[]) {
	TestRoundtrip();
	TestFormat();
	TestRejects();
	TestTitleEntities();
	TestMercurygramIds();
	TestMarkedPeerIds();

	std::cout << "\n" << (gChecks - gFailures) << "/" << gChecks
		<< " checks passed" << std::endl;
	if (gFailures > 0) {
		std::cerr << gFailures << " check(s) FAILED" << std::endl;
		return 1;
	}
	std::cout << "OK" << std::endl;
	return 0;
}
