/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Ui {
class RpWidget;
} // namespace Ui

namespace Calls::Group::Ui {

using namespace ::Ui;

struct StarsColoring {
	int bg1 = 0;
	int bg2 = 0;
	int tillStars = 0;
	int minutesPin = 0;
	int charactersMax = 0;
	int emojiLimit = 0;

	friend inline auto operator<=>(
		const StarsColoring &,
		const StarsColoring &) = default;
	friend inline bool operator==(
		const StarsColoring &,
		const StarsColoring &) = default;
};

[[nodiscard]] StarsColoring StarsColoringForCount(int stars);

[[nodiscard]] object_ptr<Ui::RpWidget> VideoStreamStarsLevel(
	not_null<Ui::RpWidget*> box,
	rpl::producer<int> starsValue);

} // namespace Calls::Group::Ui
