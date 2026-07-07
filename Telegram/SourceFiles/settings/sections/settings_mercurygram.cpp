/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "settings/sections/settings_mercurygram.h"

#include "settings/settings_common_session.h"

#include "core/mg_settings.h"
#include "lang/lang_keys.h"
#include "settings/settings_builder.h"
#include "settings/sections/settings_main.h"
#include "ui/ui_utility.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"

namespace Settings {
namespace {

using namespace Builder;

void AddBoolToggle(
		SectionBuilder &builder,
		const QString &id,
		rpl::producer<QString> title,
		QStringList keywords,
		Fn<bool()> getter,
		Fn<void(bool)> setter) {
	const auto button = builder.addButton({
		.id = id,
		.title = std::move(title),
		.st = &st::settingsButtonNoIcon,
		.toggled = rpl::single(getter()),
		.keywords = std::move(keywords),
	});
	if (button) {
		button->toggledValue(
		) | rpl::filter([=](bool checked) {
			return (checked != getter());
		}) | rpl::on_next([=](bool checked) {
			setter(checked);
		}, button->lifetime());
	}
}

void BuildGeneralSection(SectionBuilder &builder) {
	builder.addSkip();
	builder.addSubsectionTitle(tr::lng_mg_general());

	AddBoolToggle(
		builder,
		u"mercurygram/show_peer_id"_q,
		tr::lng_mg_show_peer_id(),
		{ u"id"_q, u"peer"_q, u"profile"_q },
		MG::ShowPeerId,
		MG::SetShowPeerId);
	AddBoolToggle(
		builder,
		u"mercurygram/hide_stories"_q,
		tr::lng_mg_hide_stories(),
		{ u"stories"_q },
		MG::HideStories,
		MG::SetHideStories);
	AddBoolToggle(
		builder,
		u"mercurygram/delete_for_all"_q,
		tr::lng_mg_delete_for_all(),
		{ u"delete"_q, u"revoke"_q, u"everyone"_q },
		MG::DeleteForAllDefault,
		MG::SetDeleteForAllDefault);
	AddBoolToggle(
		builder,
		u"mercurygram/message_details"_q,
		tr::lng_mg_message_details(),
		{ u"message"_q, u"details"_q },
		MG::MessageDetails,
		MG::SetMessageDetails);
	AddBoolToggle(
		builder,
		u"mercurygram/hide_all_chats"_q,
		tr::lng_mg_hide_all_chats(),
		{ u"all"_q, u"chats"_q, u"folder"_q, u"tab"_q },
		MG::HideAllChats,
		MG::SetHideAllChats);

	builder.addSkip(st::settingsCheckboxesSkip);
}

void BuildMediaSection(SectionBuilder &builder) {
	builder.addDivider();
	builder.addSkip();
	builder.addSubsectionTitle(tr::lng_mg_media());

	AddBoolToggle(
		builder,
		u"mercurygram/large_photos"_q,
		tr::lng_mg_large_photos(),
		{ u"photos"_q, u"quality"_q, u"large"_q },
		MG::LargePhotos,
		MG::SetLargePhotos);

	builder.addDividerText(tr::lng_mg_large_photos_about());
}

void BuildPrivacySection(SectionBuilder &builder) {
	builder.addDivider();
	builder.addSkip();
	builder.addSubsectionTitle(tr::lng_mg_privacy());

	AddBoolToggle(
		builder,
		u"mercurygram/disable_global_search"_q,
		tr::lng_mg_disable_global_search(),
		{ u"search"_q, u"global"_q, u"privacy"_q },
		MG::DisableGlobalSearch,
		MG::SetDisableGlobalSearch);
	AddBoolToggle(
		builder,
		u"mercurygram/disable_link_previews"_q,
		tr::lng_mg_disable_link_previews(),
		{ u"link"_q, u"preview"_q, u"webpage"_q, u"privacy"_q },
		MG::DisableLinkPreviews,
		MG::SetDisableLinkPreviews);
	AddBoolToggle(
		builder,
		u"mercurygram/disable_ai_editor"_q,
		tr::lng_mg_disable_ai_editor(),
		{ u"ai"_q, u"editor"_q, u"rewrite"_q, u"privacy"_q },
		MG::DisableAiEditor,
		MG::SetDisableAiEditor);
	AddBoolToggle(
		builder,
		u"mercurygram/disable_ai_summaries"_q,
		tr::lng_mg_disable_ai_summaries(),
		{ u"ai"_q, u"summary"_q, u"summaries"_q, u"privacy"_q },
		MG::DisableAiSummaries,
		MG::SetDisableAiSummaries);
	AddBoolToggle(
		builder,
		u"mercurygram/open_links_in_browser"_q,
		tr::lng_mg_open_links_in_browser(),
		{ u"link"_q, u"browser"_q, u"instant"_q, u"view"_q, u"privacy"_q },
		MG::OpenLinksInBrowser,
		MG::SetOpenLinksInBrowser);

	builder.addDividerText(tr::lng_mg_privacy_about());
}

void BuildMercurygramSectionContent(SectionBuilder &builder) {
	BuildGeneralSection(builder);
	BuildMediaSection(builder);
	BuildPrivacySection(builder);
}

class Mercurygram : public Section<Mercurygram> {
public:
	Mercurygram(
		QWidget *parent,
		not_null<Window::SessionController*> controller);

	[[nodiscard]] rpl::producer<QString> title() override;

private:
	void setupContent();

};

const auto kMeta = BuildHelper({
	.id = Mercurygram::Id(),
	.parentId = MainId(),
	.title = &tr::lng_mg_settings_title,
	.icon = &st::menuIconCustomize,
}, [](SectionBuilder &builder) {
	BuildMercurygramSectionContent(builder);
});

const SectionBuildMethod kMercurygramSection = kMeta.build;

Mercurygram::Mercurygram(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent, controller) {
	setupContent();
}

rpl::producer<QString> Mercurygram::title() {
	return tr::lng_mg_settings_title();
}

void Mercurygram::setupContent() {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);

	build(content, kMercurygramSection);

	Ui::ResizeFitChild(this, content);
}

} // namespace

Type MercurygramId() {
	return Mercurygram::Id();
}

} // namespace Settings
