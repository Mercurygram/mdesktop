/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "iv/iv_rich_message_serializer.h"

#include "base/flat_map.h"
#include "data/data_document.h"
#include "data/data_photo.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "data/stickers/data_custom_emoji.h"
#include "history/history.h"
#include "history/history_item.h"
#include "iv/markdown/iv_markdown_prepare.h"
#include "iv/markdown/iv_markdown_prepare_links.h"
#include "iv/markdown/iv_markdown_prepare_serialize.h"
#include "main/main_session.h"
#include "ui/text/text_utilities.h"

#include <algorithm>

namespace Iv {
namespace {

using Block = RichPage::Block;
using BlockKind = RichPage::BlockKind;
using GroupedMediaItem = RichPage::GroupedMediaItem;
using ListKind = RichPage::ListKind;
using RichText = RichPage::RichText;
using TableAlignment = RichPage::TableAlignment;
using TableCell = RichPage::TableCell;
using TableVerticalAlignment = RichPage::TableVerticalAlignment;
using TaskState = RichPage::TaskState;

constexpr auto kDefaultMapWidth = 400;
constexpr auto kDefaultMapHeight = 200;
constexpr auto kNoEntityIndex = -1;

[[nodiscard]] QString FormulaTexFromSource(QString source) {
	source = source.trimmed();
	if (source.size() >= 2
		&& source.front() == QChar('$')
		&& source.back() == QChar('$')) {
		source = source.mid(1, source.size() - 2).trimmed();
	}
	return source;
}

[[nodiscard]] bool StringIsEmpty(const QString &text) {
	return text.trimmed().isEmpty();
}

struct SerializeContext {
	not_null<Main::Session*> session;
	SerializeInputRichMessageMode mode = SerializeInputRichMessageMode::Draft;
	base::flat_map<uint64, MTPInputPhoto> photos;
	base::flat_map<uint64, MTPInputDocument> documents;
	base::flat_map<uint64, MTPInputUser> users;
};

enum class SerializeBlockState : uchar {
	Success,
	Skipped,
	Failed,
};

struct SerializeBlockResult {
	SerializeBlockState state = SerializeBlockState::Failed;
	std::optional<MTPPageBlock> block;
};

[[nodiscard]] bool IsFinalSerializeMode(const SerializeContext *context) {
	return context->mode == SerializeInputRichMessageMode::FinalSubmit;
}

[[nodiscard]] SerializeBlockResult SuccessfulSerializeBlock(
		MTPPageBlock block) {
	auto result = SerializeBlockResult();
	result.state = SerializeBlockState::Success;
	result.block = std::move(block);
	return result;
}

[[nodiscard]] SerializeBlockResult SkippedSerializeBlock() {
	auto result = SerializeBlockResult();
	result.state = SerializeBlockState::Skipped;
	return result;
}

[[nodiscard]] SerializeBlockResult FailedSerializeBlock() {
	return {};
}

[[nodiscard]] SerializeBlockResult FinishSerializeBlock(
		std::optional<MTPPageBlock> block) {
	return block
		? SuccessfulSerializeBlock(std::move(*block))
		: FailedSerializeBlock();
}

[[nodiscard]] int EntitySerializationOrder(EntityType type) {
	switch (type) {
	case EntityType::CustomUrl: return 0;
	case EntityType::MentionName: return 1;
	case EntityType::Bold: return 2;
	case EntityType::Italic: return 3;
	case EntityType::Underline: return 4;
	case EntityType::StrikeOut: return 5;
	case EntityType::Code: return 6;
	case EntityType::Subscript: return 7;
	case EntityType::Superscript: return 8;
	case EntityType::Marked: return 9;
	case EntityType::Spoiler: return 10;
	case EntityType::CustomEmoji: return 11;
	case EntityType::FormattedDate: return 12;
	case EntityType::Mention: return 13;
	case EntityType::Hashtag: return 14;
	case EntityType::BotCommand: return 15;
	case EntityType::Cashtag: return 16;
	case EntityType::Url: return 17;
	case EntityType::Email: return 18;
	case EntityType::Phone: return 19;
	case EntityType::BankCard: return 20;
	case EntityType::Invalid:
	case EntityType::Semibold:
	case EntityType::MediaTimestamp:
	case EntityType::Colorized:
	case EntityType::Pre:
	case EntityType::Blockquote:
		break;
	}
	return 100;
}

[[nodiscard]] MTPRichText MakePlainRichText(const QString &text) {
	return text.isEmpty()
		? MTP_textEmpty()
		: MTP_textPlain(MTP_string(text));
}

[[nodiscard]] MTPRichText JoinRichTextParts(QVector<MTPRichText> &&parts) {
	if (parts.isEmpty()) {
		return MTP_textEmpty();
	} else if (parts.size() == 1) {
		return std::move(parts.front());
	}
	return MTP_textConcat(MTP_vector<MTPRichText>(std::move(parts)));
}

[[nodiscard]] MTPRichText WrapRichTextAnchor(
		MTPRichText text,
		const QString &anchorId) {
	return anchorId.isEmpty()
		? text
		: MTP_textAnchor(std::move(text), MTP_string(anchorId));
}

[[nodiscard]] bool HasRichTextContent(const RichText &text) {
	return !text.text.empty() || !text.anchorId.isEmpty();
}

[[nodiscard]] bool HasVisibleRichTextContent(const RichText &text) {
	return !StringIsEmpty(text.text.text);
}

[[nodiscard]] bool ShouldSkipFinalRichTextContent(
		const RichText &text,
		const SerializeContext *context) {
	return IsFinalSerializeMode(context) && !HasVisibleRichTextContent(text);
}

[[nodiscard]] PhotoData *ResolvePhotoData(
		SerializeContext *context,
		uint64 id,
		PhotoData *photo) {
	return photo
		? photo
		: (id ? context->session->data().photo(id).get() : nullptr);
}

[[nodiscard]] DocumentData *ResolveDocumentData(
		SerializeContext *context,
		uint64 id,
		DocumentData *document) {
	return document
		? document
		: (id ? context->session->data().document(id).get() : nullptr);
}

[[nodiscard]] std::optional<MTPInputPhoto> ResolveInputPhoto(
		SerializeContext *context,
		uint64 id,
		PhotoData *photo) {
	const auto resolved = ResolvePhotoData(context, id, photo);
	if (!resolved) {
		return std::nullopt;
	}
	const auto input = resolved->mtpInput();
	return (input.type() == mtpc_inputPhoto
		&& input.c_inputPhoto().vid().v
		&& input.c_inputPhoto().vaccess_hash().v
		&& !resolved->fileReference().isEmpty())
		? std::make_optional(input)
		: std::nullopt;
}

[[nodiscard]] std::optional<MTPInputDocument> ResolveInputDocument(
		SerializeContext *context,
		uint64 id,
		DocumentData *document) {
	const auto resolved = ResolveDocumentData(context, id, document);
	if (!resolved) {
		return std::nullopt;
	}
	const auto input = resolved->mtpInput();
	return (resolved->hasRemoteLocation()
		&& input.type() == mtpc_inputDocument
		&& input.c_inputDocument().vid().v
		&& input.c_inputDocument().vaccess_hash().v
		&& !resolved->fileReference().isEmpty())
		? std::make_optional(input)
		: std::nullopt;
}

[[nodiscard]] std::optional<uint64> CollectPhoto(
		SerializeContext *context,
		uint64 id,
		PhotoData *photo) {
	if (const auto input = ResolveInputPhoto(context, id, photo)) {
		const auto serverId = uint64(input->c_inputPhoto().vid().v);
		context->photos.emplace(serverId, *input);
		return serverId;
	}
	return std::nullopt;
}

[[nodiscard]] std::optional<uint64> CollectDocument(
		SerializeContext *context,
		uint64 id,
		DocumentData *document = nullptr) {
	if (const auto input = ResolveInputDocument(context, id, document)) {
		const auto serverId = uint64(input->c_inputDocument().vid().v);
		context->documents.emplace(serverId, *input);
		return serverId;
	}
	return std::nullopt;
}

[[nodiscard]] std::optional<uint64> CollectMentionUser(
		SerializeContext *context,
		const QString &data) {
	const auto fields = TextUtilities::MentionNameDataToFields(data);
	if (!fields.userId || fields.selfId != context->session->userId().bare) {
		return std::nullopt;
	}
	if (context->users.find(fields.userId) != end(context->users)) {
		return fields.userId;
	}
	if (fields.userId == fields.selfId) {
		context->users.emplace(fields.userId, MTP_inputUserSelf());
		return fields.userId;
	}
	const auto user = context->session->data().user(UserId(fields.userId));
	if (user->isLoaded()) {
		context->users.emplace(fields.userId, user->inputUser());
		return fields.userId;
	}
	if (const auto item = user->owner().messageWithPeer(user->id)) {
		context->users.emplace(
			fields.userId,
			MTP_inputUserFromMessage(
				item->history()->peer->input(),
				MTP_int(int(item->id.bare)),
				MTP_long(fields.userId)));
		return fields.userId;
	}
	if (!fields.accessHash) {
		return std::nullopt;
	}
	context->users.emplace(
		fields.userId,
		MTP_inputUser(
			MTP_long(fields.userId),
			MTP_long(fields.accessHash)));
	return fields.userId;
}

[[nodiscard]] std::vector<EntityInText> SortedRichTextEntities(
		const TextWithEntities &text) {
	auto result = std::vector<EntityInText>();
	result.reserve(text.entities.size());
	const auto textLength = text.text.size();
	for (const auto &entity : text.entities) {
		const auto till = entity.offset() + entity.length();
		if (entity.offset() < 0
			|| entity.length() <= 0
			|| till > textLength) {
			continue;
		}
		result.push_back(entity);
	}
	std::sort(result.begin(), result.end(), [](const EntityInText &a, const EntityInText &b) {
		if (a.offset() != b.offset()) {
			return a.offset() < b.offset();
		}
		if (a.length() != b.length()) {
			return a.length() > b.length();
		}
		return EntitySerializationOrder(a.type())
			< EntitySerializationOrder(b.type());
	});
	return result;
}

[[nodiscard]] bool SkipEntityForRange(
		const std::vector<EntityInText> &entities,
		int index,
		int skipIndex) {
	if (index == skipIndex) {
		return true;
	} else if (skipIndex == kNoEntityIndex) {
		return false;
	}
	const auto &entity = entities[index];
	const auto &skip = entities[skipIndex];
	return (index < skipIndex)
		&& (entity.offset() == skip.offset())
		&& (entity.length() == skip.length());
}

[[nodiscard]] int FindOuterEntityAt(
		const std::vector<EntityInText> &entities,
		int position,
		int till,
		int skipIndex) {
	const auto count = int(entities.size());
	for (auto index = 0; index != count; ++index) {
		const auto &entity = entities[index];
		if (SkipEntityForRange(entities, index, skipIndex)) {
			continue;
		}
		if (entity.offset() == position
			&& entity.offset() + entity.length() <= till) {
			return index;
		}
		if (entity.offset() > position) {
			break;
		}
	}
	return kNoEntityIndex;
}

[[nodiscard]] std::optional<MTPRichText> SerializeRichTextRange(
		const QString &text,
		const std::vector<EntityInText> &entities,
		int from,
		int till,
		SerializeContext *context,
		int skipIndex);

[[nodiscard]] std::optional<MTPRichText> SerializeRichTextEntity(
		const QString &text,
		const std::vector<EntityInText> &entities,
		int entityIndex,
		SerializeContext *context) {
	const auto &entity = entities[entityIndex];
	const auto from = entity.offset();
	const auto length = entity.length();
	const auto segment = text.mid(from, length);
	const auto inner = SerializeRichTextRange(
		text,
		entities,
		from,
		from + length,
		context,
		entityIndex);
	if (!inner) {
		return std::nullopt;
	}
	switch (entity.type()) {
	case EntityType::Bold:
		return MTP_textBold(*inner);
	case EntityType::Italic:
		return MTP_textItalic(*inner);
	case EntityType::Underline:
		return MTP_textUnderline(*inner);
	case EntityType::StrikeOut:
		return MTP_textStrike(*inner);
	case EntityType::Code:
		return MTP_textFixed(*inner);
	case EntityType::Subscript:
		return MTP_textSubscript(*inner);
	case EntityType::Superscript:
		return MTP_textSuperscript(*inner);
	case EntityType::Marked:
		return MTP_textMarked(*inner);
	case EntityType::Spoiler:
		return MTP_textSpoiler(*inner);
	case EntityType::Mention:
		return MTP_textMention(*inner);
	case EntityType::Hashtag:
		return MTP_textHashtag(*inner);
	case EntityType::BotCommand:
		return MTP_textBotCommand(*inner);
	case EntityType::Cashtag:
		return MTP_textCashtag(*inner);
	case EntityType::Url:
		return MTP_textAutoUrl(*inner);
	case EntityType::Email:
		return MTP_textAutoEmail(*inner);
	case EntityType::Phone:
		return MTP_textAutoPhone(*inner);
	case EntityType::BankCard:
		return MTP_textBankCard(*inner);
	case EntityType::CustomUrl: {
		const auto data = entity.data();
		if (data.startsWith(u"mailto:"_q)) {
			return MTP_textEmail(*inner, MTP_string(data.mid(7)));
		} else if (data.startsWith(u"tel:"_q)) {
			return MTP_textPhone(*inner, MTP_string(data.mid(4)));
		}
		const auto decoded = DecodeRichPageLinkUrl(data);
		return MTP_textUrl(
			*inner,
			MTP_string(decoded ? decoded->url : data),
			MTP_long(0));
	}
	case EntityType::MentionName: {
		const auto userId = CollectMentionUser(context, entity.data());
		return userId
			? std::optional<MTPRichText>(MTP_textMentionName(
				*inner,
				MTP_long(*userId)))
			: std::optional<MTPRichText>(*inner);
	}
	case EntityType::CustomEmoji: {
		if (const auto parsed = Markdown::ParseInlineTextObjectEntity(
				entity.data())) {
			switch (parsed->kind) {
			case Markdown::InlineTextObjectKind::Formula: {
				const auto formula = std::get_if<
					Markdown::InlineTextObjectFormulaData>(&parsed->data);
				if (!formula) {
					return std::nullopt;
				}
				const auto source = !formula->trimmedTex.isEmpty()
					? formula->trimmedTex
					: FormulaTexFromSource(formula->copySource);
				return source.isEmpty()
					? std::optional<MTPRichText>(
						MakePlainRichText(segment))
					: std::optional<MTPRichText>(
						MTP_textMath(MTP_string(source)));
			}
			case Markdown::InlineTextObjectKind::IvImage: {
				const auto image = std::get_if<
					Markdown::InlineTextObjectIvImageData>(&parsed->data);
				return std::optional<MTPRichText>(MakePlainRichText(
					(image && !image->replacementText.isEmpty())
						? image->replacementText
						: u"[image]"_q));
			}
			}
		}
		const auto documentId = ::Data::ParseCustomEmojiData(entity.data());
		const auto collected = documentId
			? CollectDocument(context, documentId)
			: std::nullopt;
		return collected
			? std::optional<MTPRichText>(MTP_textCustomEmoji(
				MTP_long(*collected),
				MTP_string(segment.isEmpty() ? u"@"_q : segment)))
			: std::optional<MTPRichText>(MakePlainRichText(segment));
	}
	case EntityType::FormattedDate: {
		const auto [date, flags] = DeserializeFormattedDateData(entity.data());
		if (!date) {
			return *inner;
		}
		using Flag = MTPDtextDate::Flag;
		auto mtpFlags = MTPDtextDate::Flags();
		if (flags & FormattedDateFlag::Relative) {
			mtpFlags |= Flag::f_relative;
		}
		if (flags & FormattedDateFlag::ShortTime) {
			mtpFlags |= Flag::f_short_time;
		}
		if (flags & FormattedDateFlag::LongTime) {
			mtpFlags |= Flag::f_long_time;
		}
		if (flags & FormattedDateFlag::ShortDate) {
			mtpFlags |= Flag::f_short_date;
		}
		if (flags & FormattedDateFlag::LongDate) {
			mtpFlags |= Flag::f_long_date;
		}
		if (flags & FormattedDateFlag::DayOfWeek) {
			mtpFlags |= Flag::f_day_of_week;
		}
		return MTP_textDate(
			MTP_flags(mtpFlags),
			*inner,
			MTP_int(date));
	}
	case EntityType::Invalid:
	case EntityType::Semibold:
	case EntityType::MediaTimestamp:
	case EntityType::Colorized:
	case EntityType::Pre:
	case EntityType::Blockquote:
		break;
	}
	return *inner;
}

[[nodiscard]] std::optional<MTPRichText> SerializeRichTextRange(
		const QString &text,
		const std::vector<EntityInText> &entities,
		int from,
		int till,
		SerializeContext *context,
		int skipIndex) {
	auto parts = QVector<MTPRichText>();
	auto position = from;
	while (position < till) {
		auto nextEntityStart = till;
		const auto count = int(entities.size());
		for (auto index = 0; index != count; ++index) {
			const auto &entity = entities[index];
			if (SkipEntityForRange(entities, index, skipIndex)) {
				continue;
			}
			if (entity.offset() >= position
				&& entity.offset() + entity.length() <= till) {
				nextEntityStart = entity.offset();
				break;
			}
		}
		if (nextEntityStart > position) {
			parts.push_back(MakePlainRichText(
				text.mid(position, nextEntityStart - position)));
			position = nextEntityStart;
			continue;
		}
		const auto entity = FindOuterEntityAt(
			entities,
			position,
			till,
			skipIndex);
		if (entity == kNoEntityIndex) {
			parts.push_back(MakePlainRichText(text.mid(position, 1)));
			++position;
			continue;
		}
		const auto wrapped = SerializeRichTextEntity(
			text,
			entities,
			entity,
			context);
		if (!wrapped) {
			return std::nullopt;
		}
		parts.push_back(*wrapped);
		const auto &wrappedEntity = entities[entity];
		position = wrappedEntity.offset() + wrappedEntity.length();
	}
	return JoinRichTextParts(std::move(parts));
}

[[nodiscard]] std::optional<MTPRichText> SerializeRichTextWithAnchor(
		const RichText &text,
		const QString &anchorId,
		SerializeContext *context) {
	const auto entities = SortedRichTextEntities(text.text);
	auto result = SerializeRichTextRange(
		text.text.text,
		entities,
		0,
		text.text.text.size(),
		context,
		kNoEntityIndex);
	if (!result) {
		return std::nullopt;
	}
	*result = WrapRichTextAnchor(std::move(*result), text.anchorId);
	*result = WrapRichTextAnchor(std::move(*result), anchorId);
	return result;
}

[[nodiscard]] std::optional<MTPPageCaption> SerializeCaption(
		const RichText &caption,
		const QString &anchorId,
		SerializeContext *context) {
	const auto text = SerializeRichTextWithAnchor(caption, anchorId, context);
	return text
		? std::make_optional(MTP_pageCaption(*text, MTP_textEmpty()))
		: std::nullopt;
}

[[nodiscard]] std::optional<MTPPageBlock> SerializeGroupedMediaItem(
		const GroupedMediaItem &item,
		SerializeContext *context) {
	const auto caption = SerializeCaption(RichText(), QString(), context);
	if (!caption) {
		return std::nullopt;
	}
	switch (item.kind) {
	case BlockKind::Photo: {
		const auto photoId = CollectPhoto(context, item.photoId, item.photo);
		if (!photoId) {
			return std::nullopt;
		}
		using Flag = MTPDpageBlockPhoto::Flag;
		auto flags = MTPDpageBlockPhoto::Flags();
		if (item.spoiler) {
			flags |= Flag::f_spoiler;
		}
		return MTP_pageBlockPhoto(
			MTP_flags(flags),
			MTP_long(*photoId),
			*caption,
			MTPstring(),
			MTPlong());
	}
	case BlockKind::Video: {
		const auto documentId = CollectDocument(
			context,
			item.documentId,
			item.document);
		if (!documentId) {
			return std::nullopt;
		}
		using Flag = MTPDpageBlockVideo::Flag;
		auto flags = MTPDpageBlockVideo::Flags();
		if (item.autoplay) {
			flags |= Flag::f_autoplay;
		}
		if (item.loop) {
			flags |= Flag::f_loop;
		}
		if (item.spoiler) {
			flags |= Flag::f_spoiler;
		}
		return MTP_pageBlockVideo(
			MTP_flags(flags),
			MTP_long(*documentId),
			*caption);
	}
	default:
		return std::nullopt;
	}
}

[[nodiscard]] std::optional<QVector<MTPPageBlock>> SerializeGroupedMediaItems(
		const std::vector<GroupedMediaItem> &items,
		SerializeContext *context) {
	auto result = QVector<MTPPageBlock>();
	result.reserve(items.size());
	for (const auto &item : items) {
		const auto serialized = SerializeGroupedMediaItem(item, context);
		if (!serialized) {
			return std::nullopt;
		}
		result.push_back(*serialized);
	}
	return result;
}

[[nodiscard]] std::optional<QVector<MTPPageBlock>> SerializeBlocks(
		const std::vector<Block> &blocks,
		SerializeContext *context);

[[nodiscard]] std::optional<MTPPageBlock> SerializeParagraphBlock(
		const RichText &text,
		const QString &anchorId,
		SerializeContext *context) {
	const auto serialized = SerializeRichTextWithAnchor(text, anchorId, context);
	return serialized
		? std::make_optional(MTP_pageBlockParagraph(*serialized))
		: std::nullopt;
}

[[nodiscard]] bool AppendSerializedParagraphBlock(
		QVector<MTPPageBlock> *blocks,
		const RichText &text,
		const QString &anchorId,
		SerializeContext *context) {
	if (IsFinalSerializeMode(context) && !HasVisibleRichTextContent(text)) {
		return true;
	}
	const auto paragraph = SerializeParagraphBlock(text, anchorId, context);
	if (!paragraph) {
		return false;
	}
	blocks->push_back(*paragraph);
	return true;
}

[[nodiscard]] std::optional<MTPPageTableCell> SerializeTableCell(
		const TableCell &cell,
		SerializeContext *context) {
	using Flag = MTPDpageTableCell::Flag;
	auto flags = MTPDpageTableCell::Flags();
	if (cell.header) {
		flags |= Flag::f_header;
	}
	switch (cell.alignment) {
	case TableAlignment::Center:
		flags |= Flag::f_align_center;
		break;
	case TableAlignment::Right:
		flags |= Flag::f_align_right;
		break;
	case TableAlignment::Left:
		break;
	}
	switch (cell.verticalAlignment) {
	case TableVerticalAlignment::Middle:
		flags |= Flag::f_valign_middle;
		break;
	case TableVerticalAlignment::Bottom:
		flags |= Flag::f_valign_bottom;
		break;
	case TableVerticalAlignment::Top:
		break;
	}
	const auto colspan = std::max(cell.colspan, 1);
	const auto rowspan = std::max(cell.rowspan, 1);
	if (colspan != 1) {
		flags |= Flag::f_colspan;
	}
	if (rowspan != 1) {
		flags |= Flag::f_rowspan;
	}
	const auto hasText = HasRichTextContent(cell.text);
	auto text = MTPRichText(MTP_textEmpty());
	if (hasText) {
		flags |= Flag::f_text;
		const auto serialized = SerializeRichTextWithAnchor(
			cell.text,
			QString(),
			context);
		if (!serialized) {
			return std::nullopt;
		}
		text = *serialized;
	}
	return MTP_pageTableCell(
		MTP_flags(flags),
		std::move(text),
		(colspan != 1 ? MTP_int(colspan) : MTPint()),
		(rowspan != 1 ? MTP_int(rowspan) : MTPint()));
}

[[nodiscard]] SerializeBlockResult SerializeBlock(
		const Block &block,
		SerializeContext *context) {
	switch (block.kind) {
	case BlockKind::Heading: {
		if (ShouldSkipFinalRichTextContent(block.text, context)) {
			return SkippedSerializeBlock();
		}
		const auto text = SerializeRichTextWithAnchor(
			block.text,
			block.anchorId,
			context);
		if (!text) {
			return FailedSerializeBlock();
		}
		switch (std::clamp(block.headingLevel, 1, 6)) {
		case 1: return SuccessfulSerializeBlock(MTP_pageBlockHeading1(*text));
		case 2: return SuccessfulSerializeBlock(MTP_pageBlockHeading2(*text));
		case 3: return SuccessfulSerializeBlock(MTP_pageBlockHeading3(*text));
		case 4: return SuccessfulSerializeBlock(MTP_pageBlockHeading4(*text));
		case 5: return SuccessfulSerializeBlock(MTP_pageBlockHeading5(*text));
		case 6: return SuccessfulSerializeBlock(MTP_pageBlockHeading6(*text));
		}
		return FailedSerializeBlock();
	}
	case BlockKind::Paragraph:
		if (IsFinalSerializeMode(context)
			&& !HasVisibleRichTextContent(block.text)) {
			return SkippedSerializeBlock();
		}
		return FinishSerializeBlock(SerializeParagraphBlock(
			block.text,
			block.anchorId,
			context));
	case BlockKind::Footer: {
		if (ShouldSkipFinalRichTextContent(block.text, context)) {
			return SkippedSerializeBlock();
		}
		const auto text = SerializeRichTextWithAnchor(
			block.text,
			block.anchorId,
			context);
		return text
			? SuccessfulSerializeBlock(MTP_pageBlockFooter(*text))
			: FailedSerializeBlock();
	}
	case BlockKind::Divider:
		return SuccessfulSerializeBlock(MTP_pageBlockDivider());
	case BlockKind::Anchor:
		return block.anchorId.isEmpty()
			? FailedSerializeBlock()
			: SuccessfulSerializeBlock(MTP_pageBlockAnchor(
				MTP_string(block.anchorId)));
	case BlockKind::Quote: {
		if (block.pullquote) {
			if (!block.blocks.empty()) {
				return FailedSerializeBlock();
			}
			if (IsFinalSerializeMode(context)
				&& !HasVisibleRichTextContent(block.text)) {
				return SkippedSerializeBlock();
			}
			const auto caption = SerializeRichTextWithAnchor(
				block.caption,
				QString(),
				context);
			const auto text = SerializeRichTextWithAnchor(
				block.text,
				block.anchorId,
				context);
			return (text && caption)
				? SuccessfulSerializeBlock(MTP_pageBlockPullquote(
					*text,
					*caption))
				: FailedSerializeBlock();
		}
		if (block.blocks.empty()) {
			if (IsFinalSerializeMode(context)
				&& !HasVisibleRichTextContent(block.text)) {
				return SkippedSerializeBlock();
			}
			const auto caption = SerializeRichTextWithAnchor(
				block.caption,
				QString(),
				context);
			const auto text = SerializeRichTextWithAnchor(
				block.text,
				block.anchorId,
				context);
			return (text && caption)
				? SuccessfulSerializeBlock(MTP_pageBlockBlockquote(
					*text,
					*caption))
				: FailedSerializeBlock();
		}
		auto blocks = QVector<MTPPageBlock>();
		if ((HasRichTextContent(block.text) || IsFinalSerializeMode(context))
			&& !AppendSerializedParagraphBlock(
				&blocks,
				block.text,
				QString(),
				context)) {
			return FailedSerializeBlock();
		}
		const auto nested = SerializeBlocks(block.blocks, context);
		if (!nested) {
			return FailedSerializeBlock();
		}
		blocks += *nested;
		if (IsFinalSerializeMode(context) && blocks.isEmpty()) {
			return SkippedSerializeBlock();
		}
		const auto caption = SerializeRichTextWithAnchor(
			block.caption,
			block.anchorId,
			context);
		return caption
			? SuccessfulSerializeBlock(MTP_pageBlockBlockquoteBlocks(
				MTP_vector<MTPPageBlock>(std::move(blocks)),
				*caption))
			: FailedSerializeBlock();
	}
	case BlockKind::List: {
		if (block.listKind == ListKind::Ordered) {
			using Flag = MTPDpageBlockOrderedList::Flag;
			auto flags = MTPDpageBlockOrderedList::Flags();
			if (block.orderedList.reversed) {
				flags |= Flag::f_reversed;
			}
			if (block.orderedList.start.has_value()) {
				flags |= Flag::f_start;
			}
			if (block.orderedList.type.has_value()) {
				flags |= Flag::f_type;
			}
			auto items = QVector<MTPPageListOrderedItem>();
			items.reserve(block.listItems.size());
			for (const auto &item : block.listItems) {
				if (!item.blocks.empty()) {
					using ItemFlag = MTPDpageListOrderedItemBlocks::Flag;
					auto itemFlags = MTPDpageListOrderedItemBlocks::Flags();
					if (item.taskState != TaskState::None) {
						itemFlags |= ItemFlag::f_checkbox;
					}
					if (item.taskState == TaskState::Checked) {
						itemFlags |= ItemFlag::f_checked;
					}
					if (item.number.num.has_value()) {
						itemFlags |= ItemFlag::f_num;
					}
					if (item.number.value.has_value()) {
						itemFlags |= ItemFlag::f_value;
					}
					if (item.number.type.has_value()) {
						itemFlags |= ItemFlag::f_type;
					}
					auto blocks = QVector<MTPPageBlock>();
					if ((HasRichTextContent(item.text)
							|| !item.anchorId.isEmpty()
							|| IsFinalSerializeMode(context))
						&& !AppendSerializedParagraphBlock(
							&blocks,
							item.text,
							item.anchorId,
							context)) {
						return FailedSerializeBlock();
					}
					const auto nested = SerializeBlocks(item.blocks, context);
					if (!nested) {
						return FailedSerializeBlock();
					}
					blocks += *nested;
					if (IsFinalSerializeMode(context) && blocks.isEmpty()) {
						continue;
					}
					items.push_back(MTP_pageListOrderedItemBlocks(
						MTP_flags(itemFlags),
						item.number.num.has_value()
							? MTP_string(*item.number.num)
							: MTPstring(),
						MTP_vector<MTPPageBlock>(std::move(blocks)),
						item.number.value.has_value()
							? MTP_int(*item.number.value)
							: MTPint(),
						item.number.type.has_value()
							? MTP_string(*item.number.type)
							: MTPstring()));
				} else {
					using ItemFlag = MTPDpageListOrderedItemText::Flag;
					auto itemFlags = MTPDpageListOrderedItemText::Flags();
					if (item.taskState != TaskState::None) {
						itemFlags |= ItemFlag::f_checkbox;
					}
					if (item.taskState == TaskState::Checked) {
						itemFlags |= ItemFlag::f_checked;
					}
					if (item.number.num.has_value()) {
						itemFlags |= ItemFlag::f_num;
					}
					if (item.number.value.has_value()) {
						itemFlags |= ItemFlag::f_value;
					}
					if (item.number.type.has_value()) {
						itemFlags |= ItemFlag::f_type;
					}
					if (IsFinalSerializeMode(context)
						&& !HasVisibleRichTextContent(item.text)) {
						continue;
					}
					const auto text = SerializeRichTextWithAnchor(
						item.text,
						item.anchorId,
						context);
					if (!text) {
						return FailedSerializeBlock();
					}
					items.push_back(MTP_pageListOrderedItemText(
						MTP_flags(itemFlags),
						item.number.num.has_value()
							? MTP_string(*item.number.num)
							: MTPstring(),
						*text,
						item.number.value.has_value()
							? MTP_int(*item.number.value)
							: MTPint(),
						item.number.type.has_value()
							? MTP_string(*item.number.type)
							: MTPstring()));
				}
			}
			if (IsFinalSerializeMode(context) && items.isEmpty()) {
				return SkippedSerializeBlock();
			}
			return SuccessfulSerializeBlock(MTP_pageBlockOrderedList(
				MTP_flags(flags),
				MTP_vector<MTPPageListOrderedItem>(std::move(items)),
				block.orderedList.start.has_value()
					? MTP_int(*block.orderedList.start)
					: MTPint(),
				block.orderedList.type.has_value()
					? MTP_string(*block.orderedList.type)
					: MTPstring()));
		}
		auto items = QVector<MTPPageListItem>();
		items.reserve(block.listItems.size());
		for (const auto &item : block.listItems) {
			if (!item.blocks.empty()) {
				using Flag = MTPDpageListItemBlocks::Flag;
				auto flags = MTPDpageListItemBlocks::Flags();
				if (item.taskState != TaskState::None) {
					flags |= Flag::f_checkbox;
				}
				if (item.taskState == TaskState::Checked) {
					flags |= Flag::f_checked;
				}
				auto blocks = QVector<MTPPageBlock>();
				if ((HasRichTextContent(item.text)
						|| !item.anchorId.isEmpty()
						|| IsFinalSerializeMode(context))
					&& !AppendSerializedParagraphBlock(
						&blocks,
						item.text,
						item.anchorId,
						context)) {
					return FailedSerializeBlock();
				}
				const auto nested = SerializeBlocks(item.blocks, context);
				if (!nested) {
					return FailedSerializeBlock();
				}
				blocks += *nested;
				if (IsFinalSerializeMode(context) && blocks.isEmpty()) {
					continue;
				}
				items.push_back(MTP_pageListItemBlocks(
					MTP_flags(flags),
					MTP_vector<MTPPageBlock>(std::move(blocks))));
			} else {
				using Flag = MTPDpageListItemText::Flag;
				auto flags = MTPDpageListItemText::Flags();
				if (item.taskState != TaskState::None) {
					flags |= Flag::f_checkbox;
				}
				if (item.taskState == TaskState::Checked) {
					flags |= Flag::f_checked;
				}
				if (IsFinalSerializeMode(context)
					&& !HasVisibleRichTextContent(item.text)) {
					continue;
				}
				const auto text = SerializeRichTextWithAnchor(
					item.text,
					item.anchorId,
					context);
				if (!text) {
					return FailedSerializeBlock();
				}
				items.push_back(MTP_pageListItemText(MTP_flags(flags), *text));
			}
		}
		if (IsFinalSerializeMode(context) && items.isEmpty()) {
			return SkippedSerializeBlock();
		}
		return SuccessfulSerializeBlock(MTP_pageBlockList(
			MTP_vector<MTPPageListItem>(std::move(items))));
	}
	case BlockKind::Photo: {
		const auto photoId = CollectPhoto(context, block.photoId, block.photo);
		const auto caption = SerializeCaption(block.caption, block.anchorId, context);
		if (!photoId || !caption) {
			return FailedSerializeBlock();
		}
		using Flag = MTPDpageBlockPhoto::Flag;
		auto flags = MTPDpageBlockPhoto::Flags();
		if (block.spoiler) {
			flags |= Flag::f_spoiler;
		}
		return SuccessfulSerializeBlock(MTP_pageBlockPhoto(
			MTP_flags(flags),
			MTP_long(*photoId),
			*caption,
			MTPstring(),
			MTPlong()));
	}
	case BlockKind::Video: {
		const auto documentId = CollectDocument(
			context,
			block.documentId,
			block.document);
		const auto caption = SerializeCaption(block.caption, block.anchorId, context);
		if (!documentId || !caption) {
			return FailedSerializeBlock();
		}
		using Flag = MTPDpageBlockVideo::Flag;
		auto flags = MTPDpageBlockVideo::Flags();
		if (block.autoplay) {
			flags |= Flag::f_autoplay;
		}
		if (block.loop) {
			flags |= Flag::f_loop;
		}
		if (block.spoiler) {
			flags |= Flag::f_spoiler;
		}
		return SuccessfulSerializeBlock(MTP_pageBlockVideo(
			MTP_flags(flags),
			MTP_long(*documentId),
			*caption));
	}
	case BlockKind::Audio: {
		const auto documentId = CollectDocument(
			context,
			block.documentId,
			block.document);
		const auto caption = SerializeCaption(block.caption, block.anchorId, context);
		return (documentId && caption)
			? SuccessfulSerializeBlock(MTP_pageBlockAudio(
				MTP_long(*documentId),
				*caption))
			: FailedSerializeBlock();
	}
	case BlockKind::Math:
		if (IsFinalSerializeMode(context) && StringIsEmpty(block.formula)) {
			return SkippedSerializeBlock();
		}
		return SuccessfulSerializeBlock(MTP_pageBlockMath(
			MTP_string(block.formula)));
	case BlockKind::Table: {
		using Flag = MTPDpageBlockTable::Flag;
		auto flags = MTPDpageBlockTable::Flags();
		if (block.bordered) {
			flags |= Flag::f_bordered;
		}
		if (block.striped) {
			flags |= Flag::f_striped;
		}
		auto rows = QVector<MTPPageTableRow>();
		rows.reserve(block.tableRows.size());
		for (const auto &row : block.tableRows) {
			auto cells = QVector<MTPPageTableCell>();
			cells.reserve(row.cells.size());
			for (const auto &cell : row.cells) {
				const auto serialized = SerializeTableCell(cell, context);
				if (!serialized) {
					return FailedSerializeBlock();
				}
				cells.push_back(*serialized);
			}
			if (IsFinalSerializeMode(context) && cells.isEmpty()) {
				continue;
			}
			rows.push_back(MTP_pageTableRow(
				MTP_vector<MTPPageTableCell>(std::move(cells))));
		}
		if (IsFinalSerializeMode(context) && rows.isEmpty()) {
			return SkippedSerializeBlock();
		}
		const auto title = SerializeRichTextWithAnchor(
			block.text,
			block.anchorId,
			context);
		if (!title) {
			return FailedSerializeBlock();
		}
		return SuccessfulSerializeBlock(MTP_pageBlockTable(
			MTP_flags(flags),
			*title,
			MTP_vector<MTPPageTableRow>(std::move(rows))));
	}
	case BlockKind::Details: {
		using Flag = MTPDpageBlockDetails::Flag;
		auto flags = block.open ? Flag::f_open : Flag();
		const auto blocks = SerializeBlocks(block.blocks, context);
		if (IsFinalSerializeMode(context) && blocks && blocks->isEmpty()) {
			return SkippedSerializeBlock();
		}
		const auto title = SerializeRichTextWithAnchor(
			block.text,
			block.anchorId,
			context);
		return (title && blocks)
			? SuccessfulSerializeBlock(MTP_pageBlockDetails(
				MTP_flags(flags),
				MTP_vector<MTPPageBlock>(*blocks),
				*title))
			: FailedSerializeBlock();
	}
	case BlockKind::Map: {
		const auto width = (block.width > 0)
			? block.width
			: kDefaultMapWidth;
		const auto height = (block.height > 0)
			? block.height
			: kDefaultMapHeight;
		if (block.zoom <= 0) {
			return FailedSerializeBlock();
		}
		const auto caption = SerializeCaption(block.caption, block.anchorId, context);
		return caption
			? SuccessfulSerializeBlock(MTP_inputPageBlockMap(
				MTP_inputGeoPoint(
					MTP_flags(0),
					MTP_double(block.latitude),
					MTP_double(block.longitude),
					MTPint()),
				MTP_int(block.zoom),
				MTP_int(width),
				MTP_int(height),
				*caption))
			: FailedSerializeBlock();
	}
	case BlockKind::Code: {
		if (!block.blocks.empty()) {
			return FailedSerializeBlock();
		}
		if (ShouldSkipFinalRichTextContent(block.text, context)) {
			return SkippedSerializeBlock();
		}
		const auto text = SerializeRichTextWithAnchor(
			block.text,
			block.anchorId,
			context);
		return text
			? SuccessfulSerializeBlock(MTP_pageBlockPreformatted(
				*text,
				MTP_string(block.language)))
			: FailedSerializeBlock();
	}
	case BlockKind::GroupedMedia: {
		const auto items = SerializeGroupedMediaItems(
			block.mediaItems,
			context);
		const auto caption = SerializeCaption(
			block.caption,
			block.anchorId,
			context);
		if (!items || items->isEmpty() || !caption) {
			return FailedSerializeBlock();
		}
		if (block.mediaIntent == RichPage::GroupedMediaIntent::Slideshow) {
			return SuccessfulSerializeBlock(MTP_pageBlockSlideshow(
				MTP_vector<MTPPageBlock>(std::move(*items)),
				*caption));
		}
		return SuccessfulSerializeBlock(MTP_pageBlockCollage(
			MTP_vector<MTPPageBlock>(std::move(*items)),
			*caption));
	}
	case BlockKind::Unsupported:
	case BlockKind::Thinking:
	case BlockKind::AuthorDate:
	case BlockKind::Embed:
	case BlockKind::EmbedPost:
	case BlockKind::Channel:
	case BlockKind::RelatedArticles:
		break;
	}
	return FailedSerializeBlock();
}

[[nodiscard]] std::optional<QVector<MTPPageBlock>> SerializeBlocks(
		const std::vector<Block> &blocks,
		SerializeContext *context) {
	auto result = QVector<MTPPageBlock>();
	result.reserve(blocks.size());
	for (const auto &block : blocks) {
		const auto serialized = SerializeBlock(block, context);
		switch (serialized.state) {
		case SerializeBlockState::Success:
			result.push_back(*serialized.block);
			break;
		case SerializeBlockState::Skipped:
			break;
		case SerializeBlockState::Failed:
			return std::nullopt;
		}
	}
	return result;
}

[[nodiscard]] SerializeInputRichMessageResult FailedSerializeInputRichMessage() {
	return {};
}

[[nodiscard]] SerializeInputRichMessageResult EmptySerializeInputRichMessage() {
	auto result = SerializeInputRichMessageResult();
	result.status = SerializeInputRichMessageStatus::EmptyContent;
	return result;
}

[[nodiscard]] SerializeInputRichMessageResult SuccessfulSerializeInputRichMessage(
		MTPInputRichMessage value) {
	auto result = SerializeInputRichMessageResult();
	result.status = SerializeInputRichMessageStatus::Success;
	result.value = std::move(value);
	return result;
}

} // namespace

SerializeInputRichMessageResult SerializeInputRichMessage(
		not_null<Main::Session*> session,
		const RichPage &page,
		SerializeInputRichMessageMode mode) {
	auto context = SerializeContext{ session };
	context.mode = mode;
	auto blocks = SerializeBlocks(page.blocks, &context);
	if (!blocks) {
		return FailedSerializeInputRichMessage();
	}
	if (mode == SerializeInputRichMessageMode::FinalSubmit
		&& blocks->isEmpty()) {
		return EmptySerializeInputRichMessage();
	}
	auto photos = QVector<MTPInputPhoto>();
	photos.reserve(context.photos.size());
	for (const auto &[id, input] : context.photos) {
		photos.push_back(input);
	}
	auto documents = QVector<MTPInputDocument>();
	documents.reserve(context.documents.size());
	for (const auto &[id, input] : context.documents) {
		documents.push_back(input);
	}
	auto users = QVector<MTPInputUser>();
	users.reserve(context.users.size());
	for (const auto &[id, input] : context.users) {
		users.push_back(input);
	}
	using Flag = MTPDinputRichMessage::Flag;
	auto flags = MTPDinputRichMessage::Flags();
	if (page.rtl) {
		flags |= Flag::f_rtl;
	}
	if (!photos.isEmpty()) {
		flags |= Flag::f_photos;
	}
	if (!documents.isEmpty()) {
		flags |= Flag::f_documents;
	}
	if (!users.isEmpty()) {
		flags |= Flag::f_users;
	}
	return SuccessfulSerializeInputRichMessage(MTP_inputRichMessage(
		MTP_flags(flags),
		MTP_vector<MTPPageBlock>(std::move(*blocks)),
		MTP_vector<MTPInputPhoto>(std::move(photos)),
		MTP_vector<MTPInputDocument>(std::move(documents)),
		MTP_vector<MTPInputUser>(std::move(users))));
}

} // namespace Iv
