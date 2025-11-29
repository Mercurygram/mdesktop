/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "settings/settings_common_session.h"

namespace Ui {
class GenericBox;
} // namespace Ui

namespace Settings {

void PasskeysNoneBox(
	not_null<Ui::GenericBox*> box,
	not_null<Main::Session*> session);

class Passkeys : public Section<Passkeys> {
public:
	Passkeys(
		QWidget *parent,
		not_null<Window::SessionController*> controller);

	[[nodiscard]] rpl::producer<QString> title() override;

private:
	void setupContent(not_null<Window::SessionController*> controller);

};

} // namespace Settings
