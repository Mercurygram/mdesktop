/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "settings/settings_common_session.h"
#include "settings/settings_type.h"

namespace Ui {
class GenericBox;
class SettingsButton;
class VerticalLayout;
} // namespace Ui

namespace Main {
class Session;
} // namespace Main

namespace Settings {

class Passkeys : public Section<Passkeys> {
public:
	Passkeys(
		QWidget *parent,
		not_null<Window::SessionController*> controller);

	void showFinished() override;

	[[nodiscard]] rpl::producer<QString> title() override;

	const Ui::RoundRect *bottomSkipRounding() const override {
		return &_bottomSkipRounding;
	}

private:
	void setupContent();

	const not_null<Ui::VerticalLayout*> _container;

	QPointer<Ui::SettingsButton> _addButton;
	Ui::RoundRect _bottomSkipRounding;

	rpl::event_stream<> _showFinished;

};

void PasskeysNoneBox(
	not_null<Ui::GenericBox*> box,
	not_null<::Main::Session*> session);

Type PasskeysId();

namespace Builder {

extern SectionBuildMethod PasskeysSection;

} // namespace Builder
} // namespace Settings
