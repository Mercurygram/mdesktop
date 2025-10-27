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
	const auto list = std::vector<StarsColoring>{
		{ 0x955CDB, 0x49079B, 0, 30, 30, 0 }, // purple
		{ 0x955CDB, 0x49079B, 10, 60, 60, 1 }, // still purple
		{ 0x46A3EB, 0x00508E, 50, 120, 80, 2 }, // blue
		{ 0x40A920, 0x176200, 100, 300, 110, 3 }, // green
		{ 0xE29A09, 0x9A3E00, 250, 600, 150, 4 }, // yellow
		{ 0xED771E, 0x9B3100, 500, 900, 200, 7 }, // orange
		{ 0xE14542, 0x8B0503, 2'000, 1800, 280, 10 }, // red
		{ 0x596473, 0x252C36, 10'000, 3600, 400, 20 }, // silver
	};
	for (auto i = begin(list), e = end(list); i != e; ++i) {
		if (i->fromStars > stars) {
			Assert(i != begin(list));
			return *(std::prev(i));
		}
	}
	return list.back();
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
		const auto seconds = value.secondsPin;
		return (seconds >= 3600)
			? tr::lng_hours_tiny(tr::now, lt_count, seconds / 3600)
			: (seconds >= 60)
			? tr::lng_minutes_tiny(tr::now, lt_count, seconds / 60)
			: tr::lng_seconds_tiny(tr::now, lt_count, seconds);
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
