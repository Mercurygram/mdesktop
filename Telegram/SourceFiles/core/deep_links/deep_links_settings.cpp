/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/deep_links/deep_links_router.h"

#include "base/binary_guard.h"
#include "boxes/language_box.h"
#include "main/main_session.h"
#include "settings/settings_active_sessions.h"
#include "settings/settings_advanced.h"
#include "settings/settings_blocked_peers.h"
#include "settings/settings_business.h"
#include "settings/settings_calls.h"
#include "settings/settings_chat.h"
#include "settings/settings_credits.h"
#include "settings/settings_folders.h"
#include "settings/settings_global_ttl.h"
#include "settings/settings_information.h"
#include "settings/settings_local_passcode.h"
#include "settings/settings_main.h"
#include "settings/settings_notifications.h"
#include "settings/settings_premium.h"
#include "settings/settings_privacy_security.h"
#include "settings/settings_websites.h"
#include "window/window_session_controller.h"

namespace Core::DeepLinks {
namespace {

Result ShowLanguageBox(const Context &ctx) {
	static auto Guard = base::binary_guard();
	Guard = LanguageBox::Show(ctx.controller);
	return Result::Handled;
}

Result ShowSavedMessages(const Context &ctx) {
	if (!ctx.controller) {
		return Result::NeedsAuth;
	}
	ctx.controller->showPeerHistory(ctx.controller->session().userPeerId());
	return Result::Handled;
}

Result ShowFaq(const Context &ctx) {
	::Settings::OpenFaq(
		ctx.controller ? base::make_weak(ctx.controller) : nullptr);
	return Result::Handled;
}

} // namespace

void RegisterSettingsHandlers(Router &router) {
	router.add(u"settings"_q, {
		.path = QString(),
		.action = SettingsSection{ ::Settings::Main::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"edit"_q,
		.action = SettingsSection{ ::Settings::Information::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"my-profile"_q,
		.action = SettingsSection{ ::Settings::Information::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"devices"_q,
		.action = SettingsSection{ ::Settings::Sessions::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"folders"_q,
		.action = SettingsSection{ ::Settings::Folders::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"notifications"_q,
		.action = SettingsSection{ ::Settings::Notifications::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"privacy"_q,
		.action = SettingsSection{ ::Settings::PrivacySecurity::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"privacy/blocked"_q,
		.action = SettingsSection{ ::Settings::Blocked::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"privacy/active-websites"_q,
		.action = SettingsSection{ ::Settings::Websites::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"privacy/passcode"_q,
		.action = SettingsSection{ ::Settings::LocalPasscodeManageId() },
	});

	router.add(u"settings"_q, {
		.path = u"privacy/auto-delete"_q,
		.action = SettingsSection{ ::Settings::GlobalTTLId() },
	});

	router.add(u"settings"_q, {
		.path = u"data/storage"_q,
		.action = SettingsSection{ ::Settings::Advanced::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"data/proxy"_q,
		.action = SettingsSection{ ::Settings::Advanced::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"appearance"_q,
		.action = SettingsSection{ ::Settings::Chat::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"power-saving"_q,
		.action = SettingsSection{ ::Settings::Chat::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"language"_q,
		.action = CodeBlock{ ShowLanguageBox },
		.requiresAuth = false,
	});

	router.add(u"settings"_q, {
		.path = u"premium"_q,
		.action = SettingsSection{ ::Settings::PremiumId() },
	});

	router.add(u"settings"_q, {
		.path = u"stars"_q,
		.action = SettingsSection{ ::Settings::CreditsId() },
	});

	router.add(u"settings"_q, {
		.path = u"ton"_q,
		.action = SettingsSection{ ::Settings::CurrencyId() },
	});

	router.add(u"settings"_q, {
		.path = u"business"_q,
		.action = SettingsSection{ ::Settings::BusinessId() },
	});

	router.add(u"settings"_q, {
		.path = u"saved-messages"_q,
		.action = CodeBlock{ ShowSavedMessages },
	});

	router.add(u"settings"_q, {
		.path = u"calls"_q,
		.action = SettingsSection{ ::Settings::Calls::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"calls/all"_q,
		.action = SettingsSection{ ::Settings::Calls::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"faq"_q,
		.action = CodeBlock{ ShowFaq },
		.requiresAuth = false,
	});
}

} // namespace Core::DeepLinks
