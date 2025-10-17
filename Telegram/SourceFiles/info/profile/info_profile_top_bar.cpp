/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "info/profile/info_profile_top_bar.h"

#include "api/api_user_privacy.h"
#include "apiwrap.h"
#include "base/timer_rpl.h"
#include "base/timer.h"
#include "base/unixtime.h"
#include "calls/calls_instance.h"
#include "chat_helpers/stickers_lottie.h"
#include "core/application.h"
#include "core/shortcuts.h"
#include "data/components/recent_shared_media_gifts.h"
#include "data/data_changes.h"
#include "data/data_channel.h"
#include "data/data_document_media.h"
#include "data/data_document.h"
#include "data/data_emoji_statuses.h"
#include "data/data_forum_topic.h"
#include "data/data_peer_values.h"
#include "data/data_peer.h"
#include "data/data_photo.h"
#include "data/data_saved_sublist.h"
#include "data/data_session.h"
#include "data/data_star_gift.h"
#include "data/data_stories.h"
#include "data/data_user.h"
#include "data/notify/data_notify_settings.h"
#include "data/notify/data_peer_notify_settings.h"
#include "data/stickers/data_custom_emoji.h"
#include "history/history.h"
#include "info_profile_actions.h"
#include "info/info_controller.h"
#include "info/info_memento.h"
#include "info/profile/info_profile_badge_tooltip.h"
#include "info/profile/info_profile_badge.h"
#include "info/profile/info_profile_cover.h" // LargeCustomEmojiMargins
#include "info/profile/info_profile_status_label.h"
#include "info/profile/info_profile_top_bar_action_button.h"
#include "info/profile/info_profile_values.h"
#include "lang/lang_keys.h"
#include "lottie/lottie_animation.h"
#include "lottie/lottie_multi_player.h"
#include "main/main_session.h"
#include "menu/menu_mute.h"
#include "settings/settings_credits_graphics.h"
#include "settings/settings_information.h"
#include "settings/settings_premium.h"
#include "ui/boxes/show_or_premium_box.h"
#include "ui/color_contrast.h"
#include "ui/controls/stars_rating.h"
#include "ui/effects/animations.h"
#include "ui/empty_userpic.h"
#include "ui/layers/generic_box.h"
#include "ui/peer/video_userpic_player.h"
#include "ui/painter.h"
#include "ui/rect.h"
#include "ui/top_background_gradient.h"
#include "ui/ui_utility.h"
#include "ui/effects/outline_segments.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/horizontal_fit_container.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/menu/menu_add_action_callback_factory.h"
#include "ui/widgets/popup_menu.h"
#include "ui/wrap/fade_wrap.h"
#include "window/window_peer_menu.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_chat.h"
#include "styles/style_chat_helpers.h"
#include "styles/style_info.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"

#include <QGraphicsOpacityEffect>

namespace Info::Profile {
namespace {

constexpr auto kWaitBeforeGiftBadge = crl::time(1000);
constexpr auto kGiftBadgeGlares = 3;
constexpr auto kMinPatternRadius = 8;
constexpr auto kMinContrast = 5.5;
constexpr auto kStoryOutlineFadeEnd = 0.4;
constexpr auto kStoryOutlineFadeRange = 1. - kStoryOutlineFadeEnd;

using AnimatedPatternPoint = TopBar::AnimatedPatternPoint;

[[nodiscard]] std::vector<AnimatedPatternPoint> GenerateAnimatedPattern(
		const QRect &userpicRect) {
	auto points = std::vector<TopBar::AnimatedPatternPoint>();
	points.reserve(18); // 6 + 6 + 4 + 2.
	const auto ax = float64(userpicRect.x());
	const auto ay = float64(userpicRect.y());
	const auto aw = float64(userpicRect.width());
	const auto ah = float64(userpicRect.height());
	const auto acx = ax + aw / 2.;
	const auto acy = ay + ah / 2.;

	const auto padding24 = 24.;
	const auto padding16 = 16.;
	const auto padding8 = 8.;
	const auto padding12 = 12.;
	const auto padding48 = 48.;
	const auto padding96 = 96.;
	static const auto kCos120 = std::cos(M_PI * 120. / 180.);
	static const auto kCos160 = std::cos(M_PI * 160. / 180.);
	const auto r48Cos120 = (padding48 + aw / 2.) * kCos120;
	const auto r16Cos160 = (padding16 + ah / 2.) * kCos160;

	// First ring.
	points.push_back({ { acx, ay - padding24 }, 20, 0.02, 0.42 });
	points.push_back({ { acx, ay + ah + padding24 }, 20, 0.00, 0.32 });
	points.push_back({ { ax - padding16, acy - ah / 4 - padding8 }, 23, 0.00, 0.40 });
	points.push_back({ { ax + aw + padding16, acy - ah / 4 - padding8 }, 18, 0.00, 0.40 });
	points.push_back({ { ax - padding16, acy + ah / 4 + padding8 }, 24, 0.00, 0.40 });
	points.push_back({ { ax + aw + padding16 - 4, acy + ah / 4 + padding8 }, 24, 0.00, 0.40 });

	// Second ring.
	points.push_back({ { ax - padding48, acy }, 19, 0.14, 0.60 });
	points.push_back({ { ax + aw + padding48, acy }, 19, 0.16, 0.64 });
	points.push_back({ { acx + r48Cos120, ay - padding48 + padding12 }, 17, 0.14, 0.70 });
	points.push_back({ { acx - r48Cos120, ay - padding48 + padding12 }, 17, 0.14, 0.90 });
	points.push_back({ { acx + r48Cos120, ay + ah + padding48 - padding12 }, 20, 0.20, 0.75 });
	points.push_back({ { acx - r48Cos120, ay + ah + padding48 - padding12 }, 20, 0.20, 0.85 });

	// Third ring.
	points.push_back({ { ax - padding48 - padding8, acy + r16Cos160 }, 20, 0.09, 0.45 });
	points.push_back({ { ax + aw + padding48 + padding8, acy + r16Cos160 }, 19, 0.09, 0.45 });
	points.push_back({ { ax - padding48 - padding8, acy - r16Cos160 }, 21, 0.09, 0.45 });
	points.push_back({ { ax + aw + padding48 + padding8, acy - r16Cos160 }, 18, 0.11, 0.45 });

	// Fourth ring.
	points.push_back({ { ax - padding96, acy }, 19, 0.14, 0.75 });
	points.push_back({ { ax + aw + padding96, acy }, 19, 0.20, 0.80 });

	return points;
}

} // namespace

TopBar::TopBar(
	not_null<Ui::RpWidget*> parent,
	Descriptor descriptor)
: RpWidget(parent)
, _peer(descriptor.peer
	? descriptor.peer
	: descriptor.controller->key().peer())
, _topic(descriptor.controller->key().topic())
, _st(st::infoTopBar)
, _badgeTooltipHide(
	std::make_unique<base::Timer>([=] { hideBadgeTooltip(); }))
, _botVerify(std::make_unique<Badge>(
	this,
	st::infoBotVerifyBadge,
	&_peer->session(),
	BotVerifyBadgeForPeer(_peer),
	nullptr,
	Fn<bool()>([=, controller = descriptor.controller->parentController()] {
		return controller->isGifPausedAtLeastFor(
			Window::GifPauseReason::Layer);
	})))
, _badgeContent(BadgeContentForPeer(_peer))
, _gifPausedChecker([=,
		controller = descriptor.controller->parentController()] {
	return controller->isGifPausedAtLeastFor(
		Window::GifPauseReason::Layer);
})
, _badge(std::make_unique<Badge>(
	this,
	st::infoPeerBadge,
	&_peer->session(),
	_badgeContent.value(),
	nullptr,
	_gifPausedChecker))
, _verified(std::make_unique<Badge>(
	this,
	st::infoPeerBadge,
	&_peer->session(),
	VerifiedContentForPeer(_peer),
	nullptr,
	_gifPausedChecker))
, _title(this, Info::Profile::NameValue(_peer), _st.title)
, _starsRating(_peer->isUser()
	? std::make_unique<Ui::StarsRating>(
		this,
		descriptor.controller->uiShow(),
		_peer->isSelf() ? QString() : _peer->shortName(),
		Data::StarsRatingValue(_peer),
		(_peer->isSelf()
			? [=] { return _peer->owner().pendingStarsRating(); }
			: Fn<Data::StarsRatingPending()>()))
	: nullptr)
, _status(
	this,
	QString(),
	_peer->isMegagroup()
		? st::infoProfileMegagroupCover.status
		: st::infoProfileCover.status)
, _statusLabel(std::make_unique<StatusLabel>(_status.data(), _peer))
, _showLastSeen(
		this,
		tr::lng_status_lastseen_when(),
		st::infoProfileCover.showLastSeen) {
	QWidget::setMinimumHeight(st::infoLayerTopBarHeight);
	QWidget::setMaximumHeight(st::infoProfileTopBarHeightMax);

	const auto controller = descriptor.controller;

	if (_peer->isMegagroup() || _peer->isChat()) {
		_statusLabel->setMembersLinkCallback([=, peer = _peer] {
			const auto topic = controller->key().topic();
			const auto sublist = controller->key().sublist();
			const auto shown = sublist
				? sublist->sublistPeer().get()
				: peer.get();
			const auto section = Section::Type::Members;
			controller->showSection(topic
				? std::make_shared<Info::Memento>(topic, section)
				: std::make_shared<Info::Memento>(shown, section));
		});
	}
	if (!_peer->isMegagroup()) {
		_status->setAttribute(Qt::WA_TransparentForMouseEvents);
		if (const auto rating = _starsRating.get()) {
			_statusShift = rating->widthValue();
			_statusShift.changes() | rpl::start_with_next([=] {
				updateLabelsPosition();
			}, _status->lifetime());
			rating->raise();
		}
	}

	setupShowLastSeen(controller);

	_peer->session().changes().peerFlagsValue(
		_peer,
		Data::PeerUpdate::Flag::OnlineStatus | Data::PeerUpdate::Flag::Members
	) | rpl::start_with_next([=] {
		_statusLabel->refresh();
	}, lifetime());

	_status->widthValue() | rpl::start_with_next([=] {
		updateLabelsPosition();
	}, _status->lifetime());

	auto badgeUpdates = rpl::producer<rpl::empty_value>();
	if (_badge) {
		badgeUpdates = rpl::merge(
			std::move(badgeUpdates),
			_badge->updated());
	}
	if (_verified) {
		badgeUpdates = rpl::merge(
			std::move(badgeUpdates),
			_verified->updated());
	}
	if (_botVerify) {
		badgeUpdates = rpl::merge(
			std::move(badgeUpdates),
			_botVerify->updated());
	}
	std::move(badgeUpdates) | rpl::start_with_next([=] {
		updateLabelsPosition();
	}, _title->lifetime());

	setupUniqueBadgeTooltip();
	setupButtons(
		controller,
		descriptor.backToggles.value(),
		descriptor.source);
	setupUserpicButton(controller);
	setupActions(controller);
	setupStoryOutline();
	if (_topic) {
		_topicIconView = std::make_unique<TopicIconView>(
			_topic,
			_gifPausedChecker,
			[=] { update(); });
	} else {
		updateVideoUserpic();
	}

	_peer->session().changes().peerFlagsValue(
		_peer,
		Data::PeerUpdate::Flag::EmojiStatus
	) | rpl::start_with_next([=] {
		if (_pinnedToTopGiftsFirstTimeShowed) {
			_peer->session().recentSharedGifts().clearLastRequestTime(_peer);
			setupPinnedToTopGifts(controller);
		} else {
			updateCollectibleStatus();
		}
	}, lifetime());

	std::move(
		descriptor.showFinished
	) | rpl::take(1) | rpl::start_with_next([=] {
		setupPinnedToTopGifts(controller);
	}, lifetime());
}

void TopBar::adjustColors(const std::optional<QColor> &edgeColor) {
	constexpr auto kMinContrast = 5.5;
	const auto shouldOverride = [&](const style::color &color) {
		return edgeColor
			&& (kMinContrast > Ui::CountContrast(color->c, *edgeColor));
	};
	const auto shouldOverrideTitle = shouldOverride(_title->st().textFg);
	const auto shouldOverrideStatus = shouldOverride(_status->st().textFg);
	_title->setTextColorOverride(shouldOverrideTitle
		? std::optional<QColor>(st::groupCallMembersFg->c)
		: std::nullopt);
	_status->setTextColorOverride(shouldOverrideStatus
		? std::optional<QColor>(st::groupCallVideoSubTextFg->c)
		: std::nullopt);
	_statusLabel->setColorized(!shouldOverrideStatus);

	const auto shouldOverrideBadges = shouldOverride(
		st::infoBotVerifyBadge.premiumFg);
	_botVerify->setOverrideStyle(shouldOverrideBadges
		? &st::infoColoredBotVerifyBadge
		: nullptr);
	_badge->setOverrideStyle(shouldOverrideBadges
		? &st::infoColoredPeerBadge
		: nullptr);
	_verified->setOverrideStyle(shouldOverrideBadges
		? &st::infoColoredPeerBadge
		: nullptr);

	if (_starsRating) {
		const auto shouldOverrideRating = shouldOverride(st::windowBgActive);
		_starsRating->setCustomColors(
			shouldOverrideRating
				? edgeColor
				: std::nullopt,
			shouldOverrideRating
				? std::make_optional<QColor>(st::windowFgActive->c)
				: std::nullopt);
	}

	_edgeColor = edgeColor;
}

void TopBar::updateCollectibleStatus() {
	const auto collectible = _peer->emojiStatusId().collectible;
	_hasBackground = collectible != nullptr;
	_cachedClipPath = QPainterPath();
	_cachedGradient = QImage();
	_basePatternImage = QImage();
	_lastUserpicRect = QRect();
	_patternEmojis.clear();
	if (collectible && collectible->patternDocumentId) {
		const auto document = _peer->owner().document(
			collectible->patternDocumentId);
		_patternEmoji = document->owner().customEmojiManager().create(
			document,
			[=] { update(); },
			Data::CustomEmojiSizeTag::Large);
		setupAnimatedPattern();
	} else {
		_patternEmoji = nullptr;
		_animatedPoints.clear();
		_pinnedToTopGifts.clear();
	}
	update();
	adjustColors(collectible
		? std::optional<QColor>(collectible->edgeColor)
		: std::nullopt);
}

void TopBar::setupActions(not_null<Controller*> controller) {
	const auto peer = _peer;
	const auto user = peer->asUser();
	const auto channel = peer->asChannel();
	const auto chat = peer->asChat();
	const auto topic = controller->key().topic();
	const auto mapped = [=](std::optional<QColor> c) {
		return !c
			? anim::with_alpha(
				st::activeButtonBg->c,
				1. - st::infoProfileTopBarActionButtonBgOpacity)
			: anim::with_alpha(
				Qt::black,
				st::infoProfileTopBarActionButtonBgOpacity);
	};
	auto buttons = std::vector<not_null<TopBarActionButton*>>();
	_actions = base::make_unique_q<Ui::HorizontalFitContainer>(
		this,
		st::infoProfileTopBarActionButtonsSpace);
	if (user) {
		const auto message = Ui::CreateChild<TopBarActionButton>(
			this,
			tr::lng_profile_action_short_message(tr::now),
			st::infoProfileTopBarActionMessage);
		message->setClickedCallback([=, window = controller] {
			window->showPeerHistory(
				peer->id,
				Window::SectionShow::Way::Forward);
		});
		buttons.push_back(message);
		_actions->add(message);
	}
	{
		const auto notifications = Ui::CreateChild<TopBarActionButton>(
			this,
			tr::lng_profile_action_short_mute(tr::now),
			st::infoProfileTopBarActionMessage);
		notifications->convertToToggle(
			st::infoProfileTopBarActionUnmute,
			st::infoProfileTopBarActionMute,
			u"profile_muting"_q,
			u"profile_unmuting"_q);

		const auto topicRootId = topic ? topic->rootId() : MsgId();
		const auto makeThread = [=] {
			return topicRootId
				? static_cast<Data::Thread*>(peer->forumTopicFor(topicRootId))
				: peer->owner().history(peer).get();
		};
		(topic
			? NotificationsEnabledValue(topic)
			: NotificationsEnabledValue(peer)
		) | rpl::start_with_next([=](bool enabled) {
			notifications->toggle(enabled);
			notifications->setText(enabled
				? tr::lng_profile_action_short_mute(tr::now)
				: tr::lng_profile_action_short_unmute(tr::now));
		}, notifications->lifetime());
		notifications->finishAnimating();

		notifications->setAcceptBoth();
		const auto notifySettings = &peer->owner().notifySettings();
			MuteMenu::SetupMuteMenu(
				notifications,
				notifications->clicks(
				) | rpl::filter([=](Qt::MouseButton button) {
					if (button == Qt::RightButton) {
						return true;
					}
					const auto topic = topicRootId
						? peer->forumTopicFor(topicRootId)
						: nullptr;
					Assert(!topicRootId || topic != nullptr);
					const auto is = topic
						? notifySettings->isMuted(topic)
						: notifySettings->isMuted(peer);
					if (is) {
						if (topic) {
							notifySettings->update(topic, { .unmute = true });
						} else {
							notifySettings->update(peer, { .unmute = true });
						}
						return false;
					} else {
						return true;
					}
				}) | rpl::to_empty,
				makeThread,
				controller->uiShow());
		buttons.push_back(notifications);
		_actions->add(notifications);
	}
	if (user && !user->sharedMediaInfo() && !user->isInaccessible()) {
		user->session().changes().peerFlagsValue(
			user,
			Data::PeerUpdate::Flag::HasCalls
		) | rpl::filter([=] {
			return user->hasCalls();
		}) | rpl::take(1) | rpl::start_with_next([=] {
			const auto call = Ui::CreateChild<TopBarActionButton>(
				this,
				tr::lng_profile_action_short_call(tr::now),
				st::infoProfileTopBarActionCall);
			call->setClickedCallback([=] {
				Core::App().calls().startOutgoingCall(user, false);
			});
			_edgeColor.value() | rpl::map(mapped) | rpl::start_with_next([=](
					QColor c) {
				call->setBgColor(c);
			}, call->lifetime());
			_actions->add(call);
		}, lifetime());
		if (user->callsStatus() == UserData::CallsStatus::Unknown) {
			user->updateFull();
		}
	}
	_edgeColor.value() | rpl::map(mapped) | rpl::start_with_next([=](
			QColor c) {
		for (const auto &button : buttons) {
			button->setBgColor(c);
		}
	}, _actions->lifetime());
	const auto padding = st::infoProfileTopBarActionButtonsPadding;
	sizeValue() | rpl::start_with_next([=](const QSize &size) {
		const auto ratio = float64(size.height())
			/ (st::infoProfileTopBarActionButtonsHeight
				+ st::infoLayerTopBarHeight);
		const auto h = st::infoProfileTopBarActionButtonSize;
		const auto resultHeight = (ratio >= 1.)
			? h
			: (ratio <= 0.5)
			? 0
			: int(h * (ratio - 0.5) / 0.5);
		_actions->setGeometry(
			padding.left(),
			size.height() - resultHeight - padding.bottom(),
			size.width() - rect::m::sum::h(padding),
			resultHeight);
	}, _actions->lifetime());
	_actions->show();
	_actions->raise();
}

void TopBar::setupUserpicButton(not_null<Controller*> controller) {
	_userpicButton = base::make_unique_q<Ui::AbstractButton>(this);
	rpl::single(
		rpl::empty_value()
	) | rpl::then(
		_peer->session().changes().peerFlagsValue(
			_peer,
			Data::PeerUpdate::Flag::Photo) | rpl::to_empty
	) | rpl::start_with_next([=] {
		_userpicButton->setAttribute(
			Qt::WA_TransparentForMouseEvents,
			!_peer->userpicPhotoId() && !_hasStories);
		updateVideoUserpic();
	}, lifetime());

	const auto openPhoto = [=, peer = _peer] {
		if (const auto id = peer->userpicPhotoId()) {
			if (const auto photo = peer->owner().photo(id); photo->date()) {
				controller->parentController()->openPhoto(photo, peer);
			}
		}
	};

	_userpicButton->setAcceptBoth(true);

	const auto menu = _userpicButton->lifetime().make_state<
		base::unique_qptr<Ui::PopupMenu>
	>();
	_userpicButton->clicks() | rpl::start_with_next([=](
			Qt::MouseButton button) {
		if (button == Qt::RightButton
			&& _hasStories
			&& _peer->userpicPhotoId()) {
			*menu = base::make_unique_q<Ui::PopupMenu>(
				this,
				st::popupMenuExpandedSeparator);

			(*menu)->addAction(
				tr::lng_profile_open_photo(tr::now),
				openPhoto,
				&st::menuIconPhoto);

			(*menu)->popup(QCursor::pos());
		} else if (button == Qt::LeftButton) {
			if (_hasStories) {
				controller->parentController()->openPeerStories(_peer->id);
			} else {
				openPhoto();
			}
		}
	}, _userpicButton->lifetime());
}

void TopBar::setupUniqueBadgeTooltip() {
	if (!_badge) {
		return;
	}
	base::timer_once(kWaitBeforeGiftBadge) | rpl::then(
		_badge->updated()
	) | rpl::start_with_next([=] {
		const auto widget = _badge->widget();
		const auto &content = _badgeContent.current();
		const auto &collectible = content.emojiStatusId.collectible;
		const auto premium = (content.badge == BadgeType::Premium);
		const auto id = (collectible && widget && premium)
			? collectible->id
			: uint64();
		if (_badgeCollectibleId == id) {
			return;
		}
		hideBadgeTooltip();
		if (!collectible) {
			return;
		}
		const auto parent = window();
		_badgeTooltip = std::make_unique<BadgeTooltip>(
			parent,
			collectible,
			widget);
		const auto raw = _badgeTooltip.get();
		raw->fade(true);
		_badgeTooltipHide->callOnce(kGiftBadgeGlares * raw->glarePeriod()
			- st::infoGiftTooltip.duration * 1.5);
		raw->setOpacity(_progress.current());
	}, lifetime());

	if (const auto raw = _badgeTooltip.get()) {
		raw->finishAnimating();
	}
}

void TopBar::hideBadgeTooltip() {
	_badgeTooltipHide->cancel();
	if (auto old = base::take(_badgeTooltip)) {
		const auto raw = old.get();
		_badgeOldTooltips.push_back(std::move(old));

		raw->fade(false);
		raw->shownValue(
		) | rpl::filter(
			!rpl::mappers::_1
		) | rpl::start_with_next([=] {
			const auto i = ranges::find(
				_badgeOldTooltips,
				raw,
				&std::unique_ptr<BadgeTooltip>::get);
			if (i != end(_badgeOldTooltips)) {
				_badgeOldTooltips.erase(i);
			}
		}, raw->lifetime());
	}
}

TopBar::~TopBar() {
	base::take(_badgeTooltip);
	base::take(_badgeOldTooltips);
}

void TopBar::setOnlineCount(rpl::producer<int> &&count) {
	std::move(count) | rpl::start_with_next([=](int v) {
		if (_statusLabel) {
			_statusLabel->setOnlineCount(v);
		}
	}, lifetime());
}

void TopBar::setEnableBackButtonValue(rpl::producer<bool> &&producer) {
	std::move(
		producer
	) | rpl::start_with_next([=](bool value) {
		updateLabelsPosition();
	}, lifetime());
}

void TopBar::setRoundEdges(bool value) {
	_roundEdges = value;
	update();
}

void TopBar::paintEdges(QPainter &p, const QBrush &brush) const {
	const auto r = rect();
	if (_roundEdges) {
		auto hq = PainterHighQualityEnabler(p);
		const auto radius = st::boxRadius;
		p.setPen(Qt::NoPen);
		p.setBrush(brush);
		p.drawRoundedRect(
			r + QMargins{ 0, 0, 0, radius + 1 },
			radius,
			radius);
	} else {
		p.fillRect(r, brush);
	}
}

void TopBar::paintEdges(QPainter &p) const {
	paintEdges(p, st::boxBg);
}

int TopBar::titleMostLeft() const {
	return (_back && _back->toggled())
		? _back->width()
		: _st.titleWithSubtitlePosition.x();
}

int TopBar::statusMostLeft() const {
	return (_back && _back->toggled())
		? _back->width()
		: _st.subtitlePosition.x();
}

void TopBar::updateLabelsPosition() {
	_progress = [&] {
		const auto max = QWidget::maximumHeight();
		const auto min = QWidget::minimumHeight()
			+ st::infoProfileTopBarActionButtonsHeight;
		const auto p = (max > min)
			? ((height() - min) / float64(max - min))
			: 1.;
		return std::clamp(p, 0., 1.);
	}();
	const auto progressCurrent = _progress.current();

	auto rightButtonsWidth = 0;
	if (_close) {
		rightButtonsWidth += _close->width();
	}
	if (_topBarMenuToggle) {
		rightButtonsWidth += _topBarMenuToggle->width();
	}

	const auto reservedRight = anim::interpolate(
		0,
		rightButtonsWidth,
		1. - progressCurrent);
	const auto interpolatedPadding = anim::interpolate(
		titleMostLeft(),
		rect::m::sum::h(st::boxRowPadding),
		progressCurrent);
	auto titleWidth = width() - interpolatedPadding - reservedRight;
	const auto verifiedWidget = _verified ? _verified->widget() : nullptr;
	const auto badgeWidget = _badge ? _badge->widget() : nullptr;
	if (verifiedWidget) {
		titleWidth -= verifiedWidget->width();
	}
	if (badgeWidget) {
		titleWidth -= badgeWidget->width();
	}
	if (verifiedWidget || badgeWidget) {
		titleWidth -= st::infoVerifiedCheckPosition.x();
	}

	if (titleWidth > 0 && _title->textMaxWidth() > titleWidth) {
		_title->resizeToWidth(titleWidth);
	}

	const auto titleTop = anim::interpolate(
		_st.titleWithSubtitlePosition.y(),
		st::infoProfileTopBarTitleTop,
		progressCurrent);
	const auto statusTop = anim::interpolate(
		_st.subtitlePosition.y(),
		st::infoProfileTopBarStatusTop,
		progressCurrent);

	auto titleLeft = anim::interpolate(
		titleMostLeft(),
		(width() - _title->width()) / 2,
		progressCurrent);
	const auto badgeTop = titleTop;
	const auto badgeBottom = titleTop + _title->height();
	const auto margins = LargeCustomEmojiMargins();

	if (_botVerify) {
		const auto widget = _botVerify->widget();
		const auto skip = widget
			? widget->width() + st::infoVerifiedCheckPosition.x()
			: 0;
		_botVerify->move(
			titleLeft - margins.left() - skip * progressCurrent,
			badgeTop,
			badgeBottom);
		titleLeft += skip * (1. - progressCurrent);
	}

	const auto statusLeft = anim::interpolate(
		statusMostLeft(),
		(width() - _status->width()) / 2,
		progressCurrent);

	if (const auto rating = _starsRating.get()) {
		rating->moveTo(statusLeft - _statusShift.current(), statusTop);
		rating->setOpacity(progressCurrent);
	}

	_title->moveToLeft(titleLeft, titleTop);
	const auto badgeLeft = titleLeft + _title->width();
	if (_badge) {
		_badge->move(badgeLeft, badgeTop, badgeBottom);
	}
	if (_verified) {
		_verified->move(
			badgeLeft + (badgeWidget ? badgeWidget->width() : 0),
			badgeTop,
			badgeBottom);
	}

	_status->moveToLeft(statusLeft, statusTop);

	if (!_showLastSeen->isHidden()) {
		_showLastSeen->moveToLeft(
			statusLeft
				+ _status->textMaxWidth()
				+ st::infoProfileTopBarLastSeenSkip,
			statusTop);
		if (_showLastSeenOpacity) {
			_showLastSeenOpacity->setOpacity(progressCurrent);
		}
		_showLastSeen->setAttribute(
			Qt::WA_TransparentForMouseEvents,
			!progressCurrent);
	}

	if (_badgeTooltip) {
		_badgeTooltip->setOpacity(progressCurrent);
	}

	{
		const auto userpicRect = userpicGeometry();
		if (_userpicButton) {
			_userpicButton->setGeometry(userpicGeometry());
		}

		updateGiftButtonsGeometry(progressCurrent, userpicRect);
	}
}

void TopBar::resizeEvent(QResizeEvent *e) {
	_cachedClipPath = QPainterPath();
	if (_hasBackground && !_animatedPoints.empty()) {
		setupAnimatedPattern();
	}
	if (_hasBackground && e->oldSize().width() != e->size().width()) {
		_cachedClipPath = QPainterPath();
		_cachedGradient = QImage();
	}
	updateLabelsPosition();
	RpWidget::resizeEvent(e);
}

QRect TopBar::userpicGeometry() const {
	constexpr auto kMinScale = 0.25;
	const auto progressCurrent = _progress.current();
	const auto fullSize = st::infoProfileTopBarPhotoSize;
	const auto minSize = fullSize * kMinScale;
	const auto size = anim::interpolate(minSize, fullSize, progressCurrent);
	const auto x = (width() - size) / 2;
	const auto minY = -minSize;
	const auto maxY = st::infoProfileTopBarPhotoTop;
	const auto y = anim::interpolate(minY, maxY, progressCurrent);
	return QRect(x, y, size, size);
}

void TopBar::updateGiftButtonsGeometry(
		float64 progressCurrent,
		const QRect &userpicRect) {
	const auto sz = st::infoProfileTopBarGiftSize;
	const auto halfSz = sz / 2.;
	for (const auto &gift : _pinnedToTopGifts) {
		if (gift.button) {
			const auto giftPos = calculateGiftPosition(
				gift.position,
				progressCurrent,
				userpicRect);
			const auto buttonRect = QRect(
				QPoint(giftPos.x() - halfSz, giftPos.y() - halfSz),
				Size(sz));
			gift.button->setGeometry(buttonRect);
		}
	}
}

void TopBar::paintUserpic(QPainter &p, const QRect &geometry) {
	if (_topicIconView) {
		_topicIconView->paintInRect(p, geometry);
		return;
	}
	if (_videoUserpicPlayer && _videoUserpicPlayer->ready()) {
		const auto size = st::infoProfileTopBarPhotoSize;
		const auto frame = _videoUserpicPlayer->frame(Size(size), _peer);
		if (!frame.isNull()) {
			p.drawImage(geometry, frame);
			update();
			return;
		}
	}
	const auto key = _peer->userpicUniqueKey(_userpicView);
	if (_userpicUniqueKey != key) {
		_userpicUniqueKey = key;
		const auto fullSize = st::infoProfileTopBarPhotoSize;
		const auto scaled = fullSize * style::DevicePixelRatio();
		auto image = QImage();
		if (const auto broadcast = _peer->monoforumBroadcast()) {
			image = PeerData::GenerateUserpicImage(
				broadcast,
				_userpicView,
				scaled,
				0);
			if (_monoforumMask.isNull()) {
				_monoforumMask = Ui::MonoforumShapeMask(Size(scaled));
			}
			constexpr auto kFormat = QImage::Format_ARGB32_Premultiplied;
			if (image.format() != kFormat) {
				image = std::move(image).convertToFormat(kFormat);
			}
			auto q = QPainter(&image);
			q.setCompositionMode(QPainter::CompositionMode_DestinationIn);
			q.drawImage(
				Rect(image.size() / image.devicePixelRatio()),
				_monoforumMask);
			q.end();
		} else {
			image = PeerData::GenerateUserpicImage(
				_peer,
				_userpicView,
				scaled,
				std::nullopt);
		}
		_cachedUserpic = std::move(image);
		_cachedUserpic.setDevicePixelRatio(style::DevicePixelRatio());
	}
	p.drawImage(geometry, _cachedUserpic);
}

void TopBar::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	const auto geometry = userpicGeometry();

	if (_hasBackground && _cachedGradient.isNull()) {
		_cachedGradient = Ui::CreateTopBgGradient(
			QSize(width(), maximumHeight()),
			_peer,
			QPoint(0, -st::infoProfileTopBarPhotoBgShift));
	}
	if (!_hasBackground) {
		paintEdges(p);
	} else {
		const auto x = (width()
			- _cachedGradient.width() / style::DevicePixelRatio())
				/ 2;
		const auto y = (height()
			- _cachedGradient.height() / style::DevicePixelRatio())
				/ 2;
		if (_roundEdges) {
			if (_cachedClipPath.isEmpty()) {
				const auto radius = st::boxRadius;
				_cachedClipPath.addRoundedRect(
					rect() + QMargins{ 0, 0, 0, radius + 1 },
					radius,
					radius);
			}
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setClipPath(_cachedClipPath);
			p.drawImage(x, y, _cachedGradient);
		} else {
			p.drawImage(x, y, _cachedGradient);
		}

		if (_patternEmoji && _patternEmoji->ready()) {
			paintAnimatedPattern(p, rect(), geometry);
		}

		paintPinnedToTopGifts(p, rect(), geometry);
	}
	paintUserpic(p, geometry);
	paintStoryOutline(p, geometry);
}

void TopBar::setupButtons(
		not_null<Controller*> controller,
		rpl::producer<bool> backToggles,
		Source source) {
	rpl::combine(
		controller->wrapValue(),
		_edgeColor.value()
	) | rpl::start_with_next([=, backToggles = std::move(backToggles)](
			Wrap wrap,
			std::optional<QColor> edgeColor) mutable {
		const auto isLayer = (wrap == Wrap::Layer);
		setRoundEdges(isLayer);

		const auto shouldUseColored = edgeColor
			&& (kMinContrast > Ui::CountContrast(
				st::boxTitleCloseFg->c,
				*edgeColor));
		_back = base::make_unique_q<Ui::FadeWrap<Ui::IconButton>>(
			this,
			object_ptr<Ui::IconButton>(
				this,
				(isLayer
					? (shouldUseColored
						? st::infoTopBarColoredBack
						: st::infoTopBarBack)
					: (shouldUseColored
						? st::infoLayerTopBarColoredBack
						: st::infoLayerTopBarBack))),
			st::infoTopBarScale);
		_back->QWidget::show();
		_back->setDuration(0);
		_back->toggleOn(isLayer
			? rpl::duplicate(backToggles)
			: rpl::single(wrap == Wrap::Narrow));
		setEnableBackButtonValue(_back->toggledValue());
		_back->entity()->addClickHandler([=] {
			controller->showBackFromStack();
		});

		if (!isLayer) {
			_close = nullptr;
		} else {
			_close = base::make_unique_q<Ui::IconButton>(
				this,
				shouldUseColored
					? st::infoTopBarColoredClose
					: st::infoTopBarClose);
			_close->show();
			_close->addClickHandler([=] {
				controller->parentController()->hideLayer();
				controller->parentController()->hideSpecialLayer();
			});
			widthValue() | rpl::start_with_next([=] {
				_close->moveToRight(0, 0);
			}, _close->lifetime());
		}

		if (wrap != Wrap::Side) {
			if (source == Source::Profile) {
				addTopBarMenuButton(controller, wrap, shouldUseColored);
			} else if (source == Source::Stories) {
				addTopBarEditButton(controller, wrap, shouldUseColored);
			}
		}
	}, lifetime());
}

void TopBar::addTopBarMenuButton(
		not_null<Controller*> controller,
		Wrap wrap,
		bool shouldUseColored) {
	{
		const auto guard = gsl::finally([&] { _topBarMenu = nullptr; });
		showTopBarMenu(controller, true);
		if (!_topBarMenu) {
			return;
		}
	}
	_topBarMenuToggle = base::make_unique_q<Ui::IconButton>(
		this,
		((wrap == Wrap::Layer)
			? (shouldUseColored
				? st::infoLayerTopBarColoredMenu
				: st::infoLayerTopBarMenu)
			: (shouldUseColored
				? st::infoTopBarColoredMenu
				: st::infoTopBarMenu)));
	_topBarMenuToggle->show();
	_topBarMenuToggle->addClickHandler([=] {
		showTopBarMenu(controller, false);
	});

	widthValue() | rpl::start_with_next([=] {
		if (_close) {
			_topBarMenuToggle->moveToRight(_close->width(), 0);
		} else {
			_topBarMenuToggle->moveToRight(0, 0);
		}
	}, _topBarMenuToggle->lifetime());

	Shortcuts::Requests(
	) | rpl::filter([=] {
		return (controller->section().type() == Section::Type::Profile);
	}) | rpl::start_with_next([=](not_null<Shortcuts::Request*> request) {
		using Command = Shortcuts::Command;

		request->check(Command::ShowChatMenu, 1) && request->handle([=] {
			Window::ActivateWindow(controller->parentController());
			showTopBarMenu(controller, false);
			return true;
		});
	}, _topBarMenuToggle->lifetime());
}

void TopBar::addTopBarEditButton(
		not_null<Controller*> controller,
		Wrap wrap,
		bool shouldUseColored) {
	_topBarMenuToggle = base::make_unique_q<Ui::IconButton>(
		this,
		((wrap == Wrap::Layer)
			? (shouldUseColored
				? st::infoLayerTopBarColoredEdit
				: st::infoLayerTopBarEdit)
			: (shouldUseColored
				? st::infoTopBarColoredEdit
				: st::infoTopBarEdit)));
	_topBarMenuToggle->show();
	_topBarMenuToggle->addClickHandler([=] {
		controller->showSettings(::Settings::Information::Id());
	});

	widthValue() | rpl::start_with_next([=] {
		if (_close) {
			_topBarMenuToggle->moveToRight(_close->width(), 0);
		} else {
			_topBarMenuToggle->moveToRight(0, 0);
		}
	}, _topBarMenuToggle->lifetime());
}

void TopBar::showTopBarMenu(
		not_null<Controller*> controller,
		bool check) {
	if (_topBarMenu) {
		_topBarMenu->hideMenu(true);
		return;
	}
	_topBarMenu = base::make_unique_q<Ui::PopupMenu>(
		QWidget::window(),
		st::popupMenuExpandedSeparator);

	_topBarMenu->setDestroyedCallback([this] {
		InvokeQueued(this, [this] { _topBarMenu = nullptr; });
		if (auto toggle = _topBarMenuToggle.get()) {
			toggle->setForceRippled(false);
		}
	});

	fillTopBarMenu(
		controller,
		Ui::Menu::CreateAddActionCallback(_topBarMenu));
	if (_topBarMenu->empty()) {
		_topBarMenu = nullptr;
		return;
	} else if (check) {
		return;
	}
	_topBarMenu->setForcedOrigin(Ui::PanelAnimation::Origin::TopRight);
	_topBarMenuToggle->setForceRippled(true);
	_topBarMenu->popup(_topBarMenuToggle->mapToGlobal(
		st::infoLayerTopBarMenuPosition));
}

void TopBar::fillTopBarMenu(
		not_null<Controller*> controller,
		const Ui::Menu::MenuCallback &addAction) {
	const auto peer = _peer;
	const auto topic = controller->key().topic();
	const auto sublist = controller->key().sublist();
	if (!peer && !topic) {
		return;
	}

	Window::FillDialogsEntryMenu(
		controller->parentController(),
		Dialogs::EntryState{
			.key = (topic
				? Dialogs::Key{ topic }
				: sublist
				? Dialogs::Key{ sublist }
				: Dialogs::Key{ peer->owner().history(peer) }),
			.section = Dialogs::EntryState::Section::Profile,
		},
		addAction);
}

void TopBar::updateVideoUserpic() {
	const auto id = _peer->userpicPhotoId();
	if (!id) {
		_videoUserpicPlayer = nullptr;
		return;
	}
	const auto photo = _peer->owner().photo(id);
	if (!photo->date() || !photo->videoCanBePlayed()) {
		_videoUserpicPlayer = nullptr;
		return;
	}
	if (!_videoUserpicPlayer) {
		_videoUserpicPlayer = std::make_unique<Ui::VideoUserpicPlayer>();
	}
	_videoUserpicPlayer->setup(_peer, photo);
}

void TopBar::setupShowLastSeen(not_null<Controller*> controller) {
	const auto user = _peer->asUser();
	if (!user
		|| user->isSelf()
		|| user->isBot()
		|| user->isServiceUser()
		|| !user->session().premiumPossible()) {
		_showLastSeen->hide();
		return;
	}

	if (user->session().premium()) {
		if (user->lastseen().isHiddenByMe()) {
			user->updateFullForced();
		}
		_showLastSeen->hide();
		return;
	}

	rpl::combine(
		user->session().changes().peerFlagsValue(
			user,
			Data::PeerUpdate::Flag::OnlineStatus),
		Data::AmPremiumValue(&user->session())
	) | rpl::start_with_next([=](auto, bool premium) {
		const auto wasShown = !_showLastSeen->isHidden();
		const auto hiddenByMe = user->lastseen().isHiddenByMe();
		const auto shown = hiddenByMe
			&& !user->lastseen().isOnline(base::unixtime::now())
			&& !premium
			&& user->session().premiumPossible();
		_showLastSeen->setVisible(shown);
		if (wasShown && premium && hiddenByMe) {
			user->updateFullForced();
		}
	}, _showLastSeen->lifetime());

	controller->session().api().userPrivacy().value(
		Api::UserPrivacy::Key::LastSeen
	) | rpl::filter([=](Api::UserPrivacy::Rule rule) {
		return (rule.option == Api::UserPrivacy::Option::Everyone);
	}) | rpl::start_with_next([=] {
		if (user->lastseen().isHiddenByMe()) {
			user->updateFullForced();
		}
	}, _showLastSeen->lifetime());

	_showLastSeenOpacity = Ui::CreateChild<QGraphicsOpacityEffect>(
		_showLastSeen.get());
	_showLastSeen->setGraphicsEffect(_showLastSeenOpacity);
	_showLastSeenOpacity->setOpacity(0.);

	using TextTransform = Ui::RoundButton::TextTransform;
	_showLastSeen->setTextTransform(TextTransform::NoTransform);
	_showLastSeen->setFullRadius(true);

	_showLastSeen->setClickedCallback([=] {
		const auto type = Ui::ShowOrPremium::LastSeen;
		controller->parentController()->show(Box(
			Ui::ShowOrPremiumBox,
			type,
			user->shortName(),
			[=] {
				controller->session().api().userPrivacy().save(
					::Api::UserPrivacy::Key::LastSeen,
					{});
			},
			[=] {
				::Settings::ShowPremium(
					controller->parentController(),
					u"lastseen_hidden"_q);
			}));
	});
}

void TopBar::setupAnimatedPattern(const QRect &userpicGeometry) {
	_animatedPoints = GenerateAnimatedPattern(userpicGeometry.isNull()
		? TopBar::userpicGeometry()
		: userpicGeometry);
}

void TopBar::paintAnimatedPattern(
		QPainter &p,
		const QRect &rect,
		const QRect &userpicGeometry) {
	if (!_patternEmoji || !_patternEmoji->ready()) {
		return;
	}

	{
		// TODO make it better.
		if (_lastUserpicRect != userpicGeometry) {
			_lastUserpicRect = userpicGeometry;
			setupAnimatedPattern(userpicGeometry);
		}
	}

	if (_basePatternImage.isNull()) {
		const auto collectible = _peer->emojiStatusId().collectible;
		if (!collectible) {
			return;
		}
		const auto ratio = style::DevicePixelRatio();
		const auto scale = 0.75;
		const auto size = Ui::Emoji::GetSizeNormal() * scale;
		_basePatternImage = QImage(
			QSize(size, size) * ratio,
			QImage::Format_ARGB32_Premultiplied);
		_basePatternImage.setDevicePixelRatio(ratio);
		_basePatternImage.fill(Qt::transparent);
		auto painter = QPainter(&_basePatternImage);
		auto hq = PainterHighQualityEnabler(painter);
		painter.scale(scale, scale);
		_patternEmoji->paint(painter, {
			.textColor = collectible->patternColor,
		});
	}

	const auto progress = _progress.current();
	// const auto collapseDiff = progress >= 0.85 ? 1. : (progress / 0.85);
	// const auto collapse = std::clamp((collapseDiff - 0.2) / 0.8, 0., 1.);
	const auto collapse = progress;

	const auto userpicCenter = rect::center(userpicGeometry);
	const auto yOffset = 12 * (1. - progress);
	const auto imageSize = _basePatternImage.size()
		/ style::DevicePixelRatio();
	const auto halfImageWidth = imageSize.width() * 0.5;
	const auto halfImageHeight = imageSize.height() * 0.5;

	for (const auto &point : _animatedPoints) {
		const auto timeRange = point.endTime - point.startTime;
		const auto collapseProgress = (1. - collapse <= point.startTime)
			? 1.
			: 1.
				- std::clamp(
					(1. - collapse - point.startTime) / timeRange,
					0.,
					1.);

		if (collapseProgress <= 0.) {
			continue;
		}

		auto x = point.basePosition.x();
		auto y = point.basePosition.y() - yOffset;
		auto r = point.size * 0.5;

		if (collapseProgress < 1.) {
			const auto dx = x - userpicCenter.x();
			const auto dy = y - userpicCenter.y();
			x = userpicCenter.x() + dx * collapseProgress;
			y = userpicCenter.y() + dy * collapseProgress;
			r = kMinPatternRadius
				+ (r - kMinPatternRadius) * collapseProgress;
		}

		const auto scale = r / (point.size * 0.5);
		const auto scaledHalfWidth = halfImageWidth * scale;
		const auto scaledHalfHeight = halfImageHeight * scale;

		// Distance-based alpha calculation.
		const auto userpicRect = _lastUserpicRect;
		const auto acx = userpicRect.x() + userpicRect.width() / 2.;
		const auto acy = userpicRect.y() + userpicRect.height() / 2.;
		const auto aw = userpicRect.width();
		const auto ah = userpicRect.height();
		const auto distance = std::sqrt((x - acx) * (x - acx)
			+ (y - acy) * (y - acy));
		const auto normalizedDistance = std::clamp(
			distance / (aw * 2.),
			0.,
			1.);
		const auto distanceAlpha = 1. - normalizedDistance;

		// Bottom alpha calculation.
		const auto bottomThreshold = userpicRect.y() + ah + kMinPatternRadius;
		const auto bottomAlpha = (y > bottomThreshold)
			? 1. - std::clamp((y - bottomThreshold) / 56., 0., 1.)
			: 1.;

		auto alpha = progress * distanceAlpha * 0.5 * bottomAlpha;
		if (collapseProgress < 1.) {
			alpha = alpha * collapseProgress;
		}

		p.setOpacity(alpha);
		p.drawImage(
			QRectF(
				x - scaledHalfWidth,
				y - scaledHalfHeight,
				scaledHalfWidth * 2,
				scaledHalfHeight * 2),
			_basePatternImage);
	}
	p.setOpacity(1.);
}

void TopBar::setupPinnedToTopGifts(not_null<Controller*> controller) {
	const auto requestDone = crl::guard(this, [=](
			std::vector<Data::SavedStarGift> gifts) {
		const auto shouldHideFirst = _pinnedToTopGiftsFirstTimeShowed
			&& !_pinnedToTopGifts.empty();

		if (shouldHideFirst) {
			_giftsHiding = std::make_unique<Ui::Animations::Simple>();
			_giftsHiding->start([=](float64 value) {
				update();
				if (value <= 0.) {
					_giftsHiding = nullptr;
					_pinnedToTopGifts.clear();
					_giftsLoadingLifetime.destroy();
					updateCollectibleStatus();
					setupNewGifts(controller, gifts);
				}
			}, 1., 0., 300, anim::linear);
			return;
		}

		_pinnedToTopGifts.clear();
		_giftsLoadingLifetime.destroy();

		updateCollectibleStatus();
		setupNewGifts(controller, gifts);
	});
	_peer->session().recentSharedGifts().request(_peer, requestDone, true);
}

void TopBar::setupNewGifts(
		not_null<Controller*> controller,
		const std::vector<Data::SavedStarGift> &gifts) {
	const auto emojiStatusId = _peer->emojiStatusId().collectible
		? _peer->emojiStatusId().collectible->id
		: CollectibleId(0);
	auto filteredGifts = std::vector<Data::SavedStarGift>();
	const auto subtract = emojiStatusId ? 1 : 0;
	filteredGifts.reserve((gifts.size() > subtract)
		? (gifts.size() - subtract)
		: 0);
	for (const auto &gift : gifts) {
		if (const auto &unique = gift.info.unique) {
			if (unique->id != emojiStatusId) {
				filteredGifts.push_back(gift);
			}
		}
	}

	_pinnedToTopGifts.reserve(filteredGifts.size());
	if (filteredGifts.empty()) {
		_giftsAppearing = nullptr;
		_lottiePlayer = nullptr;
		_pinnedToTopGiftsFirstTimeShowed = true;
	} else if (!_lottiePlayer) {
		_lottiePlayer = std::make_unique<Lottie::MultiPlayer>(
			Lottie::Quality::Default);
		_lottiePlayer->updates() | rpl::start_with_next([=] {
			update();
		}, lifetime());
	}

	_giftsAppearing = std::make_unique<Ui::Animations::Simple>();

	constexpr auto kMaxPinnedToTopGifts = 6;

	auto positions = ranges::views::iota(
		0,
		kMaxPinnedToTopGifts) | ranges::to_vector;
	ranges::shuffle(positions);

	for (auto i = 0;
		i < filteredGifts.size() && i < kMaxPinnedToTopGifts;
		++i) {
		const auto &gift = filteredGifts[i];
		const auto document = _peer->owner().document(
			gift.info.document->id);
		auto entry = PinnedToTopGiftEntry();
		entry.media = document->createMediaView();
		entry.media->checkStickerSmall();
		if (const auto &unique = gift.info.unique) {
			if (unique->backdrop.centerColor.isValid()
				&& unique->backdrop.edgeColor.isValid()) {
				entry.bg = Ui::CreateTopBgGradient(
					Size(st::infoProfileTopBarGiftSize * 2),
					unique->backdrop.centerColor,
					anim::with_alpha(unique->backdrop.edgeColor, 0.0),
					false);
			}
		}
		entry.position = positions[i];
		entry.button = base::make_unique_q<Ui::AbstractButton>(this);
		entry.button->show();

		entry.button->setClickedCallback([=, giftData = gift, peer = _peer] {
			::Settings::ShowSavedStarGiftBox(
				controller->parentController(),
				peer,
				giftData);
		});

		_pinnedToTopGifts.push_back(std::move(entry));
	}
	updateGiftButtonsGeometry(_progress.current(), userpicGeometry());

	using namespace ChatHelpers;

	rpl::single(
		rpl::empty_value()
	) | rpl::then(
		_peer->session().downloaderTaskFinished()
	) | rpl::start_with_next([=] {
		auto allLoaded = true;
		for (auto &entry : _pinnedToTopGifts) {
			if (!entry.animation && entry.media->loaded()) {
				entry.animation = LottieAnimationFromDocument(
					_lottiePlayer.get(),
					entry.media.get(),
					StickerLottieSize::StickerSet,
					Size(st::infoProfileTopBarGiftSize)
						* style::DevicePixelRatio());
			} else if (!entry.media->loaded()) {
				allLoaded = false;
			}
		}
		if (allLoaded) {
			_giftsLoadingLifetime.destroy();
			_giftsAppearing->stop();
			_giftsAppearing->start([=](float64 value) {
				update();
				if (value >= 1.) {
					_giftsAppearing = nullptr;
					_pinnedToTopGiftsFirstTimeShowed = true;
				}
			}, 0., 1., 400, anim::easeOutQuint);
		}
	}, _giftsLoadingLifetime);
}

QPointF TopBar::calculateGiftPosition(
		int position,
		float64 progress,
		const QRect &userpicRect) const {
	const auto acx = userpicRect.x() + userpicRect.width() / 2.;
	const auto acy = userpicRect.y() + userpicRect.height() / 2.;
	const auto aw = userpicRect.width();
	const auto ah = userpicRect.height();

	auto giftPos = QPointF();
	auto delayValue = 0.;
	switch (position) {
	case 0: // Left.
		giftPos = QPointF(
			acx / 2. - st::infoProfileTopBarGiftLeft.x(),
			acy - st::infoProfileTopBarGiftLeft.y());
		delayValue = 1.6;
		break;
	case 1: // Top left.
		giftPos = QPointF(
			acx * 2. / 3. - st::infoProfileTopBarGiftTopLeft.x(),
			userpicRect.y() - st::infoProfileTopBarGiftTopLeft.y());
		delayValue = 0.;
		break;
	case 2: // Bottom left.
		giftPos = QPointF(
			acx * 2. / 3. - st::infoProfileTopBarGiftBottomLeft.x(),
			userpicRect.y() + ah - st::infoProfileTopBarGiftBottomLeft.y());
		delayValue = 0.9;
		break;
	case 3: // Right.
		giftPos = QPointF(
			acx + aw / 2. + st::infoProfileTopBarGiftRight.x(),
			acy - st::infoProfileTopBarGiftRight.y());
		delayValue = 1.6;
		break;
	case 4: // Top right.
		giftPos = QPointF(
			acx + aw / 3. + st::infoProfileTopBarGiftTopRight.x(),
			userpicRect.y() - st::infoProfileTopBarGiftTopRight.y());
		delayValue = 0.9;
		break;
	default: // Bottom right.
		giftPos = QPointF(
			acx + aw / 3. + st::infoProfileTopBarGiftBottomRight.x(),
			userpicRect.y() + ah - st::infoProfileTopBarGiftBottomRight.y());
		delayValue = 0.;
		break;
	}

	const auto delayFraction = 0.2;
	const auto maxDelayFraction = 1.6 * delayFraction;
	const auto intervalFraction = 1. - maxDelayFraction;
	const auto delay = delayValue * delayFraction;
	const auto collapse = (progress >= 1. - delay)
		? 1.
		: std::clamp((progress - maxDelayFraction + delay)
			/ intervalFraction, 0., 1.);

	if (collapse < 1.) {
		const auto collapseX = 1. - std::pow(1. - collapse, 2.);
		giftPos = QPointF(
			acx + (giftPos.x() - acx) * collapseX,
			acy + (giftPos.y() - acy) * collapse);
	}

	return giftPos;
}

void TopBar::paintPinnedToTopGifts(
		QPainter &p,
		const QRect &rect,
		const QRect &userpicRect) {
	if (_pinnedToTopGifts.empty()) {
		return;
	}

	const auto progress = _giftsHiding
		? _progress.current() * _giftsHiding->value(1.)
		: (_giftsAppearing
			? _progress.current() * _giftsAppearing->value(0.)
			: _progress.current());

	const auto sz = st::infoProfileTopBarGiftSize;
	const auto halfSz = sz / 2.;

	for (auto &gift : _pinnedToTopGifts) {
		if (!gift.animation) {
			continue;
		}

		const auto giftPos = calculateGiftPosition(
			gift.position,
			progress,
			userpicRect);

		const auto alpha = progress;
		if (alpha <= 0.) {
			continue;
		}

		p.setOpacity(alpha);
		if (gift.animation && gift.animation->ready()) {
			const auto frame = gift.animation->frame();
			if (!frame.isNull()) {
				const auto resultRect = QRect(
					QPoint(giftPos.x() - halfSz, giftPos.y() - halfSz),
					QSize(sz, sz));
				if (!gift.bg.isNull()) {
					const auto bgSize = gift.bg.size()
						/ gift.bg.devicePixelRatio();
					const auto bgRect = QRect(
						resultRect.x()
							+ (resultRect.width() - bgSize.width()) / 2,
						resultRect.y()
							+ (resultRect.height() - bgSize.height()) / 2,
						bgSize.width(),
						bgSize.height());
					p.drawImage(bgRect, gift.bg);
				}
				p.drawImage(resultRect, frame);
				if (_lottiePlayer) {
					_lottiePlayer->markFrameShown();
				}
			}
		}
	}
	p.setOpacity(1.);
}

void TopBar::setupStoryOutline(const QRect &geometry) {
	const auto user = _peer->asUser();
	if (!user) {
		return;
	}

	rpl::combine(
		_edgeColor.value(),
		rpl::merge(
			rpl::single(rpl::empty_value()),
			style::PaletteChanged(),
			user->session().changes().peerUpdates(
				Data::PeerUpdate::Flag::StoriesState
			) | rpl::filter([=](const Data::PeerUpdate &update) {
				return update.peer == user;
			}) | rpl::to_empty)
	) | rpl::start_with_next([=](
			std::optional<QColor> edgeColor,
			rpl::empty_value) {
		const auto geometry = userpicGeometry();
		_storyOutlineBrush = Ui::UnreadStoryOutlineGradient(
			QRectF(
				geometry.x(),
				geometry.y(),
				geometry.width(),
				geometry.height()));
		updateStoryOutline(edgeColor);
	}, lifetime());
}

void TopBar::updateStoryOutline(std::optional<QColor> edgeColor) {
	const auto user = _peer->asUser();
	if (!user) {
		return;
	}

	const auto hasActiveStories = user->hasActiveStories();

	if (_hasStories != hasActiveStories) {
		_hasStories = hasActiveStories;
		update();
	}

	if (!hasActiveStories) {
		_storySegments.clear();
		return;
	}

	_storySegments.clear();
	const auto &stories = user->owner().stories();
	const auto source = stories.source(user->id);
	if (!source) {
		return;
	}

	const auto baseColor = edgeColor
		? Ui::BlendColors(*edgeColor, Qt::white, .5)
		: _storyOutlineBrush.color();
	const auto unreadBrush = edgeColor
		? QBrush(baseColor)
		: _storyOutlineBrush;
	const auto readBrush = edgeColor
		? QBrush(anim::with_alpha(baseColor, 0.5))
		: QBrush(st::dialogsUnreadBgMuted->b);

	const auto readTill = source->readTill;
	const auto widthBig = style::ConvertFloatScale(3.0);
	const auto widthSmall = widthBig / 2.;
	for (const auto &storyIdDates : source->ids) {
		const auto isUnread = (storyIdDates.id > readTill);
		_storySegments.push_back({
			.brush = isUnread ? unreadBrush : readBrush,
			.width = !isUnread ? widthSmall : widthBig,
		});
	}
}

void TopBar::paintStoryOutline(QPainter &p, const QRect &geometry) {
	if (!_hasStories || _storySegments.empty()) {
		return;
	}
	auto hq = PainterHighQualityEnabler(p);

	const auto progress = _progress.current();
	const auto alpha = std::clamp(
		(progress - kStoryOutlineFadeEnd) / kStoryOutlineFadeRange,
		0.,
		1.);
	if (alpha <= 0.) {
		return;
	}

	p.setOpacity(alpha);
	const auto outlineWidth = style::ConvertFloatScale(4.0);
	const auto padding = style::ConvertFloatScale(3.0);
	const auto outlineRect = QRectF(geometry).adjusted(
		-padding - outlineWidth / 2,
		-padding - outlineWidth / 2,
		padding + outlineWidth / 2,
		padding + outlineWidth / 2);

	Ui::PaintOutlineSegments(p, outlineRect, _storySegments);
}

rpl::producer<std::optional<QColor>> TopBar::edgeColor() const {
	return _edgeColor.value();
}

} // namespace Info::Profile
