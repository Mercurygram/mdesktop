/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "calls/group/ui/calls_group_stars_coloring.h"

#include "base/object_ptr.h"
#include "lang/lang_keys.h"
#include "ui/widgets/labels.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "styles/style_credits.h"
#include "styles/style_layers.h"
#include "styles/style_premium.h"

namespace Calls::Group::Ui {
namespace {

[[nodiscard]] not_null<Ui::RpWidget*> MakeInfoBlock(
		not_null<Ui::RpWidget*> parent,
		rpl::producer<QString> title,
		rpl::producer<QString> subtext) {
	const auto result = CreateChild<Ui::RpWidget>(parent);

	const auto titleHeight = st::videoStreamInfoTitle.style.font->height;
	const auto subtextHeight = st::videoStreamInfoSubtext.style.font->height;
	const auto height = titleHeight + subtextHeight;

	result->paintRequest() | rpl::start_with_next([=] {
		auto p = QPainter(result);
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::groupCallMembersBgOver);
		const auto radius = st::boxRadius;
		p.drawRoundedRect(result->rect(), radius, radius);
	}, result->lifetime());

	result->resize(
		result ->width(),
		QSize(height, height).grownBy(st::videoStreamInfoPadding).height());

	const auto titleLabel = CreateChild<Ui::FlatLabel>(
		result,
		std::move(title),
		st::videoStreamInfoTitle);
	const auto subtextLabel = CreateChild<Ui::FlatLabel>(
		result,
		std::move(subtext),
		st::videoStreamInfoSubtext);

	rpl::combine(
		result->widthValue(),
		titleLabel->widthValue(),
		subtextLabel->widthValue()
	) | rpl::start_with_next([=](int width, int titlew, int subtextw) {
		const auto padding = st::videoStreamInfoPadding;
		titleLabel->moveToLeft((width - titlew) / 2, padding.top(), width);
		subtextLabel->moveToLeft(
			(width - subtextw) / 2,
			padding.top() + titleHeight,
			width);
	}, result->lifetime());

	return result;
}

} // namespace

StarsColoring StarsColoringForCount(int stars) {
    //name: "Purple" [STRING],
    //center_color: 11431086 [INT],
    //edge_color: 8669060 [INT],
    //pattern_color: 4656199 [INT],
    //text_color: 15977459 [INT],

    //name: "Celtic Blue" [STRING],
    //center_color: 4569325 [INT],
    //edge_color: 3704537 [INT],
    //pattern_color: 16005 [INT],
    //text_color: 12773375 [INT],

    //name: "Mint Green" [STRING],
    //center_color: 8309634 [INT],
    //edge_color: 4562522 [INT],
    //pattern_color: 158498 [INT],
    //text_color: 12451788 [INT],

	//name: "Pure Gold" [STRING],
	//center_color: 13413185 [INT],
	//edge_color: 9993010 [INT],
	//pattern_color: 7355392 [INT],
	//text_color: 16770475 [INT],

	//name: "Orange" [STRING],
	//center_color: 13736506 [INT],
	//edge_color: 12611399 [INT],
	//pattern_color: 10303751 [INT],
	//text_color: 16769475 [INT],

	//name: "Strawberry" [STRING],
	//center_color: 14519919 [INT],
	//edge_color: 12016224 [INT],
	//pattern_color: 11078668 [INT],
	//text_color: 16765907 [INT],

	//name: "Steel Grey" [STRING],
	//center_color: 9937580 [INT],
	//edge_color: 6517372 [INT],
	//pattern_color: 3360082 [INT],
	//text_color: 14673128 [INT],

	const auto list = std::vector<StarsColoring>{
		{ 11431086, 8669060, 50, 1, 60, 1 },
		{ 4569325, 3704537, 100, 1, 80, 2 },
		{ 8309634, 4562522, 250, 5, 110, 3 },
		{ 13413185, 9993010, 500, 10, 150, 4 },
		{ 13736506, 12611399, 2000, 15, 200, 7 },
		{ 14519919, 12016224, 7500, 30, 280, 10 },
		{ 9937580, 6517372, 0, 60, 400, 20 },
	};
	for (const auto &entry : list) {
		if (!entry.tillStars || stars < entry.tillStars) {
			return entry;
		}
	}
	Unexpected("StarsColoringForCount: should not reach here.");
}

object_ptr<Ui::RpWidget> VideoStreamStarsLevel(
		not_null<Ui::RpWidget*> box,
		rpl::producer<int> starsValue) {
	auto result = object_ptr<Ui::RpWidget>(box.get());
	const auto raw = result.data();

	struct State {
		rpl::variable<int> stars;
		rpl::variable<StarsColoring> coloring;
		std::vector<not_null<Ui::RpWidget*>> blocks;
	};
	const auto state = raw->lifetime().make_state<State>();
	state->stars = std::move(starsValue);
	state->coloring = state->stars.value(
	) | rpl::map([](int stars) {
		return StarsColoringForCount(stars);
	});

	state->blocks.push_back(MakeInfoBlock(raw, state->coloring.value(
	) | rpl::map([=](const StarsColoring &value) {
		const auto minutes = value.minutesPin;
		return (minutes >= 60)
			? tr::lng_hours_tiny(tr::now, lt_count, minutes / 60)
			: tr::lng_minutes_tiny(tr::now, lt_count, minutes);
	}), tr::lng_paid_comment_pin_about()));

	state->blocks.push_back(MakeInfoBlock(raw, state->coloring.value(
	) | rpl::map([=](const StarsColoring &value) {
		return QString::number(value.charactersMax);
	}), state->coloring.value() | rpl::map([=](const StarsColoring &value) {
		return tr::lng_paid_comment_limit_about(
			tr::now,
			lt_count,
			value.charactersMax);
	})));

	state->blocks.push_back(MakeInfoBlock(raw, state->coloring.value(
	) | rpl::map([=](const StarsColoring &value) {
		return QString::number(value.emojiLimit);
	}), state->coloring.value() | rpl::map([=](const StarsColoring &value) {
		return tr::lng_paid_comment_emoji_about(
			tr::now,
			lt_count,
			value.emojiLimit);
	})));

	raw->resize(raw->width(), state->blocks.front()->height());
	raw->widthValue() | rpl::start_with_next([=](int width) {
		const auto count = int(state->blocks.size());
		const auto skip = (st::boxRowPadding.left() / 2);
		const auto single = (width - skip * (count - 1)) / float64(count);
		if (single < 1.) {
			return;
		}
		auto x = 0.;
		const auto w = int(base::SafeRound(single));
		for (const auto &block : state->blocks) {
			block->resizeToWidth(w);
			block->moveToLeft(int(base::SafeRound(x)), 0);
			x += single + skip;
		}
	}, raw->lifetime());

	return result;
}

} // namespace Calls::Group::Ui
