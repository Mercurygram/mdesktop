/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "settings/builder/settings_main_builder.h"

#include "api/api_credits.h"
#include "boxes/language_box.h"
#include "boxes/star_gift_box.h"
#include "core/click_handler_types.h"
#include "data/components/credits.h"
#include "data/data_chat_filters.h"
#include "data/data_session.h"
#include "lang/lang_instance.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_app_config.h"
#include "main/main_session.h"
#include "main/main_session_settings.h"
#include "settings/builder/settings_builder.h"
#include "settings/settings_advanced.h"
#include "settings/settings_business.h"
#include "settings/settings_calls.h"
#include "settings/settings_chat.h"
#include "settings/settings_credits.h"
#include "settings/settings_folders.h"
#include "settings/settings_information.h"
#include "settings/settings_main.h"
#include "settings/settings_notifications.h"
#include "settings/settings_power_saving.h"
#include "ui/power_saving.h"
#include "settings/settings_premium.h"
#include "settings/settings_privacy_security.h"
#include "ui/basic_click_handlers.h"
#include "ui/layers/generic_box.h"
#include "ui/new_badges.h"
#include "ui/text/format_values.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"

namespace Settings::Builder {
namespace {

void BuildSectionButtons(
		SectionBuilder &builder,
		Window::SessionController *controller) {
	if (!controller || !controller->session().supportMode()) {
		builder.addSectionButton({
			.id = u"main/account"_q,
			.title = tr::lng_settings_my_account(),
			.targetSection = Information::Id(),
			.icon = { &st::menuIconProfile },
			.keywords = { u"profile"_q, u"edit"_q, u"information"_q },
		});
	}

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

	const auto showOther = builder.showOther();
	if (controller) {
		const auto account = &controller->session().account();
		const auto preload = [=] {
			controller->session().data().chatsFilters().requestSuggested();
		};
		const auto hasFilters = controller->session().data().chatsFilters().has()
			|| controller->session().settings().dialogsFiltersEnabled();

		auto shownProducer = hasFilters
			? rpl::single(true)
			: (rpl::single(rpl::empty) | rpl::then(
				account->appConfig().refreshed()
			) | rpl::map([=] {
				const auto enabled = account->appConfig().get<bool>(
					u"dialog_filters_enabled"_q,
					false);
				if (enabled) {
					preload();
				}
				return enabled;
			}));

		if (hasFilters) {
			preload();
		}

		builder.addSlideButton({
			.id = u"main/folders"_q,
			.title = tr::lng_settings_section_filters(),
			.icon = { &st::menuIconShowInFolder },
			.shown = std::move(shownProducer),
			.onClick = [=] { showOther(Folders::Id()); },
			.keywords = { u"filters"_q, u"tabs"_q },
		});
	} else {
		builder.addSlideButton({
			.id = u"main/folders"_q,
			.title = tr::lng_settings_section_filters(),
			.icon = { &st::menuIconShowInFolder },
			.shown = rpl::single(true),
			.keywords = { u"filters"_q, u"tabs"_q },
		});
	}

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
			? Fn<void()>([=] { window->show(Box(PowerSavingBox, PowerSaving::Flags())); })
			: Fn<void()>(nullptr),
		.keywords = { u"battery"_q, u"animations"_q, u"power"_q, u"saving"_q },
	});

	const auto languageClick = controller ? [=] {
		static auto Guard = base::binary_guard();
		Guard = LanguageBox::Show(controller);
	} : Fn<void()>();
	builder.addLabeledButton({
		.id = u"main/language"_q,
		.title = tr::lng_settings_language(),
		.icon = { &st::menuIconTranslate },
		.label = rpl::single(
			Lang::GetInstance().id()
		) | rpl::then(
			Lang::GetInstance().idChanges()
		) | rpl::map([] { return Lang::GetInstance().nativeName(); }),
		.onClick = languageClick,
		.keywords = { u"translate"_q, u"localization"_q, u"language"_q },
	});
}

void BuildInterfaceScale(
		SectionBuilder &builder,
		Window::SessionController *controller) {
	if (!HasInterfaceScale()) {
		return;
	}

	builder.addDivider();
	builder.addSkip();

	const auto window = controller ? &controller->window() : nullptr;
	builder.addControl({
		.factory = window ? [=](not_null<Ui::VerticalLayout*> container) {
			auto wrap = object_ptr<Ui::VerticalLayout>(container);
			SetupInterfaceScale(window, wrap.data());
			return wrap;
		} : Fn<object_ptr<Ui::RpWidget>(not_null<Ui::VerticalLayout*>)>(nullptr),
		.id = u"main/scale"_q,
		.title = tr::lng_settings_default_scale(),
		.keywords = { u"zoom"_q, u"size"_q, u"interface"_q, u"ui"_q },
	});

	builder.addSkip();
}

void BuildPremiumSection(
		SectionBuilder &builder,
		Window::SessionController *controller) {
	if (controller && !controller->session().premiumPossible()) {
		return;
	}

	builder.addDivider();
	builder.addSkip();

	const auto showOther = builder.showOther();

	builder.addPremiumButton({
		.id = u"main/premium"_q,
		.title = tr::lng_premium_summary_title(),
		.onClick = controller ? Fn<void()>([=] {
			controller->setPremiumRef("settings");
			showOther(PremiumId());
		}) : Fn<void()>(nullptr),
		.keywords = { u"subscription"_q },
	});

	if (controller) {
		controller->session().credits().load();
	}
	builder.addPremiumButton({
		.id = u"main/credits"_q,
		.title = tr::lng_settings_credits(),
		.label = controller
			? (controller->session().credits().balanceValue(
				) | rpl::map([](CreditsAmount c) {
					return c
						? Lang::FormatCreditsAmountToShort(c).string
						: QString();
				}))
			: rpl::single(QString()),
		.credits = true,
		.onClick = controller ? Fn<void()>([=] {
			controller->setPremiumRef("settings");
			showOther(CreditsId());
		}) : Fn<void()>(nullptr),
		.keywords = { u"stars"_q, u"balance"_q },
	});

	if (controller) {
		controller->session().credits().tonLoad();
	}
	builder.addSlideLabeledButton({
		.id = u"main/currency"_q,
		.title = tr::lng_settings_currency(),
		.icon = { &st::menuIconTon },
		.label = controller
			? (controller->session().credits().tonBalanceValue(
				) | rpl::map([](CreditsAmount c) {
					return c
						? Lang::FormatCreditsAmountToShort(c).string
						: QString();
				}))
			: rpl::single(QString()),
		.shown = controller
			? (controller->session().credits().tonBalanceValue(
				) | rpl::map([](CreditsAmount c) { return !c.empty(); }))
			: rpl::single(false),
		.onClick = controller ? Fn<void()>([=] {
			controller->setPremiumRef("settings");
			showOther(CurrencyId());
		}) : Fn<void()>(nullptr),
		.keywords = { u"ton"_q, u"crypto"_q, u"wallet"_q },
	});

	builder.addSettingsButton({
		.id = u"main/business"_q,
		.title = tr::lng_business_title(),
		.icon = { .icon = &st::menuIconShop },
		.onClick = showOther
			? Fn<void()>([=] { showOther(BusinessId()); })
			: Fn<void()>(nullptr),
		.keywords = { u"work"_q, u"company"_q },
	});

	if (!controller || controller->session().premiumCanBuy()) {
		builder.addSettingsButton({
			.id = u"main/send-gift"_q,
			.title = tr::lng_settings_gift_premium(),
			.icon = { .icon = &st::menuIconGiftPremium, .newBadge = true },
			.onClick = controller
				? Fn<void()>([=] { Ui::ChooseStarGiftRecipient(controller); })
				: Fn<void()>(nullptr),
			.keywords = { u"present"_q, u"send"_q },
		});
	}

	builder.addSkip();
}

void BuildHelpSection(
		SectionBuilder &builder,
		Window::SessionController *controller) {
	builder.addDivider();
	builder.addSkip();

	builder.addSettingsButton({
		.id = u"main/faq"_q,
		.title = tr::lng_settings_faq(),
		.icon = { &st::menuIconFaq },
		.onClick = [=] { OpenFaq(controller); },
		.keywords = { u"help"_q, u"support"_q, u"questions"_q },
	});

	builder.addSettingsButton({
		.id = u"main/features"_q,
		.title = tr::lng_settings_features(),
		.icon = { &st::menuIconEmojiObjects },
		.onClick = [] {
			UrlClickHandler::Open(tr::lng_telegram_features_url(tr::now));
		},
		.keywords = { u"tips"_q, u"tutorial"_q },
	});

	builder.addSettingsButton({
		.id = u"main/ask-question"_q,
		.title = tr::lng_settings_ask_question(),
		.icon = { &st::menuIconDiscussion },
		.onClick = [=] { OpenAskQuestionConfirm(controller); },
		.keywords = { u"contact"_q, u"feedback"_q },
	});

	builder.addSkip();
}

void BuildValidationSuggestions(
		SectionBuilder &builder,
		Window::SessionController *controller) {
	if (!controller) {
		return;
	}

	const auto showOther = builder.showOther();

	builder.addControl({
		.factory = [=](not_null<Ui::VerticalLayout*> container) {
			auto wrap = object_ptr<Ui::VerticalLayout>(container);
			SetupValidatePhoneNumberSuggestion(controller, wrap.data(), showOther);
			return wrap;
		},
	});

	builder.addControl({
		.factory = [=](not_null<Ui::VerticalLayout*> container) {
			auto wrap = object_ptr<Ui::VerticalLayout>(container);
			SetupValidatePasswordSuggestion(controller, wrap.data(), showOther);
			return wrap;
		},
	});
}

void BuildMainSectionContent(
		SectionBuilder &builder,
		Window::SessionController *controller) {
	builder.addDivider();
	builder.addSkip();

	BuildValidationSuggestions(builder, controller);

	BuildSectionButtons(builder, controller);

	builder.addSkip();

	BuildInterfaceScale(builder, controller);
	BuildPremiumSection(builder, controller);
	BuildHelpSection(builder, controller);
}

} // namespace

void MainSection(
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*> controller,
		Fn<void(Type)> showOther,
		rpl::producer<> showFinished) {
	auto &lifetime = container->lifetime();
	const auto highlights = lifetime.make_state<HighlightRegistry>();
	const auto isPaused = Window::PausedIn(
		controller,
		Window::GifPauseReason::Layer);
	auto builder = SectionBuilder(WidgetContext{
		.container = container,
		.controller = controller,
		.showOther = std::move(showOther),
		.isPaused = isPaused,
		.highlights = highlights,
	});
	BuildMainSectionContent(builder, controller);

	std::move(showFinished) | rpl::on_next([=] {
		for (const auto &[id, entry] : *highlights) {
			if (const auto widget = entry.widget.data()) {
				controller->checkHighlightControl(
					id,
					widget,
					base::duplicate(entry.args));
			}
		}
	}, lifetime);
}

std::vector<SearchEntry> MainSectionForSearch() {
	auto entries = std::vector<SearchEntry>();
	auto builder = SectionBuilder(SearchContext{
		.entries = &entries,
	});
	BuildMainSectionContent(builder, nullptr);
	return entries;
}

} // namespace Settings::Builder
