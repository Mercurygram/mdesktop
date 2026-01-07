/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "settings/settings_type.h"

#include <vector>

namespace Ui {
class RpWidget;
class SettingsButton;
class VerticalLayout;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace Settings::Builder {

struct SearchEntry;

void MainSection(
	not_null<Ui::VerticalLayout*> container,
	not_null<Window::SessionController*> controller,
	Fn<void(Type)> showOther,
	rpl::producer<> showFinished);

[[nodiscard]] std::vector<SearchEntry> MainSectionForSearch();

} // namespace Settings::Builder
