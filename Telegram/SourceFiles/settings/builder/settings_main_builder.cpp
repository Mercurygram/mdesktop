/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "settings/builder/settings_main_builder.h"

#include "ui/layers/generic_box.h"
#include "settings/builder/settings_builder.h"
#include "settings/settings_advanced.h"
#include "settings/settings_calls.h"
#include "settings/settings_chat.h"
#include "settings/settings_folders.h"
#include "settings/settings_information.h"
#include "settings/settings_notifications.h"
#include "settings/settings_power_saving.h"
#include "settings/settings_privacy_security.h"
#include "boxes/language_box.h"
#include "lang/lang_instance.h"
#include "lang/lang_keys.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"

namespace Settings::Builder {
namespace {

void BuildMainSectionContent(
		SectionBuilder &builder,
		Window::SessionController *controller) {
	builder.addDivider();
	builder.addSkip();

	builder.addSectionButton({
		.id = u"main/account"_q,
		.title = tr::lng_settings_my_account(),
		.targetSection = Information::Id(),
		.icon = { &st::menuIconProfile },
		.keywords = { u"profile"_q, u"edit"_q, u"information"_q },
	});

	builder.addSectionButton({
		.id = u"main/notifications"_q,
		.title = tr::lng_settings_section_notify(),
		.targetSection = Notifications::Id(),
		.icon = { &st::menuIconNotifications },
		.keywords = { u"alerts"_q, u"sounds"_q, u"badge"_q },
	});

	builder.addSectionButton({
		.id = u"main/privacy"_q,
		.title = tr::lng_settings_section_privacy(),
		.targetSection = PrivacySecurity::Id(),
		.icon = { &st::menuIconLock },
		.keywords = { u"security"_q, u"passcode"_q, u"password"_q, u"2fa"_q },
	});

	builder.addSectionButton({
		.id = u"main/chat"_q,
		.title = tr::lng_settings_section_chat_settings(),
		.targetSection = Chat::Id(),
		.icon = { &st::menuIconChatBubble },
		.keywords = { u"themes"_q, u"appearance"_q, u"stickers"_q },
	});

	builder.addSectionButton({
		.id = u"main/folders"_q,
		.title = tr::lng_settings_section_filters(),
		.targetSection = Folders::Id(),
		.icon = { &st::menuIconShowInFolder },
		.keywords = { u"filters"_q, u"tabs"_q },
	});

	builder.addSectionButton({
		.id = u"main/advanced"_q,
		.title = tr::lng_settings_advanced(),
		.targetSection = Advanced::Id(),
		.icon = { &st::menuIconManage },
		.keywords = { u"performance"_q, u"proxy"_q, u"experimental"_q },
	});

	builder.addSectionButton({
		.id = u"main/devices"_q,
		.title = tr::lng_settings_section_devices(),
		.targetSection = Calls::Id(),
		.icon = { &st::menuIconUnmute },
		.keywords = { u"sessions"_q, u"calls"_q },
	});

	const auto window = controller ? &controller->window() : nullptr;
	builder.addSettingsButton({
		.id = u"main/power"_q,
		.title = tr::lng_settings_power_menu(),
		.icon = { &st::menuIconPowerUsage },
		.onClick = window
			? [=] { window->show(Box(PowerSavingBox)); }
			: Fn<void()>(nullptr),
		.keywords = { u"battery"_q, u"animations"_q, u"power"_q, u"saving"_q },
	});

	builder.addLabeledButton({
		.id = u"main/language"_q,
		.title = tr::lng_settings_language(),
		.icon = { &st::menuIconTranslate },
		.label = rpl::single(
			Lang::GetInstance().id()
		) | rpl::then(
			Lang::GetInstance().idChanges()
		) | rpl::map([] { return Lang::GetInstance().nativeName(); }),
		.onClick = controller
			? [=] { LanguageBox::Show(controller); }
			: Fn<void()>(nullptr),
		.keywords = { u"translate"_q, u"localization"_q },
	});

	builder.addSkip();
}

} // namespace

void BuildMainSection(
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*> controller,
		Fn<void(Type)> showOther) {
	auto builder = SectionBuilder(WidgetContext{
		.container = container,
		.controller = controller,
		.showOther = std::move(showOther),
	});
	BuildMainSectionContent(builder, controller);
}

std::vector<SearchEntry> BuildMainSectionForSearch() {
	auto entries = std::vector<SearchEntry>();
	auto builder = SectionBuilder(SearchContext{
		.entries = &entries,
	});
	BuildMainSectionContent(builder, nullptr);
	return entries;
}

} // namespace Settings::Builder
