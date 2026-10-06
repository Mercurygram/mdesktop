/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "settings/sections/settings_mercurygram.h"

#include "settings/settings_common_session.h"

#include "core/mg_settings.h"
#include "data/data_chat_filters.h"
#include "data/data_session.h"
#include "lang/lang_keys.h"
#include "lang/mg_mozhi_provider.h"
#include "main/main_session.h"
#include "settings/settings_builder.h"
#include "settings/sections/settings_main.h"
#include "ui/layers/generic_box.h"
#include "ui/text/text_utilities.h"
#include "ui/ui_utility.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/popup_menu.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"

namespace Settings {
namespace {

using namespace Builder;

// A row whose label shows the current launch folder and whose click opens a
// popup listing the account's folders. The stored FilterId is global, not
// per-account, so an id from another account reads as the default folder.
void AddLaunchFolderRow(SectionBuilder &builder) {
	const auto data = &builder.session()->data();
	const auto nameOf = [=](FilterId id) {
		const auto &list = data->chatsFilters().list();
		const auto i = ranges::find(list, id, &Data::ChatFilter::id);
		return (id && i != end(list))
			? i->titleText().text
			: tr::lng_filters_all(tr::now);
	};
	const auto button = builder.addButton({
		.id = u"mercurygram/launch_folder"_q,
		.title = tr::lng_mg_launch_folder(),
		.st = &st::settingsButtonNoIcon,
		.label = MG::LaunchFolderValue(
		) | rpl::map([=](int id) { return nameOf(FilterId(id)); }),
		.keywords = { u"launch"_q, u"folder"_q, u"startup"_q },
	});
	if (!button) {
		return;
	}
	const auto menu = button->lifetime().make_state<
		base::unique_qptr<Ui::PopupMenu>>();
	button->setClickedCallback([=] {
		*menu = base::make_unique_q<Ui::PopupMenu>(button);
		(*menu)->addAction(
			tr::lng_filters_all(tr::now),
			[] { MG::SetLaunchFolder(0); });
		for (const auto &filter : data->chatsFilters().list()) {
			if (const auto id = filter.id()) {
				(*menu)->addAction(
					Ui::Text::FixAmpersandInAction(filter.titleText().text),
					[=] { MG::SetLaunchFolder(id); });
			}
		}
		(*menu)->popup(button->mapToGlobal(QPoint(0, button->height())));
	});
}

void MozhiInstanceBox(not_null<Ui::GenericBox*> box) {
	box->setTitle(tr::lng_mg_translate_instance());
	const auto field = box->addRow(object_ptr<Ui::InputField>(
		box,
		st::defaultInputField,
		rpl::single(u"https://your-mozhi-instance.example"_q),
		MG::MozhiInstance()));
	box->setFocusCallback([=] {
		field->setFocusFast();
	});
	const auto submit = [=] {
		auto value = field->getLastText().trimmed();
		while (value.endsWith('/')) {
			value.chop(1);
		}
		const auto url = QUrl(value);
		const auto scheme = url.scheme();
		if (!value.isEmpty()
			&& (!url.isValid()
				|| url.host().isEmpty()
				|| (scheme != u"https"_q && scheme != u"http"_q))) {
			field->showError();
			return;
		}
		MG::SetMozhiInstance(value);
		box->closeBox();
	};
	field->submits() | rpl::on_next(submit, field->lifetime());
	box->addButton(tr::lng_settings_save(), submit);
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

void AddMozhiEngineRow(SectionBuilder &builder) {
	const auto button = builder.addButton({
		.id = u"mercurygram/translate_engine"_q,
		.title = tr::lng_mg_translate_engine(),
		.st = &st::settingsButtonNoIcon,
		.label = MG::MozhiEngineValue() | rpl::map([](int index) {
			const auto names = MG::MozhiEngineNames();
			return names.value(index, names.front());
		}),
		.keywords = { u"translate"_q, u"engine"_q, u"mozhi"_q },
	});
	if (!button) {
		return;
	}
	const auto menu = button->lifetime().make_state<
		base::unique_qptr<Ui::PopupMenu>>();
	button->setClickedCallback([=] {
		*menu = base::make_unique_q<Ui::PopupMenu>(button);
		const auto names = MG::MozhiEngineNames();
		for (auto i = 0; i != names.size(); ++i) {
			(*menu)->addAction(names[i], [=] { MG::SetMozhiEngine(i); });
		}
		(*menu)->popup(button->mapToGlobal(QPoint(0, button->height())));
	});
}

void AddMozhiInstanceRow(SectionBuilder &builder) {
	const auto controller = builder.controller();
	const auto button = builder.addButton({
		.id = u"mercurygram/translate_instance"_q,
		.title = tr::lng_mg_translate_instance(),
		.st = &st::settingsButtonNoIcon,
		.label = rpl::combine(
			MG::MozhiInstanceValue(),
			tr::lng_mg_translate_instance_auto()
		) | rpl::map([](const QString &url, const QString &automatic) {
			return url.isEmpty() ? automatic : QUrl(url).host();
		}),
		.keywords = { u"translate"_q, u"instance"_q, u"mozhi"_q },
	});
	if (!button || !controller) {
		return;
	}
	button->setClickedCallback([=] {
		controller->show(Box(MozhiInstanceBox));
	});
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
		u"mercurygram/show_char_counter"_q,
		tr::lng_mg_show_char_counter(),
		{ u"characters"_q, u"counter"_q, u"length"_q },
		MG::ShowCharCounter,
		MG::SetShowCharCounter);
	AddBoolToggle(
		builder,
		u"mercurygram/hide_all_chats"_q,
		tr::lng_mg_hide_all_chats(),
		{ u"all"_q, u"chats"_q, u"folder"_q, u"tab"_q },
		MG::HideAllChats,
		MG::SetHideAllChats);
	AddBoolToggle(
		builder,
		u"mercurygram/hide_premium_promo"_q,
		tr::lng_mg_hide_premium_promo(),
		{ u"premium"_q, u"promo"_q, u"business"_q, u"gift"_q },
		MG::HidePremiumPromo,
		MG::SetHidePremiumPromo);
	AddLaunchFolderRow(builder);

	builder.addSkip(st::settingsCheckboxesSkip);
}

void BuildMediaSection(SectionBuilder &builder) {
	builder.addDivider();
	builder.addSkip();
	builder.addSubsectionTitle(tr::lng_mg_media());

	AddBoolToggle(
		builder,
		u"mercurygram/all_recent_stickers"_q,
		tr::lng_mg_all_recent_stickers(),
		{ u"stickers"_q, u"recent"_q, u"picker"_q },
		MG::AllRecentStickers,
		MG::SetAllRecentStickers);

	builder.addSkip(st::settingsCheckboxesSkip);
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
	AddBoolToggle(
		builder,
		u"mercurygram/keep_drafts_local"_q,
		tr::lng_mg_keep_drafts_local(),
		{ u"draft"_q, u"drafts"_q, u"sync"_q, u"privacy"_q },
		MG::KeepDraftsLocal,
		MG::SetKeepDraftsLocal);
	AddBoolToggle(
		builder,
		u"mercurygram/reduce_tracking"_q,
		tr::lng_mg_reduce_tracking(),
		{ u"tracking"_q, u"network"_q, u"key"_q, u"privacy"_q },
		MG::ReduceTracking,
		[](bool value) {
			if (value) {
				MG::ResetTemporaryKeyLadder();
			}
			MG::SetReduceTracking(value);
		});
	AddBoolToggle(
		builder,
		u"mercurygram/strip_tracking"_q,
		tr::lng_mg_strip_tracking(),
		{ u"tracking"_q, u"utm"_q, u"link"_q, u"privacy"_q },
		MG::StripTracking,
		MG::SetStripTracking);
	AddBoolToggle(
		builder,
		u"mercurygram/confirm_internal_links"_q,
		tr::lng_mg_confirm_internal_links(),
		{ u"link"_q, u"telegram"_q, u"confirm"_q, u"privacy"_q },
		MG::ConfirmInternalLinks,
		MG::SetConfirmInternalLinks);
	AddBoolToggle(
		builder,
		u"mercurygram/prefer_secret_chats"_q,
		tr::lng_mg_prefer_secret_chats(),
		{ u"secret"_q, u"chat"_q, u"encrypted"_q, u"privacy"_q },
		MG::PreferSecretChats,
		MG::SetPreferSecretChats);
	AddBoolToggle(
		builder,
		u"mercurygram/keep_deleted_messages"_q,
		tr::lng_mg_keep_deleted_messages(),
		{ u"deleted"_q, u"edited"_q, u"history"_q, u"anti"_q },
		MG::KeepDeletedMessages,
		MG::SetKeepDeletedMessages);

	builder.addSkip(st::settingsCheckboxesSkip);

	// Footer only when the server refused one-hour keys.
	const auto refused = [](TimeId expiresIn) {
		return MG::ReduceTracking() && (expiresIn > 3600);
	};
	builder.scope([&] {
		builder.addDividerText(MG::TemporaryKeyExpiresInValue(
		) | rpl::filter(refused) | rpl::map([](TimeId expiresIn) {
			return tr::lng_mg_reduce_tracking_fallback(
				tr::now,
				lt_hours,
				tr::lng_hours(tr::now, lt_count, expiresIn / 3600));
		}));
	}, MG::TemporaryKeyExpiresInValue() | rpl::map(refused));
}

void BuildTranslationSection(SectionBuilder &builder) {
	builder.addDivider();
	builder.addSkip();
	builder.addSubsectionTitle(tr::lng_mg_translation());

	AddBoolToggle(
		builder,
		u"mercurygram/translate_mozhi"_q,
		tr::lng_mg_translate_mozhi(),
		{ u"translate"_q, u"translation"_q, u"mozhi"_q, u"privacy"_q },
		MG::MozhiTranslation,
		MG::SetMozhiTranslation);
	builder.scope([&] {
		AddMozhiEngineRow(builder);
		AddMozhiInstanceRow(builder);
	}, MG::MozhiTranslationValue());

	builder.addSkip(st::settingsCheckboxesSkip);
	builder.addDividerText(tr::lng_mg_translate_mozhi_about());
}

void BuildMercurygramSectionContent(SectionBuilder &builder) {
	BuildGeneralSection(builder);
	BuildMediaSection(builder);
	BuildPrivacySection(builder);
	BuildTranslationSection(builder);
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

void AddBoolToggle(
		Builder::SectionBuilder &builder,
		const QString &id,
		rpl::producer<QString> title,
		QStringList keywords,
		Fn<bool()> getter,
		Fn<void(bool)> setter,
		IconDescriptor icon) {
	const auto button = builder.addButton({
		.id = id,
		.title = std::move(title),
		.st = icon ? nullptr : &st::settingsButtonNoIcon,
		.icon = std::move(icon),
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

} // namespace Settings
