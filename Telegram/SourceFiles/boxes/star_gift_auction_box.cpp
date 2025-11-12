/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "boxes/star_gift_auction_box.h"

#include "base/unixtime.h"
#include "boxes/star_gift_box.h"
#include "core/credits_amount.h"
#include "data/components/credits.h"
#include "data/components/gift_auctions.h"
#include "data/data_message_reactions.h"
#include "data/data_user.h"
#include "info/channel_statistics/earn/earn_icons.h"
#include "info/peer_gifts/info_peer_gifts_common.h"
#include "lang/lang_keys.h"
#include "main/main_app_config.h"
#include "main/main_session.h"
#include "payments/ui/payments_reaction_box.h"
#include "payments/payments_checkout_process.h"
#include "ui/controls/button_labels.h"
#include "ui/controls/table_rows.h"
#include "ui/layers/generic_box.h"
#include "ui/text/format_values.h"
#include "ui/text/text_utilities.h"
#include "ui/toast/toast.h"
#include "ui/widgets/buttons.h"
#include "ui/wrap/table_layout.h"
#include "ui/dynamic_thumbnails.h"
#include "window/window_session_controller.h"
#include "styles/style_chat.h"
#include "styles/style_layers.h"
#include "styles/style_credits.h"

namespace Ui {
namespace {

[[nodiscard]] rpl::producer<int> MinutesLeftTillValue(TimeId endDate) {
	return [=](auto consumer) {
		auto lifetime = rpl::lifetime();

		const auto now = base::unixtime::now();
		if (endDate <= now) {
			consumer.put_next(0);
			consumer.put_done();
			return lifetime;
		}

		const auto timer = lifetime.make_state<base::Timer>();
		const auto callback = [=] {
			const auto now = base::unixtime::now();
			const auto left = (endDate > now) ? endDate - now : 0;
			const auto minutes = (left + 59) / 60;
			consumer.put_next_copy(minutes);
			if (minutes) {
				const auto next = left % 60;
				const auto wait = next ? next : 60;
				timer->callOnce(wait * crl::time(1000));
			} else {
				consumer.put_done();
			}
		};
		timer->setCallback(callback);
		callback();

		return lifetime;
	};
}

struct AuctionBidBoxArgs {
	std::shared_ptr<ChatHelpers::Show> show;
	rpl::producer<Data::GiftAuctionState> state;
	Fn<void(int)> save;
};

void PlaceAuctionBid(
		std::shared_ptr<ChatHelpers::Show> show,
		not_null<PeerData*> to,
		int64 amount,
		const Data::GiftAuctionState &state,
		Fn<void(Payments::CheckoutResult)> done) {
	auto paymentDone = [=](
			Payments::CheckoutResult result,
			const MTPUpdates *updates) {
		done(result);
	};
	const auto invoice = state.my.bid
		? MTP_inputInvoiceStarGiftAuctionUpdateBid(
			MTP_long(state.gift->id),
			MTP_long(amount))
		: MTP_inputInvoiceStarGiftAuctionBid(
			MTP_flags(0),
			to->input,
			MTP_long(state.gift->id),
			MTP_long(amount),
			MTPTextWithEntities());
	RequestOurForm(show, invoice, [=](
			uint64 formId,
			CreditsAmount price,
			std::optional<Payments::CheckoutResult> failure) {
		if (failure) {
			paymentDone(*failure, nullptr);
		} else {
			SubmitStarsForm(
				show,
				invoice,
				formId,
				price.whole(),
				paymentDone);
		}
	});
}

void AuctionBidBox(not_null<GenericBox*> box, AuctionBidBoxArgs &&args) {
	const auto weak = base::make_weak(box);
	const auto state = box->lifetime().make_state<
		rpl::variable<Data::GiftAuctionState>
	>(std::move(args.state));
	auto submit = [=](rpl::producer<int> amount) {
		return std::move(amount) | rpl::map([=](int count) {
			return TextWithEntities{ "Place a " + QString::number(count) + " Bid" };
		});
	};
	const auto show = args.show;
	const auto session = &show->session();
	auto top = std::vector<PaidReactionTop>{
		PaidReactionTop{
			.name = session->user()->shortName(),
			.photo = MakeUserpicThumbnail(session->user()),
			.barePeerId = uint64(session->userPeerId().value),
			.count = int(state->current().my.bid),
			.my = true,
		}
	};
	PaidReactionsBox(box, {
		.min = int(state->current().my.bid
			? state->current().my.minBidAmount
			: state->current().minBidAmount),
		.chosen = int(state->current().my.bid),
		.max = 10'000,
		.top = std::move(top),
		.session = session,
		.name = u"hello"_q,
		.submit = submit,
		.colorings = session->appConfig().groupCallColorings(),
		.balanceValue = session->credits().balanceValue(),
		.send = [weak, save = args.save](int count, uint64 barePeerId) {
			save(count);
			if (const auto strong = weak.get()) {
				strong->closeBox();
			}
		},
		.giftAuction = true,
	});
}

[[nodiscard]] object_ptr<BoxContent> MakeAuctionBidBox(
		AuctionBidBoxArgs &&args) {
	return Box(AuctionBidBox, std::move(args));
}

[[nodiscard]] object_ptr<RpWidget> MakeAveragePriceValue(
		not_null<TableLayout*> table,
		std::shared_ptr<TableRowTooltipData> tooltip,
		const QString &name,
		int64 amount) {
	auto helper = Ui::Text::CustomEmojiHelper();
	const auto price = helper.paletteDependent(Ui::Earn::IconCreditsEmoji(
	)).append(' ').append(
		Lang::FormatCreditsAmountDecimal(CreditsAmount{ amount }));
	return MakeTableValueWithTooltip(
		table,
		std::move(tooltip),
		price,
		tr::lng_auction_average_tooltip(
			tr::now,
			lt_amount,
			tr::bold(
				Ui::Text::IconEmoji(&st::starIconEmojiInline).append(
					Lang::FormatCountDecimal(amount))),
			lt_gift,
			tr::bold(name),
			tr::marked),
		helper.context());
}

[[nodiscard]] object_ptr<TableLayout> AuctionInfoTable(
		not_null<QWidget*> parent,
		not_null<Ui::VerticalLayout*> container,
		rpl::producer<Data::GiftAuctionState> value) {
	auto result = object_ptr<TableLayout>(parent.get(), st::defaultTable);
	const auto raw = result.data();

	struct State {
		rpl::variable<Data::GiftAuctionState> value;
		rpl::variable<bool> finished;
	};
	const auto state = raw->lifetime().make_state<State>();
	state->value = std::move(value);

	const auto &now = state->value.current();
	const auto name = now.gift->resellTitle;
	state->finished = now.finished()
		? (rpl::single(true) | rpl::type_erased())
		: (MinutesLeftTillValue(now.endDate) | rpl::map(!rpl::mappers::_1));

	const auto date = [&](TimeId time) {
		return rpl::single(
			tr::marked(langDateTime(base::unixtime::parse(time))));
	};
	AddTableRow(
		raw,
		rpl::conditional(
			state->finished.value(),
			tr::lng_gift_link_label_first_sale(),
			tr::lng_auction_start_label()),
		date(now.startDate));
	AddTableRow(
		raw,
		rpl::conditional(
			state->finished.value(),
			tr::lng_gift_link_label_last_sale(),
			tr::lng_auction_end_label()),
		date(now.endDate));

	auto roundText = state->value.value(
	) | rpl::map([](const Data::GiftAuctionState &state) {
		const auto wrapped = [](int count) {
			return rpl::single(tr::marked(Lang::FormatCountDecimal(count)));
		};
		return tr::lng_auction_round_value(
			lt_n,
			wrapped(state.currentRound),
			lt_amount,
			wrapped(state.totalRounds),
			tr::marked);
	}) | rpl::flatten_latest();
	const auto round = AddTableRow(
		raw,
		tr::lng_auction_round_label(),
		std::move(roundText));

	auto availabilityText = state->value.value(
	) | rpl::map([](const Data::GiftAuctionState &state) {
		const auto wrapped = [](int count) {
			return rpl::single(tr::marked(Lang::FormatCountDecimal(count)));
		};
		return tr::lng_auction_availability_value(
			lt_n,
			wrapped(state.giftsLeft),
			lt_amount,
			wrapped(state.gift->limitedCount),
			tr::marked);
	}) | rpl::flatten_latest();
	AddTableRow(
		raw,
		tr::lng_auction_availability_label(),
		std::move(availabilityText));

	const auto tooltip = std::make_shared<TableRowTooltipData>(
		TableRowTooltipData{ .parent = container });
	state->value.value(
	) | rpl::map([](const Data::GiftAuctionState &state) {
		return state.averagePrice;
	}) | rpl::filter(
		rpl::mappers::_1 != 0
	) | rpl::take(
		1
	) | rpl::start_with_next([=](int64 price) {
		delete round;

		raw->insertRow(
			2,
			object_ptr<FlatLabel>(
				raw,
				tr::lng_auction_average_label(),
				raw->st().defaultLabel),
			MakeAveragePriceValue(raw, tooltip, name, price),
			st::giveawayGiftCodeLabelMargin,
			st::giveawayGiftCodeValueMargin);
		raw->resizeToWidth(raw->widthNoMargins());
	}, raw->lifetime());

	return result;
}

void AuctionAboutBox(
		not_null<GenericBox*> box,
		int rounds,
		int giftsPerRound) {

}

void AuctionInfoBox(
		not_null<GenericBox*> box,
		std::shared_ptr<ChatHelpers::Show> show,
		not_null<PeerData*> peer,
		rpl::producer<Data::GiftAuctionState> value) {
	using namespace Info::PeerGifts;

	struct State {
		explicit State(not_null<Main::Session*> session)
		: delegate(session, GiftButtonMode::Minimal) {
		}

		Delegate delegate;
		rpl::variable<Data::GiftAuctionState> value;
		rpl::variable<int> minutesLeft;
	};
	const auto state = box->lifetime().make_state<State>(&show->session());
	state->value = std::move(value);
	state->minutesLeft = MinutesLeftTillValue(
		state->value.current().endDate);

	box->setStyle(st::giftBox);

	const auto name = state->value.current().gift->resellTitle;
	const auto extend = st::defaultDropdownMenu.wrap.shadow.extend;
	const auto side = st::giftBoxGiftSmall;
	const auto size = QSize(side, side).grownBy(extend);
	const auto preview = box->addRow(
		object_ptr<FixedHeightWidget>(box, size.height()),
		st::auctionInfoPreviewMargin);
	const auto gift = CreateChild<GiftButton>(preview, &state->delegate);
	gift->setAttribute(Qt::WA_TransparentForMouseEvents);
	gift->setDescriptor(GiftTypeStars{
		.info = *state->value.current().gift,
	}, GiftButtonMode::Minimal);

	preview->widthValue() | rpl::start_with_next([=](int width) {
		const auto left = (width - size.width()) / 2;
		gift->setGeometry(
			QRect(QPoint(left, 0), size).marginsRemoved(extend),
			extend);
	}, gift->lifetime());

	const auto rounds = state->value.current().totalRounds;
	const auto perRound = state->value.current().gift->auctionGiftsPerRound;
	auto aboutText = state->value.value(
	) | rpl::map([=](const Data::GiftAuctionState &state) {
		if (state.finished()) {
			return tr::lng_auction_text_ended(tr::now, tr::marked);
		}
		return tr::lng_auction_text(
			tr::now,
			lt_count,
			perRound,
			lt_name,
			tr::bold(name),
			lt_link,
			tr::lng_auction_text_link(
				tr::now,
				lt_arrow,
				Text::IconEmoji(&st::textMoreIconEmoji),
				tr::link),
			tr::rich);
	});
	box->addRow(
		object_ptr<FlatLabel>(
			box,
			name,
			st::uniqueGiftTitle),
		style::al_top);
	const auto about = box->addRow(
		object_ptr<FlatLabel>(
			box,
			std::move(aboutText),
			st::uniqueGiftSubtitle),
		st::boxRowPadding + QMargins(0, st::auctionInfoSubtitleSkip, 0, 0),
		style::al_top);
	about->setTryMakeSimilarLines(true);
	box->resizeToWidth(box->widthNoMargins());

	about->setClickHandlerFilter([=](const auto &...) {
		show->show(Box(AuctionAboutBox, rounds, perRound));
		return false;
	});

	box->addRow(
		AuctionInfoTable(box, box->verticalLayout(), state->value.value()),
		st::boxRowPadding + st::auctionInfoTableMargin);

	const auto button = box->addButton(rpl::single(QString()), [=] {
		if (state->value.current().finished() || !state->minutesLeft.current()) {
			box->closeBox();
			return;
		}

		const auto save = [=](int amount) {
			const auto &now = state->value.current();
			const auto was = (now.my.bid > 0);
			const auto perRound = now.gift->auctionGiftsPerRound;
			const auto done = [=](Payments::CheckoutResult result) {
				if (result == Payments::CheckoutResult::Paid) {
					show->showToast({
						.title = (was
							? tr::lng_auction_bid_increased_title
							: tr::lng_auction_bid_placed_title)(
								tr::now),
						.text = tr::lng_auction_bid_done_text(
							tr::now,
							lt_count,
							perRound,
							tr::rich)
					});
				}
			};
			PlaceAuctionBid(
				show,
				peer,
				amount,
				now,
				done);
		};
		const auto bidBox = show->show(MakeAuctionBidBox({
			.show = show,
			.state = state->value.value(),
			.save = save,
		}));
		bidBox->boxClosing(
		) | rpl::start_with_next([=] {
			box->closeBox();
		}, box->lifetime());
	});

	auto buttonTitle = rpl::combine(
		state->value.value(),
		state->minutesLeft.value()
	) | rpl::map([=](const Data::GiftAuctionState &state, int minutes) {
		return (state.finished() || minutes <= 0)
			? tr::lng_box_ok(tr::marked)
			: tr::lng_auction_join_button(tr::marked);
	}) | rpl::flatten_latest();

	auto buttonSubtitle = rpl::combine(
		state->value.value(),
		state->minutesLeft.value()
	) | rpl::map([=](const Data::GiftAuctionState &state, int minutes) {
		if (state.finished() || minutes <= 0) {
			return rpl::single(TextWithEntities());
		}
		const auto hours = (minutes / 60);
		minutes -= (hours * 60);

		auto value = [](int count) {
			return rpl::single(tr::marked(QString::number(count)));
		};
		return tr::lng_auction_join_time_left(
			lt_time,
			(hours
				? tr::lng_auction_join_time_medium(
					lt_hours,
					value(hours),
					lt_minutes,
					value(minutes),
					tr::marked)
				: tr::lng_auction_join_time_small(
					lt_minutes,
					value(minutes),
					tr::marked)),
			tr::marked);
	}) | rpl::flatten_latest();

	SetButtonTwoLabels(
		button,
		std::move(buttonTitle),
		std::move(buttonSubtitle),
		st::resaleButtonTitle,
		st::resaleButtonSubtitle);

	rpl::combine(
		state->value.value(),
		state->minutesLeft.value()
	) | rpl::start_with_next([=](
			const Data::GiftAuctionState &state,
			int minutes) {
		about->setTextColorOverride((state.finished() || minutes <= 0)
			? st::attentionButtonFg->c
			: std::optional<QColor>());
	}, box->lifetime());
}

} // namespace

rpl::lifetime ShowStarGiftAuction(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer,
		QString slug,
		Fn<void()> finishRequesting,
		Fn<void()> boxClosed) {
	const auto weak = base::make_weak(controller);
	const auto session = &controller->session();
	const auto value = std::make_shared<
		rpl::variable<Data::GiftAuctionState>
	>();
	return session->giftAuctions().state(
		slug
	) | rpl::start_with_next([=](Data::GiftAuctionState &&state) {
		if (const auto onstack = finishRequesting) {
			onstack();
		}
		const auto initial = !value->current().gift.has_value();
		(*value) = std::move(state);
		if (initial) {
			if (const auto strong = weak.get()) {
				const auto box = strong->show(Box(
					AuctionInfoBox,
					strong->uiShow(),
					peer,
					value->value()));
				box->boxClosing(
				) | rpl::start_with_next(boxClosed, box->lifetime());
			}
		}
	});
}

} // namespace Ui
