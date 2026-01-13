/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "settings/builder/settings_chat_builder.h"

#include "base/timer_rpl.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "settings/builder/settings_builder.h"
#include "settings/settings_chat.h"
#include "settings/settings_experimental.h"
#include "settings/settings_main.h"
#include "settings/settings_privacy_security.h"
#include "settings/settings_shortcuts.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"

namespace Settings::Builder {
namespace {

void BuildChatSectionContent(
		SectionBuilder &builder,
		Window::SessionController *controller,
		Fn<void(Type)> showOther,
		HighlightRegistry *highlights) {
	if (!controller) {
		return;
	}

	const auto container = builder.container();

	auto updateOnTick = rpl::single(
	) | rpl::then(base::timer_each(60 * crl::time(1000)));

	SetupThemeOptions(controller, container, highlights);
	SetupThemeSettings(controller, container, highlights);
	SetupCloudThemes(controller, container, highlights);
	SetupChatBackground(controller, container, highlights);
	SetupChatListQuickAction(controller, container);
	SetupStickersEmoji(controller, container, highlights);
	SetupMessages(controller, container, highlights);
	Ui::AddDivider(container);
	SetupSensitiveContent(controller, container, std::move(updateOnTick), highlights);
	SetupArchive(controller, container, showOther);
}

const auto kMeta = BuildHelper(
	Chat::Id(),
	tr::lng_settings_section_chat_settings,
	[](SectionBuilder &builder) {
		const auto controller = builder.controller();
		const auto showOther = builder.showOther();
		const auto highlights = builder.highlights();
		BuildChatSectionContent(builder, controller, showOther, highlights);
	},
	Main::Id());

} // namespace

SectionBuildMethod ChatSection = kMeta.build;

} // namespace Settings::Builder
