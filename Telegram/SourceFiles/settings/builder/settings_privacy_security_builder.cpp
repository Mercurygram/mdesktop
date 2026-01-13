/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "settings/builder/settings_privacy_security_builder.h"

#include "api/api_authorizations.h"
#include "api/api_blocked_peers.h"
#include "api/api_cloud_password.h"
#include "api/api_global_privacy.h"
#include "api/api_self_destruct.h"
#include "api/api_sensitive_content.h"
#include "api/api_user_privacy.h"
#include "api/api_websites.h"
#include "apiwrap.h"
#include "main/main_app_config.h"
#include "ui/chat/chat_style.h"
#include "base/timer_rpl.h"
#include "boxes/edit_privacy_box.h"
#include "boxes/self_destruction_box.h"
#include "core/application.h"
#include "core/core_cloud_password.h"
#include "core/core_settings.h"
#include "data/components/passkeys.h"
#include "data/components/top_peers.h"
#include "data/data_peer_values.h"
#include "lang/lang_keys.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "platform/platform_webauthn.h"
#include "settings/builder/settings_builder.h"
#include "settings/cloud_password/settings_cloud_password_email_confirm.h"
#include "settings/cloud_password/settings_cloud_password_input.h"
#include "settings/cloud_password/settings_cloud_password_start.h"
#include "settings/settings_active_sessions.h"
#include "settings/settings_blocked_peers.h"
#include "settings/settings_global_ttl.h"
#include "settings/settings_local_passcode.h"
#include "settings/settings_main.h"
#include "settings/settings_passkeys.h"
#include "settings/settings_privacy_controllers.h"
#include "settings/settings_privacy_security.h"
#include "settings/settings_websites.h"
#include "storage/storage_domain.h"
#include "ui/layers/generic_box.h"
#include "ui/text/format_values.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"

namespace Settings::Builder {
namespace {

constexpr auto kUpdateTimeout = 60 * crl::time(1000);

using Privacy = Api::UserPrivacy;

void BuildSecuritySection(
		SectionBuilder &builder,
		not_null<Window::SessionController*> controller,
		rpl::producer<> updateTrigger,
		Fn<void(Type)> showOther) {
	const auto session = &controller->session();

	builder.addSkip(st::settingsPrivacySkip);
	builder.addSubsectionTitle(tr::lng_settings_security());

	using State = Core::CloudPasswordState;
	enum class PasswordState {
		Loading,
		On,
		Off,
		Unconfirmed,
	};
	auto passwordState = rpl::single(
		PasswordState::Loading
	) | rpl::then(session->api().cloudPassword().state(
	) | rpl::map([](const State &state) {
		return (!state.unconfirmedPattern.isEmpty())
			? PasswordState::Unconfirmed
			: state.hasPassword
			? PasswordState::On
			: PasswordState::Off;
	})) | rpl::distinct_until_changed();

	auto cloudPasswordLabel = rpl::duplicate(
		passwordState
	) | rpl::map([=](PasswordState state) {
		return (state == PasswordState::Loading)
			? tr::lng_profile_loading(tr::now)
			: (state == PasswordState::On)
			? tr::lng_settings_cloud_password_on(tr::now)
			: tr::lng_settings_cloud_password_off(tr::now);
	});

	builder.addButton({
		.id = u"security/cloud_password"_q,
		.title = tr::lng_settings_cloud_password_start_title(),
		.icon = { &st::menuIcon2SV },
		.label = std::move(cloudPasswordLabel),
		.onClick = [=, passwordState = base::duplicate(passwordState)] {
			const auto state = rpl::variable<PasswordState>(
				base::duplicate(passwordState)).current();
			if (state == PasswordState::Loading) {
				return;
			} else if (state == PasswordState::On) {
				showOther(CloudPasswordInputId());
			} else if (state == PasswordState::Off) {
				showOther(CloudPasswordStartId());
			} else if (state == PasswordState::Unconfirmed) {
				showOther(CloudPasswordEmailConfirmId());
			}
		},
		.keywords = { u"password"_q, u"2fa"_q, u"two-factor"_q },
	});

	session->api().cloudPassword().reload();

	auto ttlLabel = rpl::combine(
		session->api().selfDestruct().periodDefaultHistoryTTL(),
		tr::lng_settings_ttl_after_off()
	) | rpl::map([](int ttl, const QString &none) {
		return ttl ? Ui::FormatTTL(ttl) : none;
	});

	builder.addButton({
		.id = u"security/ttl"_q,
		.title = tr::lng_settings_ttl_title(),
		.icon = { &st::menuIconTTL },
		.label = std::move(ttlLabel),
		.onClick = [showOther] {
			showOther(GlobalTTLId());
		},
		.keywords = { u"ttl"_q, u"auto-delete"_q, u"timer"_q },
	});

	rpl::duplicate(
		updateTrigger
	) | rpl::on_next([=] {
		session->api().selfDestruct().reload();
	}, builder.container()->lifetime());

	auto passcodeHas = rpl::single(rpl::empty) | rpl::then(
		session->domain().local().localPasscodeChanged()
	) | rpl::map([=] {
		return session->domain().local().hasLocalPasscode();
	});
	auto passcodeLabel = rpl::combine(
		tr::lng_settings_cloud_password_on(),
		tr::lng_settings_cloud_password_off(),
		rpl::duplicate(passcodeHas)
	) | rpl::map([](const QString &on, const QString &off, bool has) {
		return has ? on : off;
	});

	builder.addButton({
		.id = u"security/passcode"_q,
		.title = tr::lng_settings_passcode_title(),
		.icon = { &st::menuIconLock },
		.label = std::move(passcodeLabel),
		.onClick = [=, passcodeHas = std::move(passcodeHas)]() mutable {
			if (rpl::variable<bool>(std::move(passcodeHas)).current()) {
				showOther(LocalPasscodeCheckId());
			} else {
				showOther(LocalPasscodeCreateId());
			}
		},
		.keywords = { u"passcode"_q, u"lock"_q, u"pin"_q },
	});

	if (session->passkeys().possible()) {
		auto passkeysLabel = rpl::combine(
			tr::lng_profile_loading(),
			(rpl::single(rpl::empty_value())
				| rpl::then(session->passkeys().requestList())) | rpl::map([=] {
				return session->passkeys().list().size();
			})
		) | rpl::map([=](const QString &loading, int count) {
			return !session->passkeys().listKnown()
				? loading
				: count == 1
				? session->passkeys().list().front().name
				: count
				? QString::number(count)
				: tr::lng_settings_cloud_password_off(tr::now);
		});

		auto passkeysShown = (rpl::single(rpl::empty_value())
			| rpl::then(session->passkeys().requestList())) | rpl::map([=] {
			return Platform::WebAuthn::IsSupported()
				|| !session->passkeys().list().empty();
		});

		builder.addButton({
			.id = u"security/passkeys"_q,
			.title = tr::lng_settings_passkeys_title(),
			.icon = { &st::menuIconPermissions },
			.label = std::move(passkeysLabel),
			.onClick = [=] {
				if (!session->passkeys().listKnown()) {
					return;
				}
				const auto count = session->passkeys().list().size();
				if (count == 0) {
					controller->show(Box([=](not_null<Ui::GenericBox*> box) {
						PasskeysNoneBox(box, session);
						box->boxClosing() | rpl::on_next([=] {
							if (session->passkeys().list().size()) {
								controller->showSettings(PasskeysId());
							}
						}, box->lifetime());
					}));
				} else {
					controller->showSettings(PasskeysId());
				}
			},
			.keywords = { u"passkeys"_q, u"biometric"_q },
			.shown = std::move(passkeysShown),
		});
	}

	auto blockedCount = rpl::combine(
		session->api().blockedPeers().slice(
		) | rpl::map([](const Api::BlockedPeers::Slice &data) {
			return data.total;
		}),
		tr::lng_settings_no_blocked_users()
	) | rpl::map([](int count, const QString &none) {
		return count ? QString::number(count) : none;
	});

	builder.addButton({
		.id = u"security/blocked"_q,
		.title = tr::lng_settings_blocked_users(),
		.icon = { &st::menuIconBlock },
		.label = std::move(blockedCount),
		.onClick = [=] {
			showOther(Blocked::Id());
		},
		.keywords = { u"blocked"_q, u"ban"_q },
	});

	rpl::duplicate(
		updateTrigger
	) | rpl::on_next([=] {
		session->api().blockedPeers().reload();
	}, builder.container()->lifetime());

	auto websitesCount = session->api().websites().totalValue();
	auto websitesShown = rpl::duplicate(websitesCount) | rpl::map(
		rpl::mappers::_1 > 0);
	auto websitesLabel = rpl::duplicate(
		websitesCount
	) | rpl::filter(rpl::mappers::_1 > 0) | rpl::map([](int count) {
		return QString::number(count);
	});

	builder.addButton({
		.id = u"security/websites"_q,
		.title = tr::lng_settings_logged_in(),
		.icon = { &st::menuIconIpAddress },
		.label = std::move(websitesLabel),
		.onClick = [=] {
			showOther(Websites::Id());
		},
		.keywords = { u"websites"_q, u"bots"_q, u"logged"_q },
		.shown = std::move(websitesShown),
	});

	rpl::duplicate(
		updateTrigger
	) | rpl::on_next([=] {
		session->api().websites().reload();
	}, builder.container()->lifetime());

	auto sessionsCount = session->api().authorizations().totalValue(
	) | rpl::map([](int count) {
		return count ? QString::number(count) : QString();
	});

	builder.addButton({
		.id = u"security/sessions"_q,
		.title = tr::lng_settings_show_sessions(),
		.icon = { &st::menuIconDevices },
		.label = std::move(sessionsCount),
		.onClick = [=] {
			showOther(Sessions::Id());
		},
		.keywords = { u"sessions"_q, u"devices"_q, u"active"_q },
	});

	std::move(
		updateTrigger
	) | rpl::on_next([=] {
		session->api().authorizations().reload();
	}, builder.container()->lifetime());

	builder.addSkip();
	builder.addDividerText(tr::lng_settings_sessions_about());
}

void BuildPrivacySection(
		SectionBuilder &builder,
		not_null<Window::SessionController*> controller) {
	const auto session = &controller->session();

	builder.addSkip(st::settingsPrivacySkip);
	builder.addSubsectionTitle(tr::lng_settings_privacy_title());

	using Key = Privacy::Key;

	builder.addPrivacyButton({
		.id = u"privacy/phone_number"_q,
		.title = tr::lng_settings_phone_number_privacy(),
		.key = Key::PhoneNumber,
		.controllerFactory = [=] {
			return std::make_unique<PhoneNumberPrivacyController>(controller);
		},
		.keywords = { u"phone"_q, u"number"_q },
	});

	builder.addPrivacyButton({
		.id = u"privacy/last_seen"_q,
		.title = tr::lng_settings_last_seen(),
		.key = Key::LastSeen,
		.controllerFactory = [=] {
			return std::make_unique<LastSeenPrivacyController>(session);
		},
		.keywords = { u"last seen"_q, u"online"_q },
	});

	builder.addPrivacyButton({
		.id = u"privacy/profile_photo"_q,
		.title = tr::lng_settings_profile_photo_privacy(),
		.key = Key::ProfilePhoto,
		.controllerFactory = [] {
			return std::make_unique<ProfilePhotoPrivacyController>();
		},
		.keywords = { u"photo"_q, u"avatar"_q },
	});

	builder.addPrivacyButton({
		.id = u"privacy/forwards"_q,
		.title = tr::lng_settings_forwards_privacy(),
		.key = Key::Forwards,
		.controllerFactory = [=] {
			return std::make_unique<ForwardsPrivacyController>(controller);
		},
		.keywords = { u"forwards"_q, u"link"_q },
	});

	builder.addPrivacyButton({
		.id = u"privacy/calls"_q,
		.title = tr::lng_settings_calls(),
		.key = Key::Calls,
		.controllerFactory = [] {
			return std::make_unique<CallsPrivacyController>();
		},
		.keywords = { u"calls"_q, u"voice"_q },
	});

	builder.addPrivacyButton({
		.id = u"privacy/voices"_q,
		.title = tr::lng_settings_voices_privacy(),
		.key = Key::Voices,
		.controllerFactory = [=] {
			return std::make_unique<VoicesPrivacyController>(session);
		},
		.premium = true,
		.keywords = { u"voice"_q, u"messages"_q },
	});

	const auto privacy = &session->api().globalPrivacy();
	auto messagesLabel = rpl::combine(
		privacy->newRequirePremium(),
		privacy->newChargeStars()
	) | rpl::map([=](bool requirePremium, int chargeStars) {
		return chargeStars
			? tr::lng_edit_privacy_paid()
			: requirePremium
			? tr::lng_edit_privacy_contacts_and_premium()
			: tr::lng_edit_privacy_everyone();
	}) | rpl::flatten_latest();

	const auto messagesPremium = !session->appConfig().newRequirePremiumFree();
	const auto messagesButton = builder.addButton({
		.id = u"privacy/messages"_q,
		.title = tr::lng_settings_messages_privacy(),
		.st = &st::settingsButtonNoIcon,
		.label = rpl::duplicate(messagesLabel),
		.onClick = [=] {
			controller->show(Box(EditMessagesPrivacyBox, controller, QString()));
		},
		.keywords = { u"messages"_q, u"new"_q, u"unknown"_q },
	});
	if (messagesPremium && messagesButton) {
		AddPrivacyPremiumStar(
			messagesButton,
			session,
			std::move(messagesLabel),
			st::settingsButtonNoIcon.padding);
	}

	builder.addPrivacyButton({
		.id = u"privacy/birthday"_q,
		.title = tr::lng_settings_birthday_privacy(),
		.key = Key::Birthday,
		.controllerFactory = [] {
			return std::make_unique<BirthdayPrivacyController>();
		},
		.keywords = { u"birthday"_q, u"age"_q },
	});

	builder.addPrivacyButton({
		.id = u"privacy/gifts"_q,
		.title = tr::lng_settings_gifts_privacy(),
		.key = Key::GiftsAutoSave,
		.controllerFactory = [] {
			return std::make_unique<GiftsAutoSavePrivacyController>();
		},
		.keywords = { u"gifts"_q },
	});

	builder.addPrivacyButton({
		.id = u"privacy/bio"_q,
		.title = tr::lng_settings_bio_privacy(),
		.key = Key::About,
		.controllerFactory = [] {
			return std::make_unique<AboutPrivacyController>();
		},
		.keywords = { u"bio"_q, u"about"_q },
	});

	builder.addPrivacyButton({
		.id = u"privacy/saved_music"_q,
		.title = tr::lng_settings_saved_music_privacy(),
		.key = Key::SavedMusic,
		.controllerFactory = [] {
			return std::make_unique<SavedMusicPrivacyController>();
		},
		.keywords = { u"music"_q, u"saved"_q },
	});

	builder.addPrivacyButton({
		.id = u"privacy/groups"_q,
		.title = tr::lng_settings_groups_invite(),
		.key = Key::Invites,
		.controllerFactory = [] {
			return std::make_unique<GroupsInvitePrivacyController>();
		},
		.keywords = { u"groups"_q, u"invite"_q },
	});

	session->api().userPrivacy().reload(Privacy::Key::AddedByPhone);

	builder.addSkip(st::settingsPrivacySecurityPadding);
	builder.addDivider();
}

void BuildArchiveAndMuteSection(
		SectionBuilder &builder,
		not_null<Window::SessionController*> controller) {
	SetupArchiveAndMute(controller, builder.container(), builder.highlights());
}

void BuildBotsAndWebsitesSection(
		SectionBuilder &builder,
		not_null<Window::SessionController*> controller) {
	SetupBotsAndWebsites(controller, builder.container(), builder.highlights());
}

void BuildTopPeersSection(
		SectionBuilder &builder,
		not_null<Window::SessionController*> controller) {
	const auto session = &controller->session();

	builder.addSkip();
	builder.addSubsectionTitle(tr::lng_settings_top_peers_title());

	const auto toggle = builder.addToggle({
		.id = u"privacy/top_peers"_q,
		.title = tr::lng_settings_top_peers_suggest(),
		.st = &st::settingsButtonNoIcon,
		.toggled = rpl::single(
			rpl::empty
		) | rpl::then(
			session->topPeers().updates()
		) | rpl::map([=] {
			return !session->topPeers().disabled();
		}),
		.keywords = { u"suggest"_q, u"contacts"_q },
	});

	if (toggle) {
		toggle->toggledChanges(
		) | rpl::filter([=](bool enabled) {
			return enabled == session->topPeers().disabled();
		}) | rpl::on_next([=](bool enabled) {
			session->topPeers().toggleDisabled(!enabled);
		}, builder.container()->lifetime());
	}

	builder.addSkip();
	builder.addDividerText(tr::lng_settings_top_peers_about());
}

void BuildSelfDestructionSection(
		SectionBuilder &builder,
		not_null<Window::SessionController*> controller,
		rpl::producer<> updateTrigger) {
	const auto session = &controller->session();

	builder.addSkip();
	builder.addSubsectionTitle(tr::lng_settings_destroy_title());

	std::move(
		updateTrigger
	) | rpl::on_next([=] {
		session->api().selfDestruct().reload();
	}, builder.container()->lifetime());

	auto label = session->api().selfDestruct().daysAccountTTL(
	) | rpl::map(SelfDestructionBox::DaysLabel);

	builder.addButton({
		.id = u"privacy/self_destruct"_q,
		.title = tr::lng_settings_destroy_if(),
		.st = &st::settingsButtonNoIcon,
		.label = std::move(label),
		.onClick = [=] {
			controller->show(Box<SelfDestructionBox>(
				session,
				SelfDestructionBox::Type::Account,
				session->api().selfDestruct().daysAccountTTL()));
		},
		.keywords = { u"delete"_q, u"destroy"_q, u"inactive"_q },
	});

	builder.addSkip();
}

void BuildPrivacySecuritySectionContent(
		SectionBuilder &builder,
		Window::SessionController *controller,
		Fn<void(Type)> showOther) {
	if (!controller) {
		return;
	}

	auto updateOnTick = rpl::single(
	) | rpl::then(base::timer_each(kUpdateTimeout));
	const auto trigger = [&] {
		return rpl::duplicate(updateOnTick);
	};

	BuildSecuritySection(builder, controller, trigger(), showOther);
	BuildPrivacySection(builder, controller);
	BuildArchiveAndMuteSection(builder, controller);
	BuildBotsAndWebsitesSection(builder, controller);
	SetupConfirmationExtensions(controller, builder.container());
	BuildTopPeersSection(builder, controller);
	BuildSelfDestructionSection(builder, controller, trigger());
}

const auto kMeta = BuildHelper(PrivacySecurity::Id(), [](SectionBuilder &builder) {
	const auto controller = builder.controller();
	const auto showOther = builder.showOther();
	BuildPrivacySecuritySectionContent(builder, controller, showOther);
}, Main::Id());

} // namespace

SectionBuildMethod PrivacySecuritySection = kMeta.build;

} // namespace Settings::Builder
