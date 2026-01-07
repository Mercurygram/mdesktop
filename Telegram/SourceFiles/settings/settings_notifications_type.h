/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "settings/settings_common_session.h"
#include "data/notify/data_notify_settings.h"

namespace Settings {

class NotificationsType : public AbstractSection {
public:
	NotificationsType(
		QWidget *parent,
		not_null<Window::SessionController*> controller,
		Data::DefaultNotify type);

	[[nodiscard]] rpl::producer<QString> title() override;

	[[nodiscard]] static Type Id(Data::DefaultNotify type);

	[[nodiscard]] Type id() const final override {
		return Id(_type);
	}

	void showFinished() override;

private:
	void setupContent(not_null<Window::SessionController*> controller);

	const not_null<Window::SessionController*> _controller;
	const Data::DefaultNotify _type;

	QPointer<Ui::RpWidget> _showToggle;
	QPointer<Ui::RpWidget> _soundToggle;
	QPointer<Ui::RpWidget> _addException;
	QPointer<Ui::RpWidget> _deleteExceptions;

};

[[nodiscard]] bool NotificationsEnabledForType(
	not_null<::Main::Session*> session,
	Data::DefaultNotify type);

[[nodiscard]] rpl::producer<bool> NotificationsEnabledForTypeValue(
	not_null<::Main::Session*> session,
	Data::DefaultNotify type);

} // namespace Settings
