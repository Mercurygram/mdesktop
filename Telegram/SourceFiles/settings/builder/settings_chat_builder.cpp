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
#include "main/main_session_settings.h"
#include "settings/builder/settings_builder.h"
#include "settings/sections/settings_advanced.h"
#include "settings/settings_chat.h"
#include "settings/settings_experimental.h"
#include "settings/sections/settings_main.h"
#include "settings/settings_privacy_security.h"
#include "settings/settings_shortcuts.h"
#include "support/support_common.h"
#include "ui/layers/generic_box.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"

namespace Settings::Builder {
namespace {

void BuildThemeOptionsSection(SectionBuilder &builder) {
	const auto controller = builder.controller();
	const auto highlights = builder.highlights();

	builder.add([controller, highlights](const WidgetContext &ctx) {
		SetupThemeOptions(controller, ctx.container.get(), highlights);
		return SectionBuilder::WidgetToAdd{};
	}, [] {
		return SearchEntry{
			.id = u"chat/themes"_q,
			.title = tr::lng_settings_themes(tr::now),
			.keywords = { u"themes"_q, u"appearance"_q, u"dark"_q, u"light"_q },
			.icon = { &st::menuIconPalette },
		};
	});

	builder.add(nullptr, [] {
		return SearchEntry{
			.id = u"chat/themes-edit"_q,
			.title = tr::lng_settings_theme_accent_title(tr::now),
			.keywords = { u"accent"_q, u"color"_q, u"customize"_q },
		};
	});
}

void BuildThemeSettingsSection(SectionBuilder &builder) {
	const auto controller = builder.controller();
	const auto highlights = builder.highlights();

	builder.add([controller, highlights](const WidgetContext &ctx) {
		SetupThemeSettings(controller, ctx.container.get(), highlights);
		return SectionBuilder::WidgetToAdd{};
	}, [] {
		return SearchEntry{
			.id = u"chat/peer-color"_q,
			.title = tr::lng_settings_theme_settings(tr::now),
			.keywords = { u"color"_q, u"profile"_q, u"name"_q },
			.icon = { &st::menuIconChangeColors },
		};
	});

	builder.add(nullptr, [] {
		return SearchEntry{
			.id = u"chat/auto-night-mode"_q,
			.title = tr::lng_settings_auto_night_mode(tr::now),
			.keywords = { u"night"_q, u"dark"_q, u"auto"_q, u"system"_q },
			.icon = { &st::menuIconNightMode },
		};
	});

	builder.add(nullptr, [] {
		return SearchEntry{
			.id = u"chat/font"_q,
			.title = tr::lng_settings_font_family(tr::now),
			.keywords = { u"font"_q, u"family"_q, u"text"_q },
			.icon = { &st::menuIconFont },
		};
	});
}

void BuildCloudThemesSection(SectionBuilder &builder) {
	const auto controller = builder.controller();
	const auto highlights = builder.highlights();

	builder.add([controller, highlights](const WidgetContext &ctx) {
		SetupCloudThemes(controller, ctx.container.get(), highlights);
		return SectionBuilder::WidgetToAdd{};
	}, [] {
		return SearchEntry{
			.id = u"chat/cloud-themes"_q,
			.title = tr::lng_settings_bg_cloud_themes(tr::now),
			.keywords = { u"cloud"_q, u"themes"_q, u"online"_q },
		};
	});
}

void BuildChatBackgroundSection(SectionBuilder &builder) {
	const auto controller = builder.controller();
	const auto highlights = builder.highlights();

	builder.add([controller, highlights](const WidgetContext &ctx) {
		SetupChatBackground(controller, ctx.container.get(), highlights);
		return SectionBuilder::WidgetToAdd{};
	}, [] {
		return SearchEntry{
			.id = u"chat/wallpapers"_q,
			.title = tr::lng_settings_section_background(tr::now),
			.keywords = { u"background"_q, u"wallpaper"_q, u"image"_q },
			.icon = { &st::menuIconPhoto },
		};
	});

	builder.add(nullptr, [] {
		return SearchEntry{
			.id = u"chat/wallpapers-set"_q,
			.title = tr::lng_settings_bg_from_gallery(tr::now),
			.keywords = { u"gallery"_q, u"wallpaper"_q },
		};
	});

	builder.add(nullptr, [] {
		return SearchEntry{
			.id = u"chat/wallpapers-choose-photo"_q,
			.title = tr::lng_settings_bg_from_file(tr::now),
			.keywords = { u"file"_q, u"photo"_q, u"upload"_q },
		};
	});

	builder.add(nullptr, [] {
		return SearchEntry{
			.id = u"chat/adaptive-layout"_q,
			.title = tr::lng_settings_adaptive_wide(tr::now),
			.keywords = { u"adaptive"_q, u"wide"_q, u"layout"_q },
			.checkIcon = Core::App().settings().adaptiveForWide()
				? SearchEntryCheckIcon::Checked
				: SearchEntryCheckIcon::Unchecked,
		};
	});
}

void BuildChatListQuickActionSection(SectionBuilder &builder) {
	const auto controller = builder.controller();

	builder.add([controller](const WidgetContext &ctx) {
		SetupChatListQuickAction(controller, ctx.container.get());
		return SectionBuilder::WidgetToAdd{};
	}, [] {
		return SearchEntry{
			.id = u"chat/quick-dialog-action"_q,
			.title = tr::lng_settings_quick_dialog_action_title(tr::now),
			.keywords = { u"swipe"_q, u"quick"_q, u"action"_q, u"dialog"_q },
		};
	});
}

void BuildStickersEmojiSection(SectionBuilder &builder) {
	const auto controller = builder.controller();
	const auto highlights = builder.highlights();

	builder.add([controller, highlights](const WidgetContext &ctx) {
		SetupStickersEmoji(controller, ctx.container.get(), highlights);
		return SectionBuilder::WidgetToAdd{};
	}, [] {
		return SearchEntry{
			.id = u"chat/stickers-emoji"_q,
			.title = tr::lng_settings_stickers_emoji(tr::now),
			.keywords = { u"stickers"_q, u"emoji"_q },
			.icon = { &st::menuIconStickers },
		};
	});

	builder.add(nullptr, [] {
		return SearchEntry{
			.id = u"chat/large-emoji"_q,
			.title = tr::lng_settings_large_emoji(tr::now),
			.keywords = { u"large"_q, u"emoji"_q, u"big"_q },
			.checkIcon = Core::App().settings().largeEmoji()
				? SearchEntryCheckIcon::Checked
				: SearchEntryCheckIcon::Unchecked,
		};
	});

	builder.add(nullptr, [] {
		return SearchEntry{
			.id = u"chat/replace-emoji"_q,
			.title = tr::lng_settings_replace_emojis(tr::now),
			.keywords = { u"replace"_q, u"emoji"_q, u"convert"_q },
			.checkIcon = Core::App().settings().replaceEmoji()
				? SearchEntryCheckIcon::Checked
				: SearchEntryCheckIcon::Unchecked,
		};
	});

	builder.add(nullptr, [] {
		return SearchEntry{
			.id = u"chat/suggest-emoji"_q,
			.title = tr::lng_settings_suggest_emoji(tr::now),
			.keywords = { u"suggest"_q, u"emoji"_q, u"autocomplete"_q },
			.checkIcon = Core::App().settings().suggestEmoji()
				? SearchEntryCheckIcon::Checked
				: SearchEntryCheckIcon::Unchecked,
		};
	});

	builder.add(nullptr, [] {
		return SearchEntry{
			.id = u"chat/suggest-animated-emoji"_q,
			.title = tr::lng_settings_suggest_animated_emoji(tr::now),
			.keywords = { u"animated"_q, u"emoji"_q, u"premium"_q },
			.checkIcon = Core::App().settings().suggestAnimatedEmoji()
				? SearchEntryCheckIcon::Checked
				: SearchEntryCheckIcon::Unchecked,
		};
	});

	builder.add(nullptr, [] {
		return SearchEntry{
			.id = u"chat/suggest-by-emoji"_q,
			.title = tr::lng_settings_suggest_by_emoji(tr::now),
			.keywords = { u"suggest"_q, u"stickers"_q, u"emoji"_q },
			.checkIcon = Core::App().settings().suggestStickersByEmoji()
				? SearchEntryCheckIcon::Checked
				: SearchEntryCheckIcon::Unchecked,
		};
	});

	builder.add(nullptr, [] {
		return SearchEntry{
			.id = u"chat/loop-stickers"_q,
			.title = tr::lng_settings_loop_stickers(tr::now),
			.keywords = { u"loop"_q, u"stickers"_q, u"animated"_q },
			.checkIcon = Core::App().settings().loopAnimatedStickers()
				? SearchEntryCheckIcon::Checked
				: SearchEntryCheckIcon::Unchecked,
		};
	});

	builder.add(nullptr, [] {
		return SearchEntry{
			.id = u"chat/my-stickers"_q,
			.title = tr::lng_stickers_you_have(tr::now),
			.keywords = { u"stickers"_q, u"manage"_q, u"installed"_q },
			.icon = { &st::menuIconStickers },
		};
	});

	builder.add(nullptr, [] {
		return SearchEntry{
			.id = u"chat/emoji-sets"_q,
			.title = tr::lng_emoji_manage_sets(tr::now),
			.keywords = { u"emoji"_q, u"sets"_q, u"manage"_q },
			.icon = { &st::menuIconEmoji },
		};
	});
}

void BuildMessagesSection(SectionBuilder &builder) {
	const auto controller = builder.controller();
	const auto highlights = builder.highlights();

	builder.add([controller, highlights](const WidgetContext &ctx) {
		SetupMessages(controller, ctx.container.get(), highlights);
		return SectionBuilder::WidgetToAdd{};
	}, [] {
		return SearchEntry{
			.id = u"chat/messages"_q,
			.title = tr::lng_settings_messages(tr::now),
			.keywords = { u"messages"_q, u"send"_q, u"enter"_q },
		};
	});

	builder.add(nullptr, [] {
		return SearchEntry{
			.id = u"chat/send-enter"_q,
			.title = tr::lng_settings_send_enter(tr::now),
			.keywords = { u"send"_q, u"enter"_q, u"keyboard"_q },
		};
	});

	builder.add(nullptr, [] {
		return SearchEntry{
			.id = u"chat/quick-reaction"_q,
			.title = tr::lng_settings_chat_quick_action_react(tr::now),
			.keywords = { u"quick"_q, u"reaction"_q, u"double"_q, u"click"_q },
		};
	});

	builder.add(nullptr, [] {
		return SearchEntry{
			.id = u"chat/corner-reaction"_q,
			.title = tr::lng_settings_chat_corner_reaction(tr::now),
			.keywords = { u"corner"_q, u"reaction"_q },
			.checkIcon = Core::App().settings().cornerReaction()
				? SearchEntryCheckIcon::Checked
				: SearchEntryCheckIcon::Unchecked,
		};
	});
}

void BuildSensitiveContentSection(SectionBuilder &builder) {
	const auto controller = builder.controller();
	const auto highlights = builder.highlights();

	builder.add([controller, highlights](const WidgetContext &ctx) {
		auto updateOnTick = rpl::single(
		) | rpl::then(base::timer_each(60 * crl::time(1000)));
		Ui::AddDivider(ctx.container.get());
		SetupSensitiveContent(
			controller,
			ctx.container.get(),
			std::move(updateOnTick),
			highlights);
		return SectionBuilder::WidgetToAdd{};
	}, [] {
		return SearchEntry{
			.id = u"chat/sensitive-content"_q,
			.title = tr::lng_settings_sensitive_title(tr::now),
			.keywords = { u"sensitive"_q, u"content"_q, u"nsfw"_q, u"adult"_q },
		};
	});
}

void BuildArchiveSection(SectionBuilder &builder) {
	const auto controller = builder.controller();
	const auto showOther = builder.showOther();
	const auto session = builder.session();

	builder.addSkip();

	builder.addButton({
		.id = u"chat/shortcuts"_q,
		.title = tr::lng_settings_shortcuts(),
		.icon = { &st::menuIconShortcut },
		.onClick = [showOther] { showOther(Shortcuts::Id()); },
		.keywords = { u"shortcuts"_q, u"keyboard"_q, u"hotkeys"_q },
	});

	if (controller) {
		PreloadArchiveSettings(session);
	}

	builder.addButton({
		.id = u"chat/archive-settings"_q,
		.title = tr::lng_context_archive_settings(),
		.icon = { &st::menuIconArchive },
		.onClick = [=] {
			if (controller) {
				controller->show(
					Box<Ui::GenericBox>(ArchiveSettingsBox, controller));
			}
		},
		.keywords = { u"archive"_q, u"settings"_q, u"folder"_q },
	});
}

void BuildSupportSection(SectionBuilder &builder) {
	const auto session = builder.session();
	if (!session->supportMode()) {
		return;
	}
	const auto controller = builder.controller();

	builder.addSkip();
	builder.addSubsectionTitle({
		.id = u"chat/support"_q,
		.title = rpl::single(u"Support settings"_q),
		.keywords = { u"support"_q },
	});
	builder.addSkip(st::settingsSendTypeSkip);

	using SwitchType = Support::SwitchSettings;

	builder.add([controller](const WidgetContext &ctx) {
		const auto container = ctx.container.get();
		auto wrap = object_ptr<Ui::VerticalLayout>(container);
		const auto inner = wrap.data();

		const auto group = std::make_shared<Ui::RadioenumGroup<SwitchType>>(
			controller->session().settings().supportSwitch());

		const auto addRadio = [&](SwitchType value, const QString &label) {
			inner->add(
				object_ptr<Ui::Radioenum<SwitchType>>(
					inner,
					group,
					value,
					label,
					st::settingsSendType),
				st::settingsSendTypePadding);
		};
		addRadio(SwitchType::None, "Just send the reply");
		addRadio(SwitchType::Next, "Send and switch to next");
		addRadio(SwitchType::Previous, "Send and switch to previous");

		group->setChangedCallback([=](SwitchType value) {
			controller->session().settings().setSupportSwitch(value);
			controller->session().saveSettingsDelayed();
		});

		return SectionBuilder::WidgetToAdd{ .widget = std::move(wrap) };
	}, [] {
		return SearchEntry{
			.id = u"chat/support/switch"_q,
			.title = u"Send and switch behavior"_q,
			.keywords = { u"switch"_q, u"next"_q, u"previous"_q, u"reply"_q },
		};
	});

	builder.addSkip(st::settingsCheckboxesSkip);

	const auto templatesAutocomplete = builder.addCheckbox({
		.id = u"chat/support/templates"_q,
		.title = rpl::single(u"Enable templates autocomplete"_q),
		.checked = session->settings().supportTemplatesAutocomplete(),
		.keywords = { u"templates"_q, u"autocomplete"_q },
	});
	if (templatesAutocomplete) {
		templatesAutocomplete->checkedChanges(
		) | rpl::on_next([=](bool checked) {
			controller->session().settings().setSupportTemplatesAutocomplete(
				checked);
			controller->session().saveSettingsDelayed();
		}, templatesAutocomplete->lifetime());
	}

	const auto allSilent = builder.addCheckbox({
		.id = u"chat/support/silent"_q,
		.title = rpl::single(u"Send all messages without sound"_q),
		.checked = session->settings().supportAllSilent(),
		.keywords = { u"silent"_q, u"sound"_q, u"mute"_q },
	});
	if (allSilent) {
		allSilent->checkedChanges(
		) | rpl::on_next([=](bool checked) {
			controller->session().settings().setSupportAllSilent(checked);
			controller->session().saveSettingsDelayed();
		}, allSilent->lifetime());
	}

	builder.addSkip(st::settingsCheckboxesSkip);
	builder.addSubsectionTitle({
		.id = u"chat/support/chats-period"_q,
		.title = rpl::single(u"Load chats for a period"_q),
		.keywords = { u"period"_q, u"days"_q },
	});

	builder.add([controller](const WidgetContext &ctx) {
		constexpr auto kDayDuration = 24 * 60 * 60;
		struct Option {
			int days = 0;
			QString label;
		};
		const auto options = std::vector<Option>{
			{ 1, "1 day" },
			{ 7, "1 week" },
			{ 30, "1 month" },
			{ 365, "1 year" },
			{ 0, "All of them" },
		};
		const auto current = controller->session().settings().supportChatsTimeSlice();
		const auto days = current / kDayDuration;
		const auto best = ranges::min_element(
			options,
			std::less<>(),
			[&](const Option &option) { return std::abs(option.days - days); });

		const auto container = ctx.container.get();
		auto wrap = object_ptr<Ui::VerticalLayout>(container);
		const auto inner = wrap.data();

		const auto group = std::make_shared<Ui::RadiobuttonGroup>(best->days);
		for (const auto &option : options) {
			inner->add(
				object_ptr<Ui::Radiobutton>(
					inner,
					group,
					option.days,
					option.label,
					st::settingsSendType),
				st::settingsSendTypePadding);
		}
		group->setChangedCallback([=](int days) {
			controller->session().settings().setSupportChatsTimeSlice(
				days * kDayDuration);
			controller->session().saveSettingsDelayed();
		});

		return SectionBuilder::WidgetToAdd{ .widget = std::move(wrap) };
	}, [] {
		return SearchEntry{
			.id = u"chat/support/chats-period/options"_q,
			.title = u"Chat loading period options"_q,
			.keywords = { u"week"_q, u"month"_q, u"year"_q },
		};
	});

	builder.addSkip(st::settingsCheckboxesSkip);
	builder.addSkip();
}

void BuildChatSectionContent(SectionBuilder &builder) {
	BuildThemeOptionsSection(builder);
	BuildThemeSettingsSection(builder);
	BuildCloudThemesSection(builder);
	BuildChatBackgroundSection(builder);
	BuildChatListQuickActionSection(builder);
	BuildStickersEmojiSection(builder);
	BuildMessagesSection(builder);
	BuildSensitiveContentSection(builder);
	BuildArchiveSection(builder);
	BuildSupportSection(builder);
}

const auto kMeta = BuildHelper({
	.id = Chat::Id(),
	.parentId = Main::Id(),
	.title = &tr::lng_settings_section_chat_settings,
	.icon = &st::menuIconChatBubble,
}, [](SectionBuilder &builder) {
	BuildChatSectionContent(builder);
});

} // namespace

SectionBuildMethod ChatSection = kMeta.build;

} // namespace Settings::Builder
