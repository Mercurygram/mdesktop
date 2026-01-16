/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Data {
struct UniqueGift;
class SavedStarGiftId;
} // namespace Data

namespace Window {
class SessionController;
} // namespace Window

namespace Ui {

void ShowGiftCraftInfoBox(
	not_null<Window::SessionController*> controller,
	std::shared_ptr<Data::UniqueGift> gift,
	Data::SavedStarGiftId savedId);

} // namespace Ui
