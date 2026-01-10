/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "settings/settings_common_session.h"

namespace Settings {

class Folders : public Section<Folders> {
public:
	Folders(
		QWidget *parent,
		not_null<Window::SessionController*> controller);
	~Folders();

	void showFinished() override;

	[[nodiscard]] rpl::producer<QString> title() override;

private:
	void setupContent();

	Fn<void()> _save;

	rpl::event_stream<> _showFinished;
	QPointer<Ui::RpWidget> _createButton;
	QPointer<Ui::RpWidget> _tagsButton;
	QPointer<Ui::RpWidget> _viewSection;
	QPointer<Ui::RpWidget> _recommendedTitle;

};

} // namespace Settings

