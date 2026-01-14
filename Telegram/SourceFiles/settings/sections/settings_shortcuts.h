/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "settings/settings_common_session.h"
#include "settings/settings_type.h"

namespace Settings {

class Shortcuts : public Section<Shortcuts> {
public:
	Shortcuts(
		QWidget *parent,
		not_null<Window::SessionController*> controller);
	~Shortcuts();

	void showFinished() override;

	[[nodiscard]] rpl::producer<QString> title() override;

private:
	void setupContent();

	Fn<void()> _save;
	QPointer<Ui::RpWidget> _resetButton;

};

namespace Builder {

extern SectionBuildMethod ShortcutsSection;

} // namespace Builder
} // namespace Settings
