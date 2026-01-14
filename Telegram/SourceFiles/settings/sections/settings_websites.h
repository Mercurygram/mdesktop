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
class RpWidget;
} // namespace Ui

namespace Settings {

class Websites : public Section<Websites> {
public:
	Websites(
		QWidget *parent,
		not_null<Window::SessionController*> controller);

	void showFinished() override;

	[[nodiscard]] rpl::producer<QString> title() override;

private:
	void setupContent();

	QPointer<Ui::RpWidget> _terminateAll;

};

namespace Builder {

extern SectionBuildMethod WebsitesSection;

} // namespace Builder
} // namespace Settings
