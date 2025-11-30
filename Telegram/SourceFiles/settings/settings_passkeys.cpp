/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "settings/settings_passkeys.h"

#include "ui/vertical_list.h"
#include "ui/wrap/vertical_layout.h"
#include "lang/lang_keys.h"
#include "styles/style_settings.h"

namespace Settings {

Passkeys::Passkeys(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent) {
	setupContent(controller);
}

rpl::producer<QString> Passkeys::title() {
	return tr::lng_settings_passkeys_title();
}

void Passkeys::setupContent(
		not_null<Window::SessionController*> controller) {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);

	Ui::ResizeFitChild(this, content);
}

} // namespace Settings
