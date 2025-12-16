/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "ui/boxes/emoji_stake_box.h"

#include "base/object_ptr.h"
#include "data/components/credits.h"
#include "info/channel_statistics/earn/earn_icons.h"
#include "lang/lang_keys.h"
#include "lottie/lottie_icon.h"
#include "main/main_session.h"
#include "settings/settings_common.h" // CreateLottieIcon
#include "settings/settings_credits_graphics.h" // AddBalanceWidget
#include "ui/controls/ton_common.h"
#include "ui/text/custom_emoji_helper.h"
#include "ui/text/custom_emoji_text_badge.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/fields/number_input.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/padding_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/vertical_list.h"
#include "styles/style_calls.h" // confcallJoinBox
#include "styles/style_chat.h"
#include "styles/style_chat_helpers.h"
#include "styles/style_credits.h" // creditsHistoryRightSkip
#include "styles/style_layers.h"
#include "styles/style_settings.h" // settingsCloudPasswordIconSize
#include "styles/style_widgets.h"

namespace Ui {
namespace {

[[nodiscard]] not_null<RpWidget*> AddMoneyInputIcon(
		not_null<QWidget*> parent,
		Text::PaletteDependentEmoji emoji) {
	auto helper = Text::CustomEmojiHelper();
	auto text = helper.paletteDependent(std::move(emoji));
	return CreateChild<FlatLabel>(
		parent,
		rpl::single(std::move(text)),
		st::defaultFlatLabel,
		st::defaultPopupMenu,
		helper.context());
}

[[nodiscard]] object_ptr<RpWidget> MakeLogo(not_null<GenericBox*> box) {
	const auto &size = st::settingsCloudPasswordIconSize;
	auto icon = Settings::CreateLottieIcon(
		box->verticalLayout(),
		{ .name = u"dice_idle"_q, .sizeOverride = { size, size } },
		st::settingLocalPasscodeIconPadding);
	const auto animate = std::move(icon.animate);
	box->showFinishes() | rpl::take(1) | rpl::on_next([=] {
		animate(anim::repeat::loop);
	}, box->lifetime());
	return std::move(icon.widget);
}

} // namespace

not_null<NumberInput*> AddStarsInputField(
		not_null<VerticalLayout*> container,
		StarsInputFieldArgs &&args) {
	const auto wrap = container->add(
		object_ptr<FixedHeightWidget>(
			container,
			st::editTagField.heightMin),
		st::boxRowPadding);
	const auto result = CreateChild<NumberInput>(
		wrap,
		st::editTagField,
		rpl::single(u"0"_q),
		args.value ? QString::number(*args.value) : QString(),
		args.max ? args.max : std::numeric_limits<int>::max());
	const auto icon = AddMoneyInputIcon(
		result,
		Earn::IconCreditsEmoji());

	wrap->widthValue() | rpl::on_next([=](int width) {
		icon->move(st::starsFieldIconPosition);
		result->move(0, 0);
		result->resize(width, result->height());
		wrap->resize(width, result->height());
	}, wrap->lifetime());

	return result;
}

not_null<InputField*> AddTonInputField(
		not_null<VerticalLayout*> container,
		TonInputFieldArgs &&args) {
	const auto wrap = container->add(
		object_ptr<FixedHeightWidget>(
			container,
			st::editTagField.heightMin),
		st::boxRowPadding);
	const auto result = CreateTonAmountInput(
		wrap,
		rpl::single('0' + TonAmountSeparator() + '0'),
		args.value);
	const auto icon = AddMoneyInputIcon(
		result,
		Earn::IconCurrencyEmoji());

	wrap->widthValue() | rpl::on_next([=](int width) {
		icon->move(st::tonFieldIconPosition);
		result->move(0, 0);
		result->resize(width, result->height());
		wrap->resize(width, result->height());
	}, wrap->lifetime());

	return result;
}

void EmojiGameStakeBox(
		not_null<GenericBox*> box,
		EmojiGameStakeArgs &&args) {
	box->setStyle(st::confcallJoinBox);
	box->setWidth(st::boxWideWidth);
	box->setNoContentMargin(true);

	box->addRow(
		MakeLogo(box),
		st::boxRowPadding + st::confcallLinkHeaderIconPadding,
		style::al_top);

	auto helper = Text::CustomEmojiHelper();
	const auto beta = helper.paletteDependent(
		Text::CustomEmojiTextBadge(
			tr::lng_stake_game_beta(tr::now),
			st::customEmojiTextBadge));
	auto title = tr::lng_stake_game_title(
		tr::marked
	) | rpl::map([=](TextWithEntities &&text) {
		return text.append(' ').append(beta);
	});
	box->addRow(
		object_ptr<FlatLabel>(
			box,
			std::move(title),
			st::boxTitle,
			st::defaultPopupMenu,
			helper.context()),
		st::boxRowPadding + st::confcallLinkTitlePadding,
		style::al_top);
	box->addRow(
		object_ptr<FlatLabel>(
			box,
			tr::lng_stake_game_about(tr::rich),
			st::confcallLinkCenteredText),
		st::boxRowPadding,
		style::al_top
	)->setTryMakeSimilarLines(true);

	const auto container = box->verticalLayout();

	AddSubsectionTitle(container, tr::lng_stake_game_results());

	const auto close = CreateChild<IconButton>(
		container,
		st::boxTitleClose);
	close->setClickedCallback([=] { box->closeBox(); });
	container->widthValue() | rpl::on_next([=](int) {
		close->moveToRight(0, 0);
	}, close->lifetime());

	const auto session = args.session;
	session->credits().tonLoad(true);
	const auto balance = Settings::AddBalanceWidget(
		container,
		session,
		session->credits().tonBalanceValue(),
		false);
	rpl::combine(
		balance->sizeValue(),
		container->sizeValue()
	) | rpl::on_next([=](const QSize &, const QSize &) {
		balance->moveToLeft(
			st::creditsHistoryRightSkip * 2,
			st::creditsHistoryRightSkip);
		balance->update();
	}, balance->lifetime());

	box->addRow(
		object_ptr<FlatLabel>(
			box,
			tr::lng_stake_game_resets(tr::rich),
			st::confcallLinkCenteredText),
		st::boxRowPadding,
		style::al_top
	)->setTryMakeSimilarLines(true);

	AddSubsectionTitle(container, tr::lng_stake_game_your());
	const auto field = AddTonInputField(container, {
		.value = args.currentStake,
	});

	const auto submit = args.submit;
	const auto button = box->addButton(rpl::single(QString()), [=] {
		const auto text = field->getLastText();
		const auto now = Ui::ParseTonAmountString(text);
		if (!now) {
			field->showError();
		} else {
			box->closeBox();
			submit(*now);
		}
	});

	button->setText(tr::lng_stake_game_save_and_roll(
	) | rpl::map([=](QString text) {
		return tr::marked(
			QString::fromUtf8("\xf0\x9f\x8e\xb2")
		).append(' ').append(text);
	}));
}

} // namespace Ui
