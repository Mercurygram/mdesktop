/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/object_ptr.h"
#include "settings/settings_common.h"
#include "settings/settings_type.h"

#include <variant>

namespace Ui {
class RpWidget;
class VerticalLayout;
class SettingsButton;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace style {
struct SettingsButton;
} // namespace style

namespace Settings::Builder {

struct SearchEntry {
	QString id;
	QString title;
	QStringList keywords;
};

struct WidgetContext {
	not_null<Ui::VerticalLayout*> container;
	not_null<Window::SessionController*> controller;
	Fn<void(Type)> showOther;
};

struct SearchContext {
	not_null<std::vector<SearchEntry>*> entries;
};

using BuildContext = std::variant<WidgetContext, SearchContext>;

class SectionBuilder {
public:
	explicit SectionBuilder(BuildContext context);

	struct ControlArgs {
		Fn<object_ptr<Ui::RpWidget>(not_null<Ui::VerticalLayout*>)> factory;
		QString id;
		rpl::producer<QString> title;
		QStringList keywords;
		style::margins margin;
	};
	Ui::RpWidget *addControl(ControlArgs &&args);

	struct ButtonArgs {
		QString id;
		rpl::producer<QString> title;
		const style::SettingsButton *st = nullptr;
		IconDescriptor icon;
		rpl::producer<QString> label;
		Fn<void()> onClick;
		QStringList keywords;
	};
	Ui::SettingsButton *addSettingsButton(ButtonArgs &&args);
	Ui::SettingsButton *addLabeledButton(ButtonArgs &&args);

	struct SectionArgs {
		QString id;
		rpl::producer<QString> title;
		Type targetSection;
		IconDescriptor icon;
		QStringList keywords;
	};
	Ui::SettingsButton *addSectionButton(SectionArgs &&args);

	void addDivider();
	void addSkip();

private:
	BuildContext _context;
};

} // namespace Settings::Builder
