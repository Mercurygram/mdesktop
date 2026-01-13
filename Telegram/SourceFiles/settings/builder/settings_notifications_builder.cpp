/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "settings/builder/settings_notifications_builder.h"

#include "api/api_authorizations.h"
#include "api/api_ringtones.h"
#include "apiwrap.h"
#include "base/platform/base_platform_info.h"
#include "boxes/ringtones_box.h"
#include "core/application.h"
#include "data/data_chat_filters.h"
#include "data/data_session.h"
#include "data/notify/data_notify_settings.h"
#include "data/notify/data_peer_notify_volume.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "mainwindow.h"
#include "platform/platform_notifications_manager.h"
#include "platform/platform_specific.h"
#include "settings/builder/settings_builder.h"
#include "settings/settings_main.h"
#include "settings/settings_notifications.h"
#include "settings/settings_notifications_common.h"
#include "settings/settings_notifications_type.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/continuous_sliders.h"
#include "ui/widgets/discrete_sliders.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/notifications_manager.h"
#include "window/window_session_controller.h"
#include "styles/style_settings.h"
#include "styles/style_menu_icons.h"

#include <QtGui/QGuiApplication>
#include <QtGui/QScreen>

namespace Settings::Builder {
namespace {

constexpr auto kDefaultDisplayIndex = -1;

using NotifyView = Core::Settings::NotifyView;
using ChangeType = Window::Notifications::ChangeType;

void BuildMultiAccountSection(
		SectionBuilder &builder,
		Window::SessionController *controller) {
	if (Core::App().domain().accounts().size() < 2) {
		return;
	}

	builder.addSubsectionTitle(tr::lng_settings_show_from());

	const auto container = builder.container();
	if (!container) {
		builder.addButton({
			.id = u"notifications/multi_account"_q,
			.title = tr::lng_settings_notify_all(),
			.toggled = rpl::single(Core::App().settings().notifyFromAll()),
			.keywords = { u"all accounts"_q, u"multiple"_q },
		});
		builder.addSkip();
		builder.addDividerText(tr::lng_settings_notify_all_about());
		builder.addSkip();
		return;
	}

	const auto fromAll = builder.addButton({
		.id = u"notifications/accounts"_q,
		.title = tr::lng_settings_notify_all(),
		.st = &st::settingsButtonNoIcon,
		.toggled = rpl::single(Core::App().settings().notifyFromAll()),
		.keywords = { u"all accounts"_q, u"multiple"_q },
	});

	if (fromAll) {
		fromAll->toggledChanges(
		) | rpl::filter([](bool checked) {
			return (checked != Core::App().settings().notifyFromAll());
		}) | rpl::on_next([=](bool checked) {
			Core::App().settings().setNotifyFromAll(checked);
			Core::App().saveSettingsDelayed();
			if (!checked) {
				auto &notifications = Core::App().notifications();
				const auto &list = Core::App().domain().accounts();
				for (const auto &[index, account] : list) {
					if (account.get() == &Core::App().domain().active()) {
						continue;
					} else if (const auto session = account->maybeSession()) {
						notifications.clearFromSession(session);
					}
				}
			}
		}, fromAll->lifetime());
	}

	builder.addSkip();
	builder.addDividerText(tr::lng_settings_notify_all_about());
	builder.addSkip();
}

void BuildGlobalNotificationsSection(
		SectionBuilder &builder,
		Window::SessionController *controller) {
	builder.addSubsectionTitle(tr::lng_settings_notify_global());

	const auto container = builder.container();
	const auto &settings = Core::App().settings();

	const auto desktopToggles = container
		? container->lifetime().make_state<rpl::event_stream<bool>>()
		: nullptr;
	const auto desktop = builder.addButton({
		.id = u"notifications/desktop"_q,
		.title = tr::lng_settings_desktop_notify(),
		.icon = { &st::menuIconNotifications },
		.toggled = desktopToggles
			? desktopToggles->events_starting_with(settings.desktopNotify())
			: rpl::single(settings.desktopNotify()),
		.keywords = { u"desktop"_q, u"popup"_q, u"show"_q },
	});

	const auto flashbounceToggles = container
		? container->lifetime().make_state<rpl::event_stream<bool>>()
		: nullptr;
	const auto flashbounce = builder.addButton({
		.id = u"notifications/flash"_q,
		.title = (Platform::IsWindows()
			? tr::lng_settings_alert_windows
			: Platform::IsMac()
			? tr::lng_settings_alert_mac
			: tr::lng_settings_alert_linux)(),
		.icon = { &st::menuIconDockBounce },
		.toggled = flashbounceToggles
			? flashbounceToggles->events_starting_with(settings.flashBounceNotify())
			: rpl::single(settings.flashBounceNotify()),
		.keywords = { u"flash"_q, u"bounce"_q, u"taskbar"_q },
	});

	const auto soundAllowed = container
		? container->lifetime().make_state<rpl::event_stream<bool>>()
		: nullptr;
	const auto allowed = [=] {
		return Core::App().settings().soundNotify();
	};
	const auto sound = builder.addButton({
		.id = u"notifications/sound"_q,
		.title = tr::lng_settings_sound_allowed(),
		.icon = { &st::menuIconUnmute },
		.toggled = soundAllowed
			? soundAllowed->events_starting_with(allowed())
			: rpl::single(allowed()),
		.keywords = { u"sound"_q, u"audio"_q, u"mute"_q },
	});

	if (container && controller) {
		const auto session = &controller->session();
		Ui::AddRingtonesVolumeSlider(
			container,
			rpl::single(true),
			tr::lng_settings_master_volume_notifications(),
			Data::VolumeController{
				.volume = []() -> ushort {
					const auto volume
						= Core::App().settings().notificationsVolume();
					return volume ? volume : 100;
				},
				.saveVolume = [=](ushort volume) {
					Core::App().notifications().playSound(
						session,
						0,
						volume / 100.);
					Core::App().settings().setNotificationsVolume(volume);
					Core::App().saveSettingsDelayed();
				}});
	}

	builder.addSkip();

	if (desktop) {
		const auto changed = [=](ChangeType change) {
			Core::App().saveSettingsDelayed();
			Core::App().notifications().notifySettingsChanged(change);
		};

		desktop->toggledChanges(
		) | rpl::filter([](bool checked) {
			return (checked != Core::App().settings().desktopNotify());
		}) | rpl::on_next([=](bool checked) {
			Core::App().settings().setDesktopNotify(checked);
			changed(ChangeType::DesktopEnabled);
		}, desktop->lifetime());

		if (sound) {
			sound->toggledChanges(
			) | rpl::filter([](bool checked) {
				return (checked != Core::App().settings().soundNotify());
			}) | rpl::on_next([=](bool checked) {
				Core::App().settings().setSoundNotify(checked);
				changed(ChangeType::SoundEnabled);
			}, sound->lifetime());
		}

		if (flashbounce) {
			flashbounce->toggledChanges(
			) | rpl::filter([](bool checked) {
				return (checked != Core::App().settings().flashBounceNotify());
			}) | rpl::on_next([=](bool checked) {
				Core::App().settings().setFlashBounceNotify(checked);
				changed(ChangeType::FlashBounceEnabled);
			}, flashbounce->lifetime());
		}

		Core::App().notifications().settingsChanged(
		) | rpl::on_next([=](ChangeType change) {
			if (change == ChangeType::DesktopEnabled) {
				desktopToggles->fire(Core::App().settings().desktopNotify());
			} else if (change == ChangeType::SoundEnabled) {
				soundAllowed->fire(allowed());
			} else if (change == ChangeType::FlashBounceEnabled) {
				flashbounceToggles->fire(
					Core::App().settings().flashBounceNotify());
			}
		}, desktop->lifetime());
	}
}

void BuildNotifyViewSection(
		SectionBuilder &builder,
		Window::SessionController *controller,
		rpl::lifetime &lifetime) {
	const auto container = builder.container();
	if (!container || !controller) {
		return;
	}

	const auto &settings = Core::App().settings();
	const auto checkboxes = SetupNotifyViewOptions(
		controller,
		container,
		(settings.notifyView() <= NotifyView::ShowName),
		(settings.notifyView() <= NotifyView::ShowPreview));
	const auto name = checkboxes.name;
	const auto preview = checkboxes.preview;
	const auto previewWrap = checkboxes.wrap;

	const auto previewDivider = container->add(
		object_ptr<Ui::SlideWrap<Ui::BoxContentDivider>>(
			container,
			object_ptr<Ui::BoxContentDivider>(container)));
	previewWrap->toggle(settings.desktopNotify(), anim::type::instant);
	previewDivider->toggle(!settings.desktopNotify(), anim::type::instant);

	const auto changed = [=](ChangeType change) {
		Core::App().saveSettingsDelayed();
		Core::App().notifications().notifySettingsChanged(change);
	};

	name->checkedChanges(
	) | rpl::map([=](bool checked) {
		if (!checked) {
			preview->setChecked(false);
			return NotifyView::ShowNothing;
		} else if (!preview->checked()) {
			return NotifyView::ShowName;
		}
		return NotifyView::ShowPreview;
	}) | rpl::filter([=](NotifyView value) {
		return (value != Core::App().settings().notifyView());
	}) | rpl::on_next([=](NotifyView value) {
		Core::App().settings().setNotifyView(value);
		changed(ChangeType::ViewParams);
	}, name->lifetime());

	preview->checkedChanges(
	) | rpl::map([=](bool checked) {
		if (checked) {
			name->setChecked(true);
			return NotifyView::ShowPreview;
		} else if (name->checked()) {
			return NotifyView::ShowName;
		}
		return NotifyView::ShowNothing;
	}) | rpl::filter([=](NotifyView value) {
		return (value != Core::App().settings().notifyView());
	}) | rpl::on_next([=](NotifyView value) {
		Core::App().settings().setNotifyView(value);
		changed(ChangeType::ViewParams);
	}, preview->lifetime());

	Core::App().notifications().settingsChanged(
	) | rpl::on_next([=](ChangeType change) {
		if (change == ChangeType::DesktopEnabled) {
			previewWrap->toggle(
				Core::App().settings().desktopNotify(),
				anim::type::normal);
			previewDivider->toggle(
				!Core::App().settings().desktopNotify(),
				anim::type::normal);
		}
	}, lifetime);
}

void BuildNotifyTypeSection(
		SectionBuilder &builder,
		Window::SessionController *controller) {
	const auto container = builder.container();
	const auto showOther = builder.showOther();

	builder.addSkip(st::notifyPreviewBottomSkip);
	builder.addSubsectionTitle(tr::lng_settings_notify_title());

	if (container && controller && showOther) {
		controller->session().data().notifySettings().loadExceptions();
		AddTypeButton(container, controller, Data::DefaultNotify::User, showOther);
		AddTypeButton(container, controller, Data::DefaultNotify::Group, showOther);
		AddTypeButton(container, controller, Data::DefaultNotify::Broadcast, showOther);
	} else {
		builder.addButton({
			.id = u"notifications/private"_q,
			.title = tr::lng_notification_private_chats(),
			.icon = { &st::menuIconProfile },
			.keywords = { u"private"_q, u"chats"_q, u"direct"_q },
		});
		builder.addButton({
			.id = u"notifications/groups"_q,
			.title = tr::lng_notification_groups(),
			.icon = { &st::menuIconGroups },
			.keywords = { u"groups"_q, u"chats"_q },
		});
		builder.addButton({
			.id = u"notifications/channels"_q,
			.title = tr::lng_notification_channels(),
			.icon = { &st::menuIconChannel },
			.keywords = { u"channels"_q, u"broadcast"_q },
		});
	}
}

void BuildEventNotificationsSection(
		SectionBuilder &builder,
		Window::SessionController *controller) {
	builder.addSkip(st::settingsCheckboxesSkip);
	builder.addDivider();
	builder.addSkip(st::settingsCheckboxesSkip);
	builder.addSubsectionTitle(tr::lng_settings_events_title());

	if (!controller) {
		return;
	}

	const auto session = &controller->session();
	const auto &settings = Core::App().settings();

	auto joinSilent = rpl::single(
		session->api().contactSignupSilentCurrent().value_or(false)
	) | rpl::then(session->api().contactSignupSilent());

	const auto joined = builder.addButton({
		.id = u"notifications/events/joined"_q,
		.title = tr::lng_settings_events_joined(),
		.icon = { &st::menuIconInvite },
		.toggled = std::move(joinSilent) | rpl::map([](bool s) { return !s; }),
		.keywords = { u"joined"_q, u"contacts"_q, u"signup"_q },
	});
	if (joined) {
		joined->toggledChanges(
		) | rpl::filter([=](bool enabled) {
			const auto silent = session->api().contactSignupSilentCurrent();
			return (enabled == silent.value_or(false));
		}) | rpl::on_next([=](bool enabled) {
			session->api().saveContactSignupSilent(!enabled);
		}, joined->lifetime());
	}

	const auto pinned = builder.addButton({
		.id = u"notifications/events/pinned"_q,
		.title = tr::lng_settings_events_pinned(),
		.icon = { &st::menuIconPin },
		.toggled = rpl::single(
			settings.notifyAboutPinned()
		) | rpl::then(settings.notifyAboutPinnedChanges()),
		.keywords = { u"pinned"_q, u"message"_q },
	});
	if (pinned) {
		pinned->toggledChanges(
		) | rpl::filter([=](bool notify) {
			return (notify != Core::App().settings().notifyAboutPinned());
		}) | rpl::on_next([=](bool notify) {
			Core::App().settings().setNotifyAboutPinned(notify);
			Core::App().saveSettingsDelayed();
		}, pinned->lifetime());
	}
}

void BuildCallNotificationsSection(
		SectionBuilder &builder,
		Window::SessionController *controller) {
	builder.addSkip(st::settingsCheckboxesSkip);
	builder.addDivider();
	builder.addSkip(st::settingsCheckboxesSkip);
	builder.addSubsectionTitle(tr::lng_settings_notifications_calls_title());

	if (!controller) {
		return;
	}

	const auto session = &controller->session();
	const auto authorizations = &session->api().authorizations();
	authorizations->reload();

	const auto acceptCalls = builder.addButton({
		.id = u"notifications/calls/accept"_q,
		.title = tr::lng_settings_call_accept_calls(),
		.icon = { &st::menuIconCallsReceive },
		.toggled = authorizations->callsDisabledHereValue()
			| rpl::map([](bool disabled) { return !disabled; }),
		.keywords = { u"calls"_q, u"receive"_q, u"incoming"_q },
	});
	if (acceptCalls) {
		const auto container = builder.container();
		acceptCalls->toggledChanges(
		) | rpl::filter([=](bool toggled) {
			return (toggled == authorizations->callsDisabledHere());
		}) | rpl::on_next([=](bool toggled) {
			authorizations->toggleCallsDisabledHere(!toggled);
		}, container->lifetime());
	}
}

void BuildBadgeCounterSection(
		SectionBuilder &builder,
		Window::SessionController *controller) {
	builder.addSkip(st::settingsCheckboxesSkip);
	builder.addDivider();
	builder.addSkip(st::settingsCheckboxesSkip);
	builder.addSubsectionTitle(tr::lng_settings_badge_title());

	const auto &settings = Core::App().settings();

	const auto muted = builder.addButton({
		.id = u"notifications/include-muted-chats"_q,
		.title = tr::lng_settings_include_muted(),
		.st = &st::settingsButtonNoIcon,
		.toggled = rpl::single(settings.includeMutedCounter()),
		.keywords = { u"muted"_q, u"badge"_q, u"counter"_q },
	});

	const auto hasFolders = controller
		&& controller->session().data().chatsFilters().has();
	const auto mutedFolders = hasFolders ? builder.addButton({
		.id = u"notifications/badge/muted_folders"_q,
		.title = tr::lng_settings_include_muted_folders(),
		.st = &st::settingsButtonNoIcon,
		.toggled = rpl::single(settings.includeMutedCounterFolders()),
		.keywords = { u"muted"_q, u"folders"_q },
	}) : nullptr;

	const auto count = builder.addButton({
		.id = u"notifications/count-unread-messages"_q,
		.title = tr::lng_settings_count_unread(),
		.st = &st::settingsButtonNoIcon,
		.toggled = rpl::single(settings.countUnreadMessages()),
		.keywords = { u"unread"_q, u"messages"_q, u"count"_q },
	});

	const auto changed = [=](ChangeType change) {
		Core::App().saveSettingsDelayed();
		Core::App().notifications().notifySettingsChanged(change);
	};

	if (muted) {
		muted->toggledChanges(
		) | rpl::filter([=](bool checked) {
			return (checked != Core::App().settings().includeMutedCounter());
		}) | rpl::on_next([=](bool checked) {
			Core::App().settings().setIncludeMutedCounter(checked);
			changed(ChangeType::IncludeMuted);
		}, muted->lifetime());
	}

	if (mutedFolders) {
		mutedFolders->toggledChanges(
		) | rpl::filter([=](bool checked) {
			return (checked
				!= Core::App().settings().includeMutedCounterFolders());
		}) | rpl::on_next([=](bool checked) {
			Core::App().settings().setIncludeMutedCounterFolders(checked);
			changed(ChangeType::IncludeMuted);
		}, mutedFolders->lifetime());
	}

	if (count) {
		count->toggledChanges(
		) | rpl::filter([=](bool checked) {
			return (checked != Core::App().settings().countUnreadMessages());
		}) | rpl::on_next([=](bool checked) {
			Core::App().settings().setCountUnreadMessages(checked);
			changed(ChangeType::CountMessages);
		}, count->lifetime());
	}
}

void BuildSystemIntegrationAndAdvancedSection(
		SectionBuilder &builder,
		Window::SessionController *controller) {
	const auto container = builder.container();

	auto nativeText = [&]() -> rpl::producer<QString> {
		if (!Platform::Notifications::Supported()
			|| Core::App().notifications().nativeEnforced()) {
			return rpl::producer<QString>();
		} else if (Platform::IsWindows()) {
			return tr::lng_settings_use_windows();
		}
		return tr::lng_settings_use_native_notifications();
	}();

	if (nativeText) {
		builder.addSkip(st::settingsCheckboxesSkip);
		builder.addDivider();
		builder.addSkip(st::settingsCheckboxesSkip);
		builder.addSubsectionTitle(tr::lng_settings_native_title());
	}

	const auto &settings = Core::App().settings();
	const auto native = nativeText ? builder.addButton({
		.id = u"notifications/native"_q,
		.title = std::move(nativeText),
		.st = &st::settingsButtonNoIcon,
		.toggled = rpl::single(settings.nativeNotifications()),
		.keywords = { u"native"_q, u"system"_q, u"windows"_q },
	}) : nullptr;

	if (Core::App().notifications().nativeEnforced()) {
		return;
	}
	if (!container || !controller) {
		return;
	}

	const auto advancedSlide = container->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			container,
			object_ptr<Ui::VerticalLayout>(container)));
	const auto advancedWrap = advancedSlide->entity();

	if (native) {
		native->toggledChanges(
		) | rpl::filter([](bool checked) {
			return (checked != Core::App().settings().nativeNotifications());
		}) | rpl::on_next([=](bool checked) {
			Core::App().settings().setNativeNotifications(checked);
			Core::App().saveSettingsDelayed();
			Core::App().notifications().createManager();
			advancedSlide->toggle(!checked, anim::type::normal);
		}, native->lifetime());
	}

	if (Platform::IsWindows()) {
		const auto skipInFocus = advancedWrap->add(object_ptr<Ui::SettingsButton>(
			advancedWrap,
			tr::lng_settings_skip_in_focus(),
			st::settingsButtonNoIcon
		))->toggleOn(rpl::single(Core::App().settings().skipToastsInFocus()));

		skipInFocus->toggledChanges(
		) | rpl::filter([](bool checked) {
			return (checked != Core::App().settings().skipToastsInFocus());
		}) | rpl::on_next([=](bool checked) {
			Core::App().settings().setSkipToastsInFocus(checked);
			Core::App().saveSettingsDelayed();
			if (checked && Platform::Notifications::SkipToastForCustom()) {
				Core::App().notifications().notifySettingsChanged(
					ChangeType::DesktopEnabled);
			}
		}, skipInFocus->lifetime());
	}

	const auto screens = QGuiApplication::screens();
	if (screens.size() > 1) {
		Ui::AddSkip(advancedWrap, st::settingsCheckboxesSkip);
		Ui::AddDivider(advancedWrap);
		Ui::AddSkip(advancedWrap, st::settingsCheckboxesSkip);
		Ui::AddSubsectionTitle(
			advancedWrap,
			tr::lng_settings_notifications_display());

		const auto currentChecksum
			= Core::App().settings().notificationsDisplayChecksum();
		auto currentIndex = (currentChecksum == 0)
			? kDefaultDisplayIndex
			: 0;
		for (auto i = 0; i < screens.size(); ++i) {
			if (Platform::ScreenNameChecksum(screens[i]) == currentChecksum) {
				currentIndex = i;
				break;
			}
		}

		const auto group = std::make_shared<Ui::RadiobuttonGroup>(
			currentIndex);

		advancedWrap->add(
			object_ptr<Ui::Radiobutton>(
				advancedWrap,
				group,
				kDefaultDisplayIndex,
				tr::lng_settings_notifications_display_default(tr::now),
				st::settingsSendType),
			st::settingsSendTypePadding);

		for (auto i = 0; i < screens.size(); ++i) {
			const auto &screen = screens[i];
			const auto name = Platform::ScreenDisplayLabel(screen);
			const auto geometry = screen->geometry();
			const auto resolution = QString::number(geometry.width())
				+ QChar(0x00D7)
				+ QString::number(geometry.height());
			const auto label = name.isEmpty()
				? QString("Display (%1)").arg(resolution)
				: QString("%1 (%2)").arg(name).arg(resolution);
			advancedWrap->add(
				object_ptr<Ui::Radiobutton>(
					advancedWrap,
					group,
					i,
					label,
					st::settingsSendType),
				st::settingsSendTypePadding);
		}
		group->setChangedCallback([=](int selectedIndex) {
			if (selectedIndex == kDefaultDisplayIndex) {
				Core::App().settings().setNotificationsDisplayChecksum(0);
				Core::App().saveSettings();
				Core::App().notifications().notifySettingsChanged(
					ChangeType::Corner);
			} else {
				const auto screens = QGuiApplication::screens();
				if (selectedIndex >= 0 && selectedIndex < screens.size()) {
					const auto checksum = Platform::ScreenNameChecksum(
						screens[selectedIndex]);
					Core::App().settings().setNotificationsDisplayChecksum(
						checksum);
					Core::App().saveSettings();
					Core::App().notifications().notifySettingsChanged(
						ChangeType::Corner);
				}
			}
		});
	}

	Ui::AddSkip(advancedWrap, st::settingsCheckboxesSkip);
	Ui::AddDivider(advancedWrap);
	Ui::AddSkip(advancedWrap, st::settingsCheckboxesSkip);
	Ui::AddSubsectionTitle(
		advancedWrap,
		tr::lng_settings_notifications_position());
	Ui::AddSkip(advancedWrap, st::settingsCheckboxesSkip);

	const auto position = advancedWrap->add(
		object_ptr<NotificationsCount>(advancedWrap, controller));

	Ui::AddSkip(advancedWrap, st::settingsCheckboxesSkip);
	Ui::AddSubsectionTitle(advancedWrap, tr::lng_settings_notifications_count());

	const auto countSlider = advancedWrap->add(
		object_ptr<Ui::SettingsSlider>(advancedWrap, st::settingsSlider),
		st::settingsBigScalePadding);
	for (int i = 0; i != kMaxNotificationsCount; ++i) {
		countSlider->addSection(QString::number(i + 1));
	}
	countSlider->setActiveSectionFast(CurrentNotificationsCount() - 1);
	countSlider->sectionActivated(
	) | rpl::on_next([=](int section) {
		position->setCount(section + 1);
	}, countSlider->lifetime());
	Ui::AddSkip(advancedWrap, st::settingsCheckboxesSkip);

	if (Core::App().settings().nativeNotifications()) {
		advancedSlide->hide(anim::type::instant);
	}

	Core::App().notifications().settingsChanged(
	) | rpl::on_next([=](ChangeType change) {
		if (change == ChangeType::DesktopEnabled) {
			const auto native = Core::App().settings().nativeNotifications();
			advancedSlide->toggle(!native, anim::type::normal);
		}
	}, advancedSlide->lifetime());
}

void BuildNotificationsSectionContent(
		SectionBuilder &builder,
		Window::SessionController *controller) {
	const auto container = builder.container();

	builder.addSkip(st::settingsPrivacySkip);

	BuildMultiAccountSection(builder, controller);
	BuildGlobalNotificationsSection(builder, controller);

	if (container && controller) {
		BuildNotifyViewSection(builder, controller, container->lifetime());
	}

	BuildNotifyTypeSection(builder, controller);
	BuildEventNotificationsSection(builder, controller);
	BuildCallNotificationsSection(builder, controller);
	BuildBadgeCounterSection(builder, controller);
	BuildSystemIntegrationAndAdvancedSection(builder, controller);
}

const auto kMeta = BuildHelper(
	Notifications::Id(),
	tr::lng_settings_section_notify,
	[](SectionBuilder &builder) {
		const auto controller = builder.controller();
		BuildNotificationsSectionContent(builder, controller);
	},
	Main::Id());

} // namespace

SectionBuildMethod NotificationsSection = kMeta.build;

} // namespace Settings::Builder
