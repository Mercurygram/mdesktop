/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "ui/boxes/emoji_stake_box.h"

#include "base/object_ptr.h"
#include "chat_helpers/stickers_lottie.h"
#include "data/components/credits.h"
#include "data/stickers/data_custom_emoji.h"
#include "data/data_document.h"
#include "data/data_session.h"
#include "info/channel_statistics/earn/earn_icons.h"
#include "lang/lang_keys.h"
#include "lottie/lottie_icon.h"
#include "main/main_session.h"
#include "settings/settings_common.h" // CreateLottieIcon
#include "settings/settings_credits_graphics.h" // AddBalanceWidget
#include "ui/controls/ton_common.h"
#include "ui/text/custom_emoji_helper.h"
#include "ui/text/custom_emoji_text_badge.h"
#include "ui/text/text_lottie_custom_emoji.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/fields/number_input.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/padding_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/painter.h"
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
		{ .name = u"dice_6"_q, .sizeOverride = { size, size } },
		st::settingLocalPasscodeIconPadding);
	const auto animate = std::move(icon.animate);
	box->showFinishes() | rpl::take(1) | rpl::on_next([=] {
		animate(anim::repeat::once);
	}, box->lifetime());
	return std::move(icon.widget);
}

[[nodiscard]] object_ptr<RpWidget> MakeTable(
		not_null<RpWidget*> parent,
		const EmojiGameStakeArgs &args) {
	auto result = object_ptr<RpWidget>(parent);
	const auto raw = result.data();

	struct State {
		std::array<std::unique_ptr<Text::CustomEmoji>, 6> dices;
		std::array<Text::String, 7> multiplicators;
	};
	const auto state = raw->lifetime().make_state<State>();
	const auto serialize = [&](int milliReward) {
		return QString(QChar(0xD7)) + QString::number(milliReward / 1000.);
	};
	const auto esize = st::largeEmojiSize;
	for (auto i = 0; i != 6; ++i) {
		state->dices[i] = Lottie::MakeEmoji({
			.name = u"dice_%1"_q.arg(i + 1),
			.sizeOverride = QSize(esize, esize),
		}, [=] { raw->update(); });
		const auto value = (i < args.milliRewards.size())
			? args.milliRewards[i]
			: 0;
		state->multiplicators[i].setText(
			st::semiboldTextStyle,
			serialize(value));
	}
	state->multiplicators[6].setText(
		st::semiboldTextStyle,
		serialize(args.jackpotMilliReward));

	raw->paintOn([=](QPainter &p) {
		auto path = QPainterPath();
		const auto &st = st::defaultTable;
		const auto border = st.border;
		const auto half = border / 2.;
		const auto add = QMargins(border, border, border, border);
		const auto inner = raw->rect().marginsRemoved(add);
		path.addRoundedRect(inner, st.radius, st.radius);
		{
			const auto y = border + inner.height() / 2.;
			path.moveTo(border + half, y);
			path.lineTo(border + inner.width() - half, y);
		}
		{
			const auto x = border + inner.width() / 2.;
			path.moveTo(x, border + half);
			path.lineTo(x, border + inner.height() - half);
		}
		{
			const auto x = border + (inner.width() - border) / 4.;
			path.moveTo(x, border + half);
			path.lineTo(x, border + inner.height() - half);
		}
		{
			const auto x = border
				+ (inner.width() / 2.)
				+ (inner.width() - border) / 4.;
			path.moveTo(x, border + half);
			path.lineTo(x, border + (inner.height() / 2.) - half);
		}
		auto pen = st.borderFg->p;
		pen.setWidth(st.border);
		p.setPen(pen);
		p.setBrush(Qt::NoBrush);

		auto hq = PainterHighQualityEnabler(p);
		p.drawPath(path);

		const auto etop = -style::ConvertScale(10);
		const auto ttop = etop + esize;
		{
			auto left = border + half;
			const auto width = (inner.width() - 3 * border) / 4.;
			auto top = border + half;
			const auto now = crl::now();
			for (auto i = 0; i != 7; ++i, left += width + border) {
				if (i == 4) {
					left -= 4 * (width + border);
					top += border + half + inner.height() / 2.;
				}
				const auto x = int(base::SafeRound(left));
				const auto y = int(base::SafeRound(top));
				const auto right = (i < 6)
					? int(base::SafeRound(left + width))
					: int(base::SafeRound(left + 2 * width + border));
				const auto w = (right - x);

				if (i < 6) {
					state->dices[i]->paint(p, {
						.textColor = st::windowBoldFg->c,
						.now = now,
						.position = QPoint(x + (w - esize) / 2, y + etop),
						.paused = true,
						.internal = { .forceLastFrame = true }
					});
				} else {
					const auto e = state->dices.back().get();
					for (auto j = 0; j != 3; ++j) {
						e->paint(p, {
							.textColor = st::windowBoldFg->c,
							.now = now,
							.position = QPoint(
								x + (w - 3 * esize) / 2 + (esize * j),
								y + etop),
							.paused = true,
							.internal = { .forceLastFrame = true }
						});
					}
				}

				p.setPen(st::windowBoldFg);
				const auto &mul = state->multiplicators[i];
				mul.draw(p, {
					.position = { x + (w - mul.maxWidth()) / 2, y + ttop },
				});
			}
		}
	});
	raw->widthValue() | rpl::on_next([=](int width) {
		raw->resize(width, width / 3);
	}, raw->lifetime());
	return result;
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
	const auto container = box->verticalLayout();

	const auto skip = st::confcallLinkHeaderIconPadding.bottom();
	box->addRow(
		MakeLogo(box),
		st::boxRowPadding + QMargins(0, 0, 0, skip),
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
		st::boxRowPadding + QMargins(0, 0, 0, skip),
		style::al_top
	)->setTryMakeSimilarLines(true);

	const auto sub = st::boxRowPadding - st::defaultSubsectionTitlePadding;
	const auto subtitlePadding = QMargins(sub.left(), 0, sub.right(), 0);
	AddSubsectionTitle(
		container,
		tr::lng_stake_game_results(),
		subtitlePadding);

	const auto half = (st::defaultTable.border + 1) / 2;
	box->addRow(
		MakeTable(box, args),
		st::boxRowPadding + QMargins(-half, 0, -half, skip / 2),
		style::al_top);

	const auto six = Lottie::IconDescriptor{
		.name = u"dice_6"_q,
		.sizeOverride = { st::emojiSize, st::emojiSize },
	};
	const auto sixText = Text::LottieEmoji(six);
	const auto sixContext = Text::LottieEmojiContext(six);
	const auto factory = [=](
			QStringView data,
			const Text::MarkedContext &context) {
		if (auto result = sixContext.customEmojiFactory(data, context)) {
			return std::make_unique<Text::LimitedLoopsEmoji>(
				std::move(result),
				0,
				true);
		}
		return std::unique_ptr<Text::LimitedLoopsEmoji>();
	};
	const auto context = Text::MarkedContext{
		.customEmojiFactory = factory,
	};
	const auto resets = box->addRow(
		object_ptr<FlatLabel>(
			box,
			tr::lng_stake_game_resets(
				lt_emoji,
				rpl::single(sixText),
				tr::rich),
			st::confcallLinkFooterOr,
			st::defaultPopupMenu,
			context),
		st::boxRowPadding,
		style::al_top);
	resets->setTryMakeSimilarLines(true);
	resets->setAnimationsPausedCallback([=] {
		return FlatLabel::WhichAnimationsPaused::All;
	});

	AddSubsectionTitle(
		container,
		tr::lng_stake_game_your(),
		subtitlePadding + QMargins(0, skip, 0, sub.bottom()));
	const auto field = AddTonInputField(container, {
		.value = args.currentStake,
	});
	box->setFocusCallback([=] {
		field->setFocusFast();
	});

	const auto submit = args.submit;
	const auto callback = [=] {
		const auto text = field->getLastText();
		const auto now = Ui::ParseTonAmountString(text);
		if (!now) {
			field->showError();
		} else {
			box->closeBox();
			submit(*now);
		}
	};
	field->submits() | rpl::on_next(callback, field->lifetime());
	const auto button = box->addButton(rpl::single(QString()), callback);

	button->setText(tr::lng_stake_game_save_and_roll(
	) | rpl::map([=](QString text) {
		return tr::marked(
			QString::fromUtf8("\xf0\x9f\x8e\xb2")
		).append(' ').append(text);
	}));

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
}

} // namespace Ui
