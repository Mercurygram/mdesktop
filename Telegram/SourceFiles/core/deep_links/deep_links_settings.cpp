/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/deep_links/deep_links_router.h"

#include "base/binary_guard.h"
#include "boxes/add_contact_box.h"
#include "boxes/language_box.h"
#include "boxes/star_gift_box.h"
#include "boxes/username_box.h"
#include "core/application.h"
#include "core/click_handler_types.h"
#include "data/data_user.h"
#include "data/notify/data_notify_settings.h"
#include "ui/boxes/peer_qr_box.h"
#include "ui/layers/generic_box.h"
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
#include "settings/settings_notifications_type.h"
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

void ShowQrBox(not_null<Window::SessionController*> controller) {
	const auto user = controller->session().user();
	controller->uiShow()->show(Box(
		Ui::FillPeerQrBox,
		user.get(),
		std::nullopt,
		rpl::single(QString())));
}

Result HandleQrCode(const Context &ctx, bool highlightCopy) {
	if (!ctx.controller) {
		return Result::NeedsAuth;
	}

	if (highlightCopy) {
		ctx.controller->setHighlightControlId(u"self-qr-code/copy"_q);
	}

	const auto user = ctx.controller->session().user();
	if (!user->username().isEmpty()) {
		ShowQrBox(ctx.controller);
	} else {
		const auto controller = ctx.controller;
		controller->uiShow()->show(Box(
			UsernamesBoxWithCallback,
			user,
			[=] { ShowQrBox(controller); }));
	}
	return Result::Handled;
}

Result ShowEditName(const Context &ctx) {
	if (!ctx.controller) {
		return Result::NeedsAuth;
	}
	if (ctx.controller->showFrozenError()) {
		return Result::Handled;
	}
	ctx.controller->show(Box<EditNameBox>(ctx.controller->session().user()));
	return Result::Handled;
}

Result ShowEditUsername(const Context &ctx) {
	if (!ctx.controller) {
		return Result::NeedsAuth;
	}
	if (ctx.controller->showFrozenError()) {
		return Result::Handled;
	}
	ctx.controller->show(Box(UsernamesBox, ctx.controller->session().user()));
	return Result::Handled;
}

Result OpenInternalUrl(const Context &ctx, const QString &url) {
	if (!ctx.controller) {
		return Result::NeedsAuth;
	}
	Core::App().openInternalUrl(
		url,
		QVariant::fromValue(ClickHandlerContext{
			.sessionWindow = base::make_weak(ctx.controller),
		}));
	return Result::Handled;
}

Result ShowNotificationType(
		const Context &ctx,
		Data::DefaultNotify type,
		const QString &highlightId = QString()) {
	if (!ctx.controller) {
		return Result::NeedsAuth;
	}
	if (!highlightId.isEmpty()) {
		ctx.controller->setHighlightControlId(highlightId);
	}
	ctx.controller->showSettings(::Settings::NotificationsType::Id(type));
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
		.path = u"my-profile/edit"_q,
		.action = SettingsSection{ ::Settings::Information::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"my-profile/posts"_q,
		.action = SettingsSection{ ::Settings::Information::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"my-profile/posts/add-album"_q,
		.action = SettingsSection{ ::Settings::Information::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"my-profile/gifts"_q,
		.action = SettingsSection{ ::Settings::Information::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"my-profile/archived-posts"_q,
		.action = SettingsSection{ ::Settings::Information::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"emoji-status"_q,
		.action = SettingsSection{ ::Settings::Main::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"profile-color"_q,
		.action = SettingsSection{ ::Settings::Information::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"profile-color/profile"_q,
		.action = SettingsSection{ ::Settings::Information::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"profile-color/profile/add-icons"_q,
		.action = SettingsSection{ ::Settings::Information::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"profile-color/profile/use-gift"_q,
		.action = SettingsSection{ ::Settings::Information::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"profile-color/name"_q,
		.action = SettingsSection{ ::Settings::Information::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"profile-color/name/add-icons"_q,
		.action = SettingsSection{ ::Settings::Information::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"profile-color/name/use-gift"_q,
		.action = SettingsSection{ ::Settings::Information::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"profile-photo"_q,
		.action = SettingsSection{ ::Settings::Information::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"profile-photo/use-emoji"_q,
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
		.path = u"privacy/blocked/block-user"_q,
		.action = SettingsControl{
			::Settings::Blocked::Id(),
			u"privacy/blocked/block-user"_q,
		},
	});

	router.add(u"settings"_q, {
		.path = u"privacy/active-websites"_q,
		.action = SettingsSection{ ::Settings::Websites::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"privacy/active-websites/disconnect-all"_q,
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
		.path = u"privacy/2sv"_q,
		.action = SettingsSection{ ::Settings::PrivacySecurity::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"privacy/passkey"_q,
		.action = SettingsSection{ ::Settings::PrivacySecurity::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"privacy/passkey/create"_q,
		.action = SettingsSection{ ::Settings::PrivacySecurity::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"privacy/passcode/disable"_q,
		.action = SettingsSection{ ::Settings::LocalPasscodeManageId() },
	});

	router.add(u"settings"_q, {
		.path = u"privacy/passcode/change"_q,
		.action = SettingsSection{ ::Settings::LocalPasscodeManageId() },
	});

	router.add(u"settings"_q, {
		.path = u"privacy/passcode/auto-lock"_q,
		.action = SettingsSection{ ::Settings::LocalPasscodeManageId() },
	});

	router.add(u"settings"_q, {
		.path = u"privacy/passcode/face-id"_q,
		.action = SettingsSection{ ::Settings::LocalPasscodeManageId() },
	});

	router.add(u"settings"_q, {
		.path = u"privacy/passcode/fingerprint"_q,
		.action = SettingsSection{ ::Settings::LocalPasscodeManageId() },
	});

	router.add(u"settings"_q, {
		.path = u"privacy/2sv/change"_q,
		.action = SettingsSection{ ::Settings::PrivacySecurity::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"privacy/2sv/disable"_q,
		.action = SettingsSection{ ::Settings::PrivacySecurity::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"privacy/2sv/change-email"_q,
		.action = SettingsSection{ ::Settings::PrivacySecurity::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"privacy/auto-delete/set-custom"_q,
		.action = SettingsSection{ ::Settings::GlobalTTLId() },
	});

	router.add(u"settings"_q, {
		.path = u"privacy/phone-number"_q,
		.action = SettingsSection{ ::Settings::PrivacySecurity::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"privacy/phone-number/never"_q,
		.action = SettingsSection{ ::Settings::PrivacySecurity::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"privacy/phone-number/always"_q,
		.action = SettingsSection{ ::Settings::PrivacySecurity::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"privacy/last-seen"_q,
		.action = SettingsSection{ ::Settings::PrivacySecurity::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"privacy/last-seen/never"_q,
		.action = SettingsSection{ ::Settings::PrivacySecurity::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"privacy/last-seen/always"_q,
		.action = SettingsSection{ ::Settings::PrivacySecurity::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"privacy/last-seen/hide-read-time"_q,
		.action = SettingsSection{ ::Settings::PrivacySecurity::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"privacy/profile-photos"_q,
		.action = SettingsSection{ ::Settings::PrivacySecurity::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"privacy/profile-photos/never"_q,
		.action = SettingsSection{ ::Settings::PrivacySecurity::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"privacy/profile-photos/always"_q,
		.action = SettingsSection{ ::Settings::PrivacySecurity::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"privacy/profile-photos/set-public"_q,
		.action = SettingsSection{ ::Settings::PrivacySecurity::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"privacy/profile-photos/update-public"_q,
		.action = SettingsSection{ ::Settings::PrivacySecurity::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"privacy/profile-photos/remove-public"_q,
		.action = SettingsSection{ ::Settings::PrivacySecurity::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"privacy/bio"_q,
		.action = SettingsSection{ ::Settings::PrivacySecurity::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"privacy/bio/never-share"_q,
		.action = SettingsSection{ ::Settings::PrivacySecurity::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"privacy/bio/always-share"_q,
		.action = SettingsSection{ ::Settings::PrivacySecurity::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"privacy/gifts"_q,
		.action = SettingsSection{ ::Settings::PrivacySecurity::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"privacy/gifts/show-icon"_q,
		.action = SettingsSection{ ::Settings::PrivacySecurity::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"privacy/gifts/never-share"_q,
		.action = SettingsSection{ ::Settings::PrivacySecurity::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"privacy/gifts/always-share"_q,
		.action = SettingsSection{ ::Settings::PrivacySecurity::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"privacy/gifts/accepted-types"_q,
		.action = SettingsSection{ ::Settings::PrivacySecurity::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"privacy/birthday"_q,
		.action = SettingsSection{ ::Settings::PrivacySecurity::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"privacy/birthday/add"_q,
		.action = SettingsSection{ ::Settings::PrivacySecurity::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"privacy/birthday/never"_q,
		.action = SettingsSection{ ::Settings::PrivacySecurity::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"privacy/birthday/always"_q,
		.action = SettingsSection{ ::Settings::PrivacySecurity::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"privacy/saved-music"_q,
		.action = SettingsSection{ ::Settings::PrivacySecurity::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"privacy/saved-music/never"_q,
		.action = SettingsSection{ ::Settings::PrivacySecurity::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"privacy/saved-music/always"_q,
		.action = SettingsSection{ ::Settings::PrivacySecurity::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"privacy/forwards"_q,
		.action = SettingsSection{ ::Settings::PrivacySecurity::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"privacy/forwards/never"_q,
		.action = SettingsSection{ ::Settings::PrivacySecurity::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"privacy/forwards/always"_q,
		.action = SettingsSection{ ::Settings::PrivacySecurity::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"privacy/calls"_q,
		.action = SettingsSection{ ::Settings::PrivacySecurity::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"privacy/calls/never"_q,
		.action = SettingsSection{ ::Settings::PrivacySecurity::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"privacy/calls/always"_q,
		.action = SettingsSection{ ::Settings::PrivacySecurity::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"privacy/calls/p2p"_q,
		.action = SettingsSection{ ::Settings::PrivacySecurity::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"privacy/calls/p2p/never"_q,
		.action = SettingsSection{ ::Settings::PrivacySecurity::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"privacy/calls/p2p/always"_q,
		.action = SettingsSection{ ::Settings::PrivacySecurity::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"privacy/voice"_q,
		.action = SettingsSection{ ::Settings::PrivacySecurity::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"privacy/voice/never"_q,
		.action = SettingsSection{ ::Settings::PrivacySecurity::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"privacy/voice/always"_q,
		.action = SettingsSection{ ::Settings::PrivacySecurity::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"privacy/messages"_q,
		.action = SettingsSection{ ::Settings::PrivacySecurity::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"privacy/messages/set-price"_q,
		.action = SettingsSection{ ::Settings::PrivacySecurity::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"privacy/messages/remove-fee"_q,
		.action = SettingsSection{ ::Settings::PrivacySecurity::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"privacy/invites"_q,
		.action = SettingsSection{ ::Settings::PrivacySecurity::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"privacy/invites/never"_q,
		.action = SettingsSection{ ::Settings::PrivacySecurity::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"privacy/invites/always"_q,
		.action = SettingsSection{ ::Settings::PrivacySecurity::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"privacy/self-destruct"_q,
		.action = SettingsSection{ ::Settings::PrivacySecurity::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"privacy/data-settings/suggest-contacts"_q,
		.action = SettingsSection{ ::Settings::PrivacySecurity::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"privacy/data-settings/clear-payment-info"_q,
		.action = SettingsSection{ ::Settings::PrivacySecurity::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"privacy/archive-and-mute"_q,
		.action = SettingsSection{ ::Settings::PrivacySecurity::Id() },
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
		.path = u"data/storage/clear-cache"_q,
		.action = SettingsSection{ ::Settings::Advanced::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"data/max-cache"_q,
		.action = SettingsSection{ ::Settings::Advanced::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"data/show-18-content"_q,
		.action = SettingsSection{ ::Settings::Chat::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"data/proxy/add-proxy"_q,
		.action = SettingsSection{ ::Settings::Advanced::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"data/proxy/share-list"_q,
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
		.path = u"power-saving/stickers"_q,
		.action = SettingsSection{ ::Settings::Chat::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"power-saving/emoji"_q,
		.action = SettingsSection{ ::Settings::Chat::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"power-saving/effects"_q,
		.action = SettingsSection{ ::Settings::Chat::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"appearance/themes"_q,
		.action = SettingsSection{ ::Settings::Chat::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"appearance/themes/edit"_q,
		.action = SettingsSection{ ::Settings::Chat::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"appearance/themes/create"_q,
		.action = SettingsSection{ ::Settings::Chat::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"appearance/wallpapers"_q,
		.action = SettingsSection{ ::Settings::Chat::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"appearance/wallpapers/set"_q,
		.action = SettingsSection{ ::Settings::Chat::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"appearance/wallpapers/choose-photo"_q,
		.action = SettingsSection{ ::Settings::Chat::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"appearance/your-color/profile"_q,
		.action = SettingsSection{ ::Settings::Chat::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"appearance/your-color/profile/add-icons"_q,
		.action = SettingsSection{ ::Settings::Chat::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"appearance/your-color/profile/use-gift"_q,
		.action = SettingsSection{ ::Settings::Chat::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"appearance/your-color/profile/reset"_q,
		.action = SettingsSection{ ::Settings::Chat::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"appearance/your-color/name"_q,
		.action = SettingsSection{ ::Settings::Chat::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"appearance/your-color/name/add-icons"_q,
		.action = SettingsSection{ ::Settings::Chat::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"appearance/your-color/name/use-gift"_q,
		.action = SettingsSection{ ::Settings::Chat::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"appearance/night-mode"_q,
		.action = SettingsSection{ ::Settings::Chat::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"appearance/auto-night-mode"_q,
		.action = SettingsSection{ ::Settings::Chat::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"appearance/text-size"_q,
		.action = SettingsSection{ ::Settings::Chat::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"appearance/animations"_q,
		.action = SettingsSection{ ::Settings::Chat::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"appearance/stickers-and-emoji"_q,
		.action = SettingsSection{ ::Settings::Chat::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"appearance/stickers-and-emoji/edit"_q,
		.action = SettingsSection{ ::Settings::Chat::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"appearance/stickers-and-emoji/trending"_q,
		.action = SettingsSection{ ::Settings::Chat::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"appearance/stickers-and-emoji/archived"_q,
		.action = SettingsSection{ ::Settings::Chat::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"appearance/stickers-and-emoji/emoji"_q,
		.action = SettingsSection{ ::Settings::Chat::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"appearance/stickers-and-emoji/emoji/suggest"_q,
		.action = SettingsSection{ ::Settings::Chat::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"appearance/stickers-and-emoji/emoji/quick-reaction"_q,
		.action = SettingsSection{ ::Settings::Chat::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"appearance/stickers-and-emoji/emoji/quick-reaction/choose"_q,
		.action = SettingsSection{ ::Settings::Chat::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"appearance/stickers-and-emoji/suggest-by-emoji"_q,
		.action = SettingsSection{ ::Settings::Chat::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"appearance/stickers-and-emoji/emoji/large"_q,
		.action = SettingsSection{ ::Settings::Chat::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"language"_q,
		.action = CodeBlock{ ShowLanguageBox },
		.requiresAuth = false,
	});

	router.add(u"settings"_q, {
		.path = u"language/show-button"_q,
		.action = CodeBlock{ ShowLanguageBox },
		.requiresAuth = false,
	});

	router.add(u"settings"_q, {
		.path = u"language/translate-chats"_q,
		.action = CodeBlock{ ShowLanguageBox },
		.requiresAuth = false,
	});

	router.add(u"settings"_q, {
		.path = u"language/do-not-translate"_q,
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
		.path = u"stars/top-up"_q,
		.action = SettingsSection{ ::Settings::CreditsId() },
	});

	router.add(u"settings"_q, {
		.path = u"stars/stats"_q,
		.action = SettingsControl{
			::Settings::CreditsId(),
			u"stars/stats"_q,
		},
	});

	router.add(u"settings"_q, {
		.path = u"stars/gift"_q,
		.action = SettingsControl{
			::Settings::CreditsId(),
			u"stars/gift"_q,
		},
	});

	router.add(u"settings"_q, {
		.path = u"stars/earn"_q,
		.action = SettingsControl{
			::Settings::CreditsId(),
			u"stars/earn"_q,
		},
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
		.path = u"business/do-not-hide-ads"_q,
		.action = SettingsControl{
			::Settings::BusinessId(),
			u"business/sponsored"_q,
		},
	});

	router.add(u"settings"_q, {
		.path = u"send-gift"_q,
		.action = SettingsControl{
			::Settings::Main::Id(),
			u"main/send-gift"_q,
		},
	});

	router.add(u"settings"_q, {
		.path = u"send-gift/self"_q,
		.action = CodeBlock{ [](const Context &ctx) {
			if (!ctx.controller) {
				return Result::NeedsAuth;
			}
			Ui::ShowStarGiftBox(ctx.controller, ctx.controller->session().user());
			return Result::Handled;
		}},
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

	router.add(u"settings"_q, {
		.path = u"ask-question"_q,
		.action = SettingsControl{
			::Settings::Main::Id(),
			u"main/ask-question"_q,
		},
	});

	router.add(u"settings"_q, {
		.path = u"features"_q,
		.action = SettingsControl{
			::Settings::Main::Id(),
			u"main/features"_q,
		},
	});

	router.add(u"settings"_q, {
		.path = u"search"_q,
		.action = SettingsSection{ ::Settings::Main::Id() },
	});

	router.add(u"settings"_q, {
		.path = u"qr-code"_q,
		.action = CodeBlock{ [](const Context &ctx) {
			return HandleQrCode(ctx, false);
		}},
	});

	router.add(u"settings"_q, {
		.path = u"qr-code/share"_q,
		.action = CodeBlock{ [](const Context &ctx) {
			return HandleQrCode(ctx, true);
		}},
	});

	// Edit profile deep links.
	router.add(u"settings"_q, {
		.path = u"edit/first-name"_q,
		.action = CodeBlock{ ShowEditName },
	});
	router.add(u"settings"_q, {
		.path = u"edit/last-name"_q,
		.action = CodeBlock{ ShowEditName },
	});
	router.add(u"settings"_q, {
		.path = u"edit/bio"_q,
		.action = SettingsControl{
			::Settings::Information::Id(),
			u"edit/bio"_q,
		},
	});
	router.add(u"settings"_q, {
		.path = u"edit/birthday"_q,
		.action = CodeBlock{ [](const Context &ctx) {
			return OpenInternalUrl(ctx, u"internal:edit_birthday"_q);
		}},
	});
	router.add(u"settings"_q, {
		.path = u"edit/change-number"_q,
		.action = SettingsSection{ ::Settings::Information::Id() },
	});
	router.add(u"settings"_q, {
		.path = u"edit/username"_q,
		.action = CodeBlock{ ShowEditUsername },
	});
	router.add(u"settings"_q, {
		.path = u"edit/your-color"_q,
		.action = SettingsControl{
			::Settings::Information::Id(),
			u"edit/your-color"_q,
		},
	});
	router.add(u"settings"_q, {
		.path = u"edit/channel"_q,
		.action = CodeBlock{ [](const Context &ctx) {
			return OpenInternalUrl(ctx, u"internal:edit_personal_channel"_q);
		}},
	});
	router.add(u"settings"_q, {
		.path = u"edit/add-account"_q,
		.action = SettingsControl{
			::Settings::Information::Id(),
			u"edit/add-account"_q,
		},
	});
	router.add(u"settings"_q, {
		.path = u"edit/log-out"_q,
		.action = SettingsSection{ ::Settings::Information::Id() },
	});

	// Calls deep links.
	router.add(u"settings"_q, {
		.path = u"calls/start-call"_q,
		.action = SettingsSection{ ::Settings::Calls::Id() },
	});

	// Devices (sessions) deep links.
	router.add(u"settings"_q, {
		.path = u"devices/terminate-sessions"_q,
		.action = SettingsControl{
			::Settings::Sessions::Id(),
			u"devices/terminate-sessions"_q,
		},
	});
	router.add(u"settings"_q, {
		.path = u"devices/auto-terminate"_q,
		.action = SettingsControl{
			::Settings::Sessions::Id(),
			u"devices/auto-terminate"_q,
		},
	});

	// Folders deep links.
	router.add(u"settings"_q, {
		.path = u"folders/create"_q,
		.action = SettingsControl{
			::Settings::Folders::Id(),
			u"folders/create"_q,
		},
	});
	router.add(u"settings"_q, {
		.path = u"folders/add-recommended"_q,
		.action = SettingsSection{ ::Settings::Folders::Id() },
	});
	router.add(u"settings"_q, {
		.path = u"folders/show-tags"_q,
		.action = SettingsControl{
			::Settings::Folders::Id(),
			u"folders/show-tags"_q,
		},
	});
	router.add(u"settings"_q, {
		.path = u"folders/tab-view"_q,
		.action = SettingsControl{
			::Settings::Folders::Id(),
			u"folders/tab-view"_q,
		},
	});

	// Notifications deep links.
	router.add(u"settings"_q, {
		.path = u"notifications/accounts"_q,
		.action = SettingsControl{
			::Settings::Notifications::Id(),
			u"notifications/accounts"_q,
		},
	});

	// Notification type deep links - Private Chats.
	router.add(u"settings"_q, {
		.path = u"notifications/private-chats"_q,
		.action = CodeBlock{ [](const Context &ctx) {
			return ShowNotificationType(ctx, Data::DefaultNotify::User);
		}},
	});
	router.add(u"settings"_q, {
		.path = u"notifications/private-chats/edit"_q,
		.action = CodeBlock{ [](const Context &ctx) {
			return ShowNotificationType(ctx, Data::DefaultNotify::User);
		}},
	});
	router.add(u"settings"_q, {
		.path = u"notifications/private-chats/show"_q,
		.action = CodeBlock{ [](const Context &ctx) {
			return ShowNotificationType(
				ctx,
				Data::DefaultNotify::User,
				u"notifications/type/show"_q);
		}},
	});
	router.add(u"settings"_q, {
		.path = u"notifications/private-chats/sound"_q,
		.action = CodeBlock{ [](const Context &ctx) {
			return ShowNotificationType(
				ctx,
				Data::DefaultNotify::User,
				u"notifications/type/sound"_q);
		}},
	});
	router.add(u"settings"_q, {
		.path = u"notifications/private-chats/add-exception"_q,
		.action = CodeBlock{ [](const Context &ctx) {
			return ShowNotificationType(
				ctx,
				Data::DefaultNotify::User,
				u"notifications/type/add-exception"_q);
		}},
	});
	router.add(u"settings"_q, {
		.path = u"notifications/private-chats/delete-exceptions"_q,
		.action = CodeBlock{ [](const Context &ctx) {
			return ShowNotificationType(
				ctx,
				Data::DefaultNotify::User,
				u"notifications/type/delete-exceptions"_q);
		}},
	});

	// Notification type deep links - Groups.
	router.add(u"settings"_q, {
		.path = u"notifications/groups"_q,
		.action = CodeBlock{ [](const Context &ctx) {
			return ShowNotificationType(ctx, Data::DefaultNotify::Group);
		}},
	});
	router.add(u"settings"_q, {
		.path = u"notifications/groups/edit"_q,
		.action = CodeBlock{ [](const Context &ctx) {
			return ShowNotificationType(ctx, Data::DefaultNotify::Group);
		}},
	});
	router.add(u"settings"_q, {
		.path = u"notifications/groups/show"_q,
		.action = CodeBlock{ [](const Context &ctx) {
			return ShowNotificationType(
				ctx,
				Data::DefaultNotify::Group,
				u"notifications/type/show"_q);
		}},
	});
	router.add(u"settings"_q, {
		.path = u"notifications/groups/sound"_q,
		.action = CodeBlock{ [](const Context &ctx) {
			return ShowNotificationType(
				ctx,
				Data::DefaultNotify::Group,
				u"notifications/type/sound"_q);
		}},
	});
	router.add(u"settings"_q, {
		.path = u"notifications/groups/add-exception"_q,
		.action = CodeBlock{ [](const Context &ctx) {
			return ShowNotificationType(
				ctx,
				Data::DefaultNotify::Group,
				u"notifications/type/add-exception"_q);
		}},
	});
	router.add(u"settings"_q, {
		.path = u"notifications/groups/delete-exceptions"_q,
		.action = CodeBlock{ [](const Context &ctx) {
			return ShowNotificationType(
				ctx,
				Data::DefaultNotify::Group,
				u"notifications/type/delete-exceptions"_q);
		}},
	});

	// Notification type deep links - Channels.
	router.add(u"settings"_q, {
		.path = u"notifications/channels"_q,
		.action = CodeBlock{ [](const Context &ctx) {
			return ShowNotificationType(ctx, Data::DefaultNotify::Broadcast);
		}},
	});
	router.add(u"settings"_q, {
		.path = u"notifications/channels/edit"_q,
		.action = CodeBlock{ [](const Context &ctx) {
			return ShowNotificationType(ctx, Data::DefaultNotify::Broadcast);
		}},
	});
	router.add(u"settings"_q, {
		.path = u"notifications/channels/show"_q,
		.action = CodeBlock{ [](const Context &ctx) {
			return ShowNotificationType(
				ctx,
				Data::DefaultNotify::Broadcast,
				u"notifications/type/show"_q);
		}},
	});
	router.add(u"settings"_q, {
		.path = u"notifications/channels/sound"_q,
		.action = CodeBlock{ [](const Context &ctx) {
			return ShowNotificationType(
				ctx,
				Data::DefaultNotify::Broadcast,
				u"notifications/type/sound"_q);
		}},
	});
	router.add(u"settings"_q, {
		.path = u"notifications/channels/add-exception"_q,
		.action = CodeBlock{ [](const Context &ctx) {
			return ShowNotificationType(
				ctx,
				Data::DefaultNotify::Broadcast,
				u"notifications/type/add-exception"_q);
		}},
	});
	router.add(u"settings"_q, {
		.path = u"notifications/channels/delete-exceptions"_q,
		.action = CodeBlock{ [](const Context &ctx) {
			return ShowNotificationType(
				ctx,
				Data::DefaultNotify::Broadcast,
				u"notifications/type/delete-exceptions"_q);
		}},
	});

	// Other notification deep links.
	router.add(u"settings"_q, {
		.path = u"notifications/include-muted-chats"_q,
		.action = SettingsControl{
			::Settings::Notifications::Id(),
			u"notifications/include-muted-chats"_q,
		},
	});
	router.add(u"settings"_q, {
		.path = u"notifications/count-unread-messages"_q,
		.action = SettingsControl{
			::Settings::Notifications::Id(),
			u"notifications/count-unread-messages"_q,
		},
	});
	router.add(u"settings"_q, {
		.path = u"notifications/new-contacts"_q,
		.action = SettingsControl{
			::Settings::Notifications::Id(),
			u"notifications/events/joined"_q,
		},
	});
	router.add(u"settings"_q, {
		.path = u"notifications/pinned-messages"_q,
		.action = SettingsControl{
			::Settings::Notifications::Id(),
			u"notifications/events/pinned"_q,
		},
	});
}

} // namespace Core::DeepLinks
