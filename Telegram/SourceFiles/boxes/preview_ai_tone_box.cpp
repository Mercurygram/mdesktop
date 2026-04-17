/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "boxes/preview_ai_tone_box.h"

#include "boxes/create_ai_tone_box.h"
#include "core/ui_integration.h"
#include "data/data_ai_compose_tones.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "ui/controls/custom_emoji_toast_icon.h"
#include "ui/effects/skeleton_animation.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/painter.h"
#include "ui/text/text_entity.h"
#include "ui/text/text_utilities.h"
#include "ui/toast/toast.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/shadow.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/vertical_list.h"
#include "styles/style_boxes.h"
#include "styles/style_chat_helpers.h"
#include "styles/style_layers.h"

namespace {

constexpr auto kToastDuration = crl::time(4000);

class PreviewAiToneExampleCard final : public Ui::RpWidget {
public:
	explicit PreviewAiToneExampleCard(QWidget *parent);

	void showExample(Data::AiComposeToneExample example);
	void showSkeleton(bool shown);
	[[nodiscard]] rpl::producer<> anotherExampleRequested() const;

protected:
	int resizeGetHeight(int newWidth) override;
	void paintEvent(QPaintEvent *e) override;

private:
	const not_null<Ui::VerticalLayout*> _layout;
	const not_null<Ui::FlatLabel*> _beforeTitle;
	const not_null<Ui::FlatLabel*> _beforeBody;
	const style::complex_color _shadowColor;
	const not_null<Ui::PlainShadow*> _shadow;
	const not_null<Ui::FlatLabel*> _afterTitle;
	const not_null<Ui::FlatLabel*> _afterBody;
	const not_null<Ui::RoundButton*> _another;
	Ui::SkeletonAnimation _beforeSkeleton;
	Ui::SkeletonAnimation _afterSkeleton;
	rpl::event_stream<> _anotherExampleRequested;

};

PreviewAiToneExampleCard::PreviewAiToneExampleCard(QWidget *parent)
: RpWidget(parent)
, _layout(Ui::CreateChild<Ui::VerticalLayout>(this))
, _beforeTitle(_layout->add(
	object_ptr<Ui::FlatLabel>(
		_layout,
		tr::lng_ai_compose_before(tr::now),
		st::aiComposeCardTitle),
	QMargins(
		st::aiTonePreviewExampleCardPadding.left(),
		st::aiTonePreviewExampleCardPadding.top(),
		st::aiTonePreviewExampleCardPadding.right(),
		0)))
, _beforeBody(_layout->add(
	object_ptr<Ui::FlatLabel>(_layout, st::aiComposeBodyLabel),
	QMargins(
		st::aiTonePreviewExampleCardPadding.left(),
		st::aiTonePreviewExampleCardTitleSkip,
		st::aiTonePreviewExampleCardPadding.right(),
		0)))
, _shadowColor([] {
	auto color = st::windowSubTextFg->c;
	color.setAlphaF(st::aiComposeShadowOpacity);
	return color;
})
, _shadow(_layout->add(
	object_ptr<Ui::PlainShadow>(_layout, _shadowColor.color()),
	QMargins(
		st::aiTonePreviewExampleCardPadding.left(),
		st::aiTonePreviewExampleCardSectionSkip / 2,
		st::aiTonePreviewExampleCardPadding.right(),
		0)))
, _afterTitle(_layout->add(
	object_ptr<Ui::FlatLabel>(
		_layout,
		tr::lng_ai_compose_after(tr::now),
		st::aiComposeCardTitle),
	QMargins(
		st::aiTonePreviewExampleCardPadding.left(),
		st::aiTonePreviewExampleCardSectionSkip / 2,
		st::aiTonePreviewExampleCardPadding.right(),
		0)))
, _afterBody(_layout->add(
	object_ptr<Ui::FlatLabel>(_layout, st::aiComposeBodyLabel),
	QMargins(
		st::aiTonePreviewExampleCardPadding.left(),
		st::aiTonePreviewExampleCardTitleSkip,
		st::aiTonePreviewExampleCardPadding.right(),
		st::aiTonePreviewExampleCardPadding.bottom())))
, _another(Ui::CreateChild<Ui::RoundButton>(
	this,
	tr::lng_ai_compose_tone_preview_add_example(),
	st::aiTonePreviewAnotherExampleButton))
, _beforeSkeleton(_beforeBody)
, _afterSkeleton(_afterBody) {
	_beforeBody->setSelectable(true);
	_afterBody->setSelectable(true);
	_another->raise();
	rpl::combine(
		widthValue(),
		_beforeTitle->geometryValue(),
		_another->widthValue()
	) | rpl::on_next([=](int width, QRect titleGeometry, int) {
		const auto right = st::aiTonePreviewExampleCardPadding.left();
		const auto &button = st::aiTonePreviewAnotherExampleButton;
		const auto &title = st::aiComposeCardTitle;
		const auto shift = title.style.font->ascent
			- button.style.font->ascent
			- button.textTop
			- button.padding.top();
		_another->moveToRight(
			right,
			titleGeometry.top() + shift,
			width);
	}, lifetime());
	_another->setClickedCallback([=] {
		_anotherExampleRequested.fire({});
	});
}

void PreviewAiToneExampleCard::showExample(
		Data::AiComposeToneExample example) {
	_beforeBody->setText(example.from);
	_afterBody->setText(example.to);
	_beforeSkeleton.stop();
	_afterSkeleton.stop();
	if (width() > 0) {
		resizeToWidth(width());
	}
}

void PreviewAiToneExampleCard::showSkeleton(bool shown) {
	if (shown) {
		_beforeSkeleton.start();
		_afterSkeleton.start();
	} else {
		_beforeSkeleton.stop();
		_afterSkeleton.stop();
	}
}

rpl::producer<> PreviewAiToneExampleCard::anotherExampleRequested() const {
	return _anotherExampleRequested.events();
}

int PreviewAiToneExampleCard::resizeGetHeight(int newWidth) {
	_layout->resizeToWidth(newWidth);
	_layout->moveToLeft(0, 0, newWidth);
	return _layout->heightNoMargins();
}

void PreviewAiToneExampleCard::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	p.setPen(Qt::NoPen);
	p.setBrush(st::aiTonePreviewExampleCardBg);
	p.drawRoundedRect(
		rect(),
		st::aiTonePreviewExampleCardRadius,
		st::aiTonePreviewExampleCardRadius);
}

void ShowToneAddedToast(
		std::shared_ptr<Ui::Show> show,
		not_null<Main::Session*> session,
		const Data::AiComposeTone &tone) {
	const auto size = QSize(
		st::aiComposeToneToastIconSize.width(),
		st::aiComposeToneToastIconSize.height());
	show->showToast(Ui::Toast::Config{
		.title = tr::lng_ai_compose_tone_added(tr::now),
		.text = tr::lng_ai_compose_tone_added_description(
			tr::now,
			lt_name,
			tr::marked(tone.title),
			tr::marked),
		.iconContent = Ui::MakeCustomEmojiToastIcon(
			session,
			tone.emojiId,
			size),
		.iconPadding = st::aiComposeToneToastIconPadding,
		.duration = kToastDuration,
	});
}

} // namespace

void PreviewAiToneBox(
		not_null<Ui::GenericBox*> box,
		not_null<Main::Session*> session,
		Data::AiComposeTone tone) {
	box->setStyle(st::aiComposeBox);
	box->setNoContentMargin(true);
	box->setWidth(st::boxWideWidth);
	box->addTopButton(st::aiComposeBoxClose, [=] { box->closeBox(); });

	const auto top = box->setPinnedToTopContent(
		object_ptr<Ui::VerticalLayout>(box));
	Ui::AddSkip(top, st::defaultVerticalListSkip * 4);
	AddAiToneIconPreview(top, session, rpl::single(tone.emojiId), nullptr);
	top->add(
		object_ptr<Ui::FlatLabel>(
			top,
			rpl::single(tone.title),
			st::aiTonePreviewTitleLabel),
		st::aiTonePreviewTitleMargin,
		style::al_top);
	top->add(
		object_ptr<Ui::FlatLabel>(
			top,
			tr::lng_ai_compose_tone_preview_about(),
			st::aiTonePreviewAboutLabel),
		st::aiTonePreviewAboutMargin,
		style::al_top
	)->setTryMakeSimilarLines(true);

	const auto body = box->verticalLayout();

	struct State {
		int examplesCount = 0;
		bool requesting = false;
	};
	const auto state = box->lifetime().make_state<State>();
	state->examplesCount = tone.firstExample ? 1 : 0;

	const auto card = body->add(
		object_ptr<PreviewAiToneExampleCard>(body),
		st::aiTonePreviewExampleCardMargin);
	const auto loadAnother = [=] {
		if (state->requesting) {
			return;
		}
		state->requesting = true;
		card->showSkeleton(true);
		const auto num = state->examplesCount;
		session->data().aiComposeTones().getToneExample(
			tone,
			num,
			crl::guard(box, [=](Data::AiComposeToneExample example) {
				state->requesting = false;
				++state->examplesCount;
				card->showExample(std::move(example));
			}),
			crl::guard(box, [=](const MTP::Error &) {
				state->requesting = false;
				card->showSkeleton(false);
				box->showToast(tr::lng_ai_compose_error(tr::now));
			}));
	};
	card->anotherExampleRequested(
	) | rpl::on_next(loadAnother, card->lifetime());

	if (tone.firstExample) {
		card->showExample(*tone.firstExample);
	} else {
		loadAnother();
	}

	const auto attribution = body->add(
		object_ptr<Ui::FlatLabel>(body, st::aiTonePreviewAttributionLabel),
		st::aiTonePreviewAttributionMargin,
		style::al_top);

	auto text = tr::marked();
	if (tone.installsCount > 0) {
		text = tr::lng_ai_compose_tone_preview_used_by(
			tr::now,
			lt_count,
			tone.installsCount,
			tr::marked);
	}
	if (const auto user = session->data().userLoaded(tone.authorId)) {
		const auto name = user->shortName();
		auto mention = tr::marked(name);
		mention.entities.push_back(EntityInText(
			EntityType::MentionName,
			0,
			name.size(),
			TextUtilities::MentionNameDataFromFields({
				.selfId = session->userId().bare,
				.userId = tone.authorId.bare,
				.accessHash = user->accessHash(),
			})));
		auto createdBy = tr::lng_ai_compose_tone_preview_created_by(
			tr::now,
			lt_user,
			std::move(mention),
			tr::marked);
		if (!text.empty()) {
			text.append(' ').append(std::move(createdBy));
		} else {
			text = std::move(createdBy);
		}
	}
	if (text.empty()) {
		attribution->setVisible(false);
	} else {
		attribution->setMarkedText(
			std::move(text),
			Core::TextContext({ .session = session }));
	}

	const auto add = box->addButton(
		tr::lng_ai_compose_tone_preview_add(),
		[=] {
			session->data().aiComposeTones().save(tone, false);
			const auto show = box->uiShow();
			box->closeBox();
			ShowToneAddedToast(show, session, tone);
		});
	add->setFullRadius(true);
}
