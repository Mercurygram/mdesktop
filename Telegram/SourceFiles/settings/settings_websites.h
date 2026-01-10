/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "settings/settings_common_session.h"

namespace Ui {
class RpWidget;
} // namespace Ui

namespace Settings {

class Websites : public Section<Websites> {
public:
	Websites(
		QWidget *parent,
		not_null<Window::SessionController*> controller);

	[[nodiscard]] rpl::producer<QString> title() override;
	void showFinished() override;

private:
	void setupContent();

	QPointer<Ui::RpWidget> _terminateAll;

};

} // namespace Settings
