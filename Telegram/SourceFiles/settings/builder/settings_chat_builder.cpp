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
		builder.addSettingsButton({
			.id = u"appearance/theme"_q,
			.title = tr::lng_settings_themes(),
			.keywords = { u"theme"_q, u"color"_q, u"dark"_q, u"light"_q },
		});
		builder.addSettingsButton({
			.id = u"appearance/background"_q,
			.title = tr::lng_settings_bg_from_gallery(),
			.keywords = { u"background"_q, u"wallpaper"_q, u"image"_q },
		});
		builder.addSettingsButton({
			.id = u"appearance/quick_action"_q,
			.title = tr::lng_settings_quick_dialog_action_title(),
			.keywords = { u"swipe"_q, u"quick"_q, u"action"_q },
		});
		builder.addSettingsButton({
			.id = u"appearance/stickers"_q,
			.title = tr::lng_stickers_you_have(),
			.keywords = { u"stickers"_q, u"packs"_q },
		});
		builder.addSettingsButton({
			.id = u"appearance/emoji_sets"_q,
			.title = tr::lng_emoji_manage_sets(),
			.keywords = { u"emoji"_q, u"sets"_q, u"packs"_q },
		});
		builder.addCheckbox({
			.id = u"appearance/large_emoji"_q,
			.title = tr::lng_settings_large_emoji(),
			.checked = true,
			.keywords = { u"emoji"_q, u"large"_q, u"big"_q },
		});
		builder.addCheckbox({
			.id = u"appearance/replace_emoji"_q,
			.title = tr::lng_settings_replace_emojis(),
			.checked = true,
			.keywords = { u"emoji"_q, u"replace"_q, u"convert"_q },
		});
		builder.addCheckbox({
			.id = u"appearance/suggest_emoji"_q,
			.title = tr::lng_settings_suggest_emoji(),
			.checked = true,
			.keywords = { u"emoji"_q, u"suggest"_q, u"autocomplete"_q },
		});
		builder.addCheckbox({
			.id = u"appearance/suggest_stickers"_q,
			.title = tr::lng_settings_suggest_by_emoji(),
			.checked = true,
			.keywords = { u"stickers"_q, u"emoji"_q, u"suggest"_q },
		});
		builder.addCheckbox({
			.id = u"appearance/loop_stickers"_q,
			.title = tr::lng_settings_loop_stickers(),
			.checked = true,
			.keywords = { u"stickers"_q, u"loop"_q, u"animate"_q },
		});
		builder.addSettingsButton({
			.id = u"appearance/send_by_enter"_q,
			.title = tr::lng_settings_send_enter(),
			.keywords = { u"send"_q, u"enter"_q, u"message"_q },
		});
		builder.addToggle({
			.id = u"appearance/sensitive"_q,
			.title = tr::lng_settings_sensitive_disable_filtering(),
			.toggled = rpl::single(false),
			.keywords = { u"sensitive"_q, u"nsfw"_q, u"adult"_q },
		});
		builder.addSettingsButton({
			.id = u"data/shortcuts"_q,
			.title = tr::lng_settings_shortcuts(),
			.keywords = { u"shortcuts"_q, u"keyboard"_q, u"hotkeys"_q },
		});
		builder.addSettingsButton({
			.id = u"data/archive"_q,
			.title = tr::lng_context_archive_settings(),
			.keywords = { u"archive"_q, u"settings"_q },
		});
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

} // namespace

void ChatSection(
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*> controller,
		Fn<void(Type)> showOther,
		rpl::producer<> showFinished) {
	auto &lifetime = container->lifetime();
	const auto highlights = lifetime.make_state<HighlightRegistry>();

	SectionBuilder builder(WidgetContext{
		.container = container,
		.controller = controller,
		.showOther = showOther,
		.isPaused = [] { return false; },
		.highlights = highlights,
	});

	BuildChatSectionContent(builder, controller, showOther, highlights);

	std::move(showFinished) | rpl::on_next([=] {
		for (const auto &[id, entry] : *highlights) {
			if (entry.widget) {
				controller->checkHighlightControl(
					id,
					entry.widget,
					base::duplicate(entry.args));
			}
		}
	}, lifetime);
}

std::vector<SearchEntry> ChatSectionForSearch() {
	std::vector<SearchEntry> entries;
	SectionBuilder builder(SearchContext{ .entries = &entries });
	BuildChatSectionContent(builder, nullptr, nullptr, nullptr);
	return entries;
}

} // namespace Settings::Builder
