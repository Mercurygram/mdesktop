/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace ChatHelpers {
class Show;
} // namespace ChatHelpers

namespace Ui {
class BoxContent;
class GenericBox;
} // namespace Ui

namespace Calls::Group {

struct VideoStreamStarsBoxArgs {
	std::shared_ptr<ChatHelpers::Show> show;
	int current = 0;
	Fn<void(int)> save;
	QString name;
};

void VideoStreamStarsBox(
	not_null<Ui::GenericBox*> box,
	VideoStreamStarsBoxArgs &&args);

[[nodiscard]] object_ptr<Ui::BoxContent> MakeVideoStreamStarsBox(
	VideoStreamStarsBoxArgs &&args);

} // namespace Calls::Group
