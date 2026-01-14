/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "settings/settings_common_session.h"
#include "settings/settings_type.h"

namespace Main {
class SessionShow;
} // namespace Main

namespace Ui {
class RadiobuttonGroup;
class VerticalLayout;
class SettingsButton;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace Settings {

class GlobalTTL : public Section<GlobalTTL> {
public:
	GlobalTTL(
		QWidget *parent,
		not_null<Window::SessionController*> controller);

	[[nodiscard]] rpl::producer<QString> title() override;

private:
	void setupContent();
	void showFinished() override;
	void rebuildButtons(TimeId currentTTL) const;
	void showSure(TimeId ttl, bool rebuild) const;
	void request(TimeId ttl) const;

	const std::shared_ptr<Ui::RadiobuttonGroup> _group;
	const std::shared_ptr<::Main::SessionShow> _show;

	not_null<Ui::VerticalLayout*> _buttons;
	QPointer<Ui::SettingsButton> _customButton;

	rpl::event_stream<> _showFinished;
	rpl::lifetime _requestLifetime;

};

Type GlobalTTLId();

namespace Builder {

extern SectionBuildMethod GlobalTTLSection;

} // namespace Builder
} // namespace Settings
