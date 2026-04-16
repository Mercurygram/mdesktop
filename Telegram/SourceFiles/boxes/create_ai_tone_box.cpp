/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "boxes/create_ai_tone_box.h"

#include "base/random.h"
#include "chat_helpers/compose/compose_show.h"
#include "chat_helpers/emoji_list_widget.h"
#include "chat_helpers/stickers_lottie.h"
#include "data/data_ai_compose_tones.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_forum_icons.h"
#include "data/data_session.h"
#include "data/stickers/data_custom_emoji.h"
#include "history/view/media/history_view_sticker_player.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "ui/abstract_button.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/vertical_list.h"
#include "ui/widgets/shadow.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/wrap/padding_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"

#include "styles/style_boxes.h"
#include "styles/style_chat_helpers.h"
#include "styles/style_dialogs.h"
#include "styles/style_layers.h"

namespace {

void ChooseToneIconBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		Fn<void(DocumentId)> chosen) {
	using namespace ChatHelpers;

	box->setTitle(tr::lng_ai_compose_tone_icon_title());
	box->setWidth(st::boxWideWidth);
	box->setMaxHeight(st::editTopicMaxHeight);
	box->setScrollStyle(st::reactPanelScroll);

	const auto manager = &controller->session().data().customEmojiManager();
	const auto icons = &controller->session().data().forumIcons();

	auto factory = [=](DocumentId id, Fn<void()> repaint)
	-> std::unique_ptr<Ui::Text::CustomEmoji> {
		return manager->create(
			id,
			std::move(repaint),
			Data::CustomEmojiManager::SizeTag::Large);
	};

	const auto top = box->setPinnedToTopContent(
		object_ptr<Ui::VerticalLayout>(box));

	const auto body = box->verticalLayout();
	const auto selector = body->add(
		object_ptr<EmojiListWidget>(body, EmojiListDescriptor{
			.show = controller->uiShow(),
			.mode = EmojiListWidget::Mode::TopicIcon,
			.paused = Window::PausedIn(
				controller,
				Window::GifPauseReason::Layer),
			.customRecentList = DocumentListToRecent(icons->list()),
			.customRecentFactory = std::move(factory),
			.st = &st::reactPanelEmojiPan,
		}),
		st::reactPanelEmojiPan.padding);

	icons->requestDefaultIfUnknown();
	icons->defaultUpdates(
	) | rpl::on_next([=] {
		selector->provideRecent(DocumentListToRecent(icons->list()));
	}, selector->lifetime());

	top->add(selector->createFooter());

	const auto shadow = Ui::CreateChild<Ui::PlainShadow>(box.get());
	shadow->show();

	rpl::combine(
		top->heightValue(),
		selector->widthValue()
	) | rpl::on_next([=](int topHeight, int width) {
		shadow->setGeometry(0, topHeight, width, st::lineWidth);
	}, shadow->lifetime());

	selector->refreshEmoji();

	selector->scrollToRequests(
	) | rpl::on_next([=](int y) {
		box->scrollToY(y);
		shadow->update();
	}, selector->lifetime());

	rpl::combine(
		box->heightValue(),
		top->heightValue(),
		rpl::mappers::_1 - rpl::mappers::_2
	) | rpl::on_next([=](int height) {
		selector->setMinimalHeight(selector->width(), height);
	}, body->lifetime());

	selector->customChosen(
	) | rpl::on_next([=](ChatHelpers::FileChosen data) {
		chosen(data.document->id);
		box->closeBox();
	}, selector->lifetime());

	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

void AddIconPreview(
		not_null<Ui::VerticalLayout*> container,
		not_null<Main::Session*> session,
		rpl::producer<DocumentId> emojiIdValue,
		Fn<void(DocumentId)> emojiIdChosen) {
	using StickerPlayer = HistoryView::StickerPlayer;
	struct State {
		DocumentId emojiId = 0;
		std::shared_ptr<StickerPlayer> player;
		bool playerUsesTextColor = false;
	};

	const auto outer = st::aiToneIconPreviewSize;
	const auto inner = st::aiToneIconPreviewInnerSize;
	const auto top = st::aiToneIconPreviewTopSkip;
	const auto bottom = st::aiToneIconPreviewBottomSkip;
	const auto holder = container->add(
		object_ptr<Ui::FixedHeightWidget>(
			container,
			outer + top + bottom));
	const auto button = Ui::CreateChild<Ui::AbstractButton>(holder);
	button->resize(outer, outer);
	button->show();

	holder->widthValue(
	) | rpl::on_next([=](int width) {
		button->move((width - outer) / 2, top);
	}, button->lifetime());

	const auto state = button->lifetime().make_state<State>();
	const auto emojiIdVar = button->lifetime().make_state<
		rpl::variable<DocumentId>>(std::move(emojiIdValue));

	emojiIdVar->value(
	) | rpl::on_next([=](DocumentId id) {
		state->emojiId = id;
	}, button->lifetime());

	const auto icons = &session->data().forumIcons();
	icons->requestDefaultIfUnknown();
	const auto seedRandom = [=] {
		if (state->emojiId) {
			return;
		}
		const auto &list = icons->list();
		if (list.empty()) {
			return;
		}
		emojiIdChosen(list[base::RandomIndex(list.size())]);
	};
	seedRandom();
	icons->defaultUpdates(
	) | rpl::on_next(seedRandom, button->lifetime());

	emojiIdVar->value(
	) | rpl::map([=](DocumentId id) -> rpl::producer<DocumentData*> {
		if (!id) {
			return rpl::single((DocumentData*)nullptr);
		}
		return session->data().customEmojiManager().resolve(
			id
		) | rpl::map([=](not_null<DocumentData*> document) {
			return document.get();
		}) | rpl::map_error_to_done();
	}) | rpl::flatten_latest(
	) | rpl::map([=](DocumentData *document)
	-> rpl::producer<std::shared_ptr<StickerPlayer>> {
		if (!document) {
			return rpl::single(std::shared_ptr<StickerPlayer>());
		}
		const auto media = document->createMediaView();
		media->checkStickerLarge();
		media->goodThumbnailWanted();

		return rpl::single() | rpl::then(
			document->session().downloaderTaskFinished()
		) | rpl::filter([=] {
			return media->loaded();
		}) | rpl::take(1) | rpl::map([=] {
			auto result = std::shared_ptr<StickerPlayer>();
			const auto sticker = document->sticker();
			const auto size = QSize(inner, inner);
			if (sticker && sticker->isLottie()) {
				result = std::make_shared<HistoryView::LottiePlayer>(
					ChatHelpers::LottiePlayerFromDocument(
						media.get(),
						ChatHelpers::StickerLottieSize::StickerSet,
						size,
						Lottie::Quality::High));
			} else if (sticker && sticker->isWebm()) {
				result = std::make_shared<HistoryView::WebmPlayer>(
					media->owner()->location(),
					media->bytes(),
					size);
			} else {
				result = std::make_shared<HistoryView::StaticStickerPlayer>(
					media->owner()->location(),
					media->bytes(),
					size);
			}
			result->setRepaintCallback([=] { button->update(); });
			state->playerUsesTextColor
				= media->owner()->emojiUsesTextColor();
			return result;
		});
	}) | rpl::flatten_latest(
	) | rpl::on_next([=](std::shared_ptr<StickerPlayer> player) {
		state->player = std::move(player);
		button->update();
	}, button->lifetime());

	button->paintRequest(
	) | rpl::on_next([=] {
		auto p = QPainter(button);
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::aiToneIconPreviewBg);
		p.drawEllipse(button->rect());
		if (state->player && state->player->ready()) {
			const auto color = state->playerUsesTextColor
				? st::windowFg->c
				: QColor(0, 0, 0, 0);
			const auto frame = state->player->frame(
				QSize(inner, inner),
				color,
				false,
				crl::now(),
				false).image;
			const auto sz = frame.size() / style::DevicePixelRatio();
			p.drawImage(
				QRect(
					(outer - sz.width()) / 2,
					(outer - sz.height()) / 2,
					sz.width(),
					sz.height()),
				frame);
			state->player->markFrameShown();
		}
	}, button->lifetime());

	button->setClickedCallback([=] {
		const auto controller = ChatHelpers::ResolveWindowDefault()(
			session);
		if (!controller) {
			return;
		}
		controller->uiShow()->showBox(Box(
			ChooseToneIconBox,
			controller,
			crl::guard(button, [=](DocumentId id) {
				emojiIdChosen(id);
			})));
	});
}

void SetupToneBox(
		not_null<Ui::GenericBox*> box,
		not_null<Main::Session*> session,
		DocumentId initialEmojiId,
		const QString &initialName,
		const QString &initialPrompt,
		rpl::producer<QString> title,
		Fn<void(DocumentId, QString, QString)> submit) {
	box->setTitle(std::move(title));

	const auto container = box->verticalLayout();
	const auto emojiId = container->lifetime().make_state<
		rpl::variable<DocumentId>>(initialEmojiId);

	AddIconPreview(
		container,
		session,
		emojiId->value(),
		[=](DocumentId id) { *emojiId = id; });

	const auto name = box->addRow(object_ptr<Ui::InputField>(
		box,
		st::defaultInputField,
		tr::lng_ai_compose_tone_name(),
		initialName));

	Ui::AddSkip(box->verticalLayout());

	const auto promptSt = box->lifetime().make_state<style::InputField>(
		st::newGroupDescription);
	const auto prompt = box->addRow(object_ptr<Ui::InputField>(
		box,
		*promptSt,
		Ui::InputField::Mode::MultiLine,
		tr::lng_ai_compose_tone_prompt(),
		initialPrompt));

	box->setFocusCallback([=] {
		name->setFocusFast();
	});

	const auto save = [=] {
		const auto nameText = name->getLastText().trimmed();
		const auto promptText = prompt->getLastText().trimmed();
		if (nameText.isEmpty() || promptText.isEmpty()) {
			if (nameText.isEmpty()) {
				name->showError();
			}
			if (promptText.isEmpty()) {
				prompt->showError();
			}
			return;
		}
		submit(emojiId->current(), nameText, promptText);
	};

	box->addButton(tr::lng_ai_compose_tone_save(), save);
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

} // namespace

void CreateAiToneBox(
		not_null<Ui::GenericBox*> box,
		not_null<Main::Session*> session,
		Fn<void()> saved) {
	SetupToneBox(
		box,
		session,
		DocumentId(0),
		QString(),
		QString(),
		tr::lng_ai_compose_create_tone_title(),
		[=](DocumentId emojiId,
				const QString &name,
				const QString &prompt) {
			session->data().aiComposeTones().create(
				name,
				prompt,
				emojiId,
				false,
				[=](Data::AiComposeTone) {
					box->closeBox();
					if (saved) {
						saved();
					}
				});
		});
}

void EditAiToneBox(
		not_null<Ui::GenericBox*> box,
		not_null<Main::Session*> session,
		const Data::AiComposeTone &tone,
		Fn<void()> saved) {
	const auto toneId = tone.id;
	const auto toneAccessHash = tone.accessHash;
	SetupToneBox(
		box,
		session,
		tone.emojiId,
		tone.title,
		tone.prompt,
		tr::lng_ai_compose_edit_tone_title(),
		[=](DocumentId emojiId,
				const QString &name,
				const QString &prompt) {
			auto toneCopy = Data::AiComposeTone();
			toneCopy.id = toneId;
			toneCopy.accessHash = toneAccessHash;
			session->data().aiComposeTones().update(
				toneCopy,
				name,
				prompt,
				std::make_optional(emojiId),
				std::nullopt,
				[=](Data::AiComposeTone) {
					box->closeBox();
					if (saved) {
						saved();
					}
				});
		});
}
