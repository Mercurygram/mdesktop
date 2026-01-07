/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "settings/settings_type.h"
#include "settings/builder/settings_builder.h"

namespace Ui {
class VerticalLayout;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace Settings::Builder {

void NotificationsSection(
	not_null<Ui::VerticalLayout*> container,
	not_null<Window::SessionController*> controller,
	Fn<void(Type)> showOther,
	rpl::producer<> showFinished);

[[nodiscard]] std::vector<SearchEntry> NotificationsSectionForSearch();

} // namespace Settings::Builder
