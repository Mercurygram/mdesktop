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
#include "data/data_user.h"
#include "data/stickers/data_custom_emoji.h"
#include "info_profile_actions.h"
#include "info/info_controller.h"
#include "info/info_memento.h"
#include "info/profile/info_profile_badge_tooltip.h"
#include "info/profile/info_profile_badge.h"
#include "info/profile/info_profile_cover.h" // LargeCustomEmojiMargins
#include "info/profile/info_profile_status_label.h"
#include "info/profile/info_profile_values.h"
#include "lang/lang_keys.h"
#include "lottie/lottie_animation.h"
#include "lottie/lottie_multi_player.h"
#include "main/main_session.h"
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
#include "ui/widgets/buttons.h"
#include "ui/widgets/buttons.h"
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
#include "styles/style_settings.h"

#include <QGraphicsOpacityEffect>

namespace Info::Profile {
namespace {

constexpr auto kWaitBeforeGiftBadge = crl::time(1000);
constexpr auto kGiftBadgeGlares = 3;
constexpr auto kMinPatternRadius = 8;

using AnimatedPatternPoint = TopBar::AnimatedPatternPoint;

[[nodiscard]] int PinnedGiftSize() {
	static const auto Size = []() -> int {
		return Ui::Emoji::GetSizeNormal() * 0.5;
	}();
	return Size;
}

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
, _peer(descriptor.controller->key().peer())
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
, _badge(std::make_unique<Badge>(
	this,
	st::infoPeerBadge,
	&_peer->session(),
	_badgeContent.value(),
	nullptr,
	Fn<bool()>([=, controller = descriptor.controller->parentController()] {
		return controller->isGifPausedAtLeastFor(
			Window::GifPauseReason::Layer);
	})))
, _verified(std::make_unique<Badge>(
	this,
	st::infoPeerBadge,
	&_peer->session(),
	VerifiedContentForPeer(_peer),
	nullptr,
	Fn<bool()>([=, controller = descriptor.controller->parentController()] {
		return controller->isGifPausedAtLeastFor(
			Window::GifPauseReason::Layer);
	})))
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
	QWidget::setMaximumHeight(st::infoLayerProfileTopBarHeightMax);

	const auto controller = descriptor.controller;

	if (_peer->isMegagroup() || _peer->isChat()) {
		_statusLabel->setMembersLinkCallback([=] {
			const auto topic = controller->key().topic();
			const auto sublist = controller->key().sublist();
			const auto shown = sublist
				? sublist->sublistPeer().get()
				: controller->key().peer();
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
	setupButtons(controller, descriptor.backToggles.value());
	setupUserpicButton(controller);
	setupPinnedToTopGifts();
	if (_topic) {
		_topicIconView = std::make_unique<TopicIconView>(
			_topic,
			[controller = controller->parentController()] {
				return controller->isGifPausedAtLeastFor(
					Window::GifPauseReason::Layer);
			},
			[=] { update(); });
	} else {
		updateVideoUserpic();
	}

	_peer->session().changes().peerFlagsValue(
		_peer,
		Data::PeerUpdate::Flag::EmojiStatus
	) | rpl::start_with_next([=] {
		const auto collectible = _peer->emojiStatusId().collectible;
		_hasBackground = collectible != nullptr;
		_cachedClipPath = QPainterPath();
		_cachedGradient = QImage();
		_basePatternImage = QImage();
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
		if (collectible) {
			constexpr auto kMinContrast = 5.5;
			const auto contrastTitle = Ui::CountContrast(
				_title->st().textFg->c,
				collectible->edgeColor);
			const auto contrastStatus = Ui::CountContrast(
				_status->st().textFg->c,
				collectible->edgeColor);
			_title->setTextColorOverride((contrastTitle < kMinContrast)
				? std::optional<QColor>(st::groupCallMembersFg->c)
				: std::nullopt);
			_status->setTextColorOverride((contrastStatus < kMinContrast)
				? std::optional<QColor>(st::groupCallVideoSubTextFg->c)
				: std::nullopt);
		} else {
			_title->setTextColorOverride(std::nullopt);
			_status->setTextColorOverride(std::nullopt);
		}
	}, lifetime());
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
			!_peer->userpicPhotoId());
		updateVideoUserpic();
	}, lifetime());
	_userpicButton->setClickedCallback([=] {
		if (const auto id = _peer->userpicPhotoId()) {
			if (const auto photo = _peer->owner().photo(id); photo->date()) {
				controller->parentController()->openPhoto(photo, _peer);
			}
		}
	});
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

void TopBar::updateLabelsPosition() {
	const auto max = QWidget::maximumHeight();
	const auto min = QWidget::minimumHeight();
	_progress = (max > min)
		? ((height() - min) / float64(max - min))
		: 1.;
	const auto progressCurrent = _progress.current();

	auto rightButtonsWidth = 0;
	if (_close) {
		rightButtonsWidth += _close->width();
	}
	if (_topBarMenuToggle) {
		rightButtonsWidth += _topBarMenuToggle->width();
	}
	if (_callsButton) {
		rightButtonsWidth += _callsButton->width();
	}

	const auto reservedRight = anim::interpolate(
		0,
		rightButtonsWidth,
		1. - progressCurrent);
	const auto interpolatedPadding = anim::interpolate(
		_st.titleWithSubtitlePosition.x(),
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
		st::infoLayerProfileTopBarTitleTop,
		progressCurrent);
	const auto statusTop = anim::interpolate(
		_st.subtitlePosition.y(),
		st::infoLayerProfileTopBarStatusTop,
		progressCurrent);

	auto titleLeft = anim::interpolate(
		_st.titleWithSubtitlePosition.x(),
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
		_st.subtitlePosition.x(),
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
				+ st::infoLayerProfileTopBarLastSeenSkip,
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

	if (_userpicButton) {
		_userpicButton->setGeometry(userpicGeometry());
	}
}

void TopBar::resizeEvent(QResizeEvent *e) {
	_cachedClipPath = QPainterPath();
	if (_hasBackground && !_animatedPoints.empty()) {
		setupAnimatedPattern();
	}
	updateLabelsPosition();
	RpWidget::resizeEvent(e);
}

QRect TopBar::userpicGeometry() const {
	constexpr auto kMinScale = 0.25;
	const auto progressCurrent = _progress.current();
	const auto fullSize = st::infoLayerProfileTopBarPhotoSize;
	const auto minSize = fullSize * kMinScale;
	const auto size = anim::interpolate(minSize, fullSize, progressCurrent);
	const auto x = (width() - size) / 2;
	const auto minY = -minSize;
	const auto maxY = st::infoLayerProfileTopBarPhotoTop;
	const auto y = anim::interpolate(minY, maxY, progressCurrent);
	return QRect(x, y, size, size);
}

void TopBar::paintUserpic(QPainter &p) {
	const auto geometry = userpicGeometry();
	if (_topicIconView) {
		_topicIconView->paintInRect(p, geometry);
		return;
	}
	if (_videoUserpicPlayer && _videoUserpicPlayer->ready()) {
		const auto size = st::infoLayerProfileTopBarPhotoSize;
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
		const auto fullSize = st::infoLayerProfileTopBarPhotoSize;
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
	if (_hasBackground && _cachedGradient.isNull()) {
		_cachedGradient = Ui::CreateTopBgGradient(
			QSize(width(), maximumHeight()),
			_peer);
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
			paintAnimatedPattern(p, rect());
		}

		paintPinnedToTopGifts(p, rect());
	}
	paintUserpic(p);
}

void TopBar::setupButtons(
		not_null<Controller*> controller,
		rpl::producer<bool> backToggles) {
	controller->wrapValue() | rpl::start_with_next([=,
			backToggles = std::move(backToggles)](Wrap wrap) mutable {
		const auto isLayer = (wrap == Wrap::Layer);
		setRoundEdges(isLayer);

		_back = base::make_unique_q<Ui::FadeWrap<Ui::IconButton>>(
			this,
			object_ptr<Ui::IconButton>(
				this,
				(isLayer
					? st::infoTopBarBack
					: st::infoLayerTopBarBack)),
			st::infoTopBarScale);
		_back->setDuration(0);
		_back->toggleOn(isLayer
			? rpl::duplicate(backToggles) | rpl::type_erased()
			: rpl::single(true));
		setEnableBackButtonValue(_back->toggledValue());
		_back->entity()->addClickHandler([=] {
			controller->showBackFromStack();
		});

		if (!isLayer) {
			_close = nullptr;
		} else {
			_close = base::make_unique_q<Ui::IconButton>(
				this,
				st::infoTopBarClose);
			_close->addClickHandler([=] {
				controller->parentController()->hideLayer();
				controller->parentController()->hideSpecialLayer();
			});
			widthValue() | rpl::start_with_next([=] {
				_close->moveToRight(0, 0);
			}, _close->lifetime());
		}

		if (wrap != Wrap::Side) {
			addTopBarMenuButton(controller, wrap);
			addProfileCallsButton(controller, wrap);
		}
	}, lifetime());
}

void TopBar::addTopBarMenuButton(
		not_null<Controller*> controller,
		Wrap wrap) {
	{
		const auto guard = gsl::finally([&] { _topBarMenu = nullptr; });
		showTopBarMenu(controller, true);
		if (!_topBarMenu) {
			return;
		}
	}

	_topBarMenuToggle = base::make_unique_q<Ui::IconButton>(
		this,
		(wrap == Wrap::Layer ? st::infoLayerTopBarMenu : st::infoTopBarMenu));
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
	const auto peer = controller->key().peer();
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

void TopBar::addProfileCallsButton(
		not_null<Controller*> controller,
		Wrap wrap) {
	const auto peer = controller->key().peer();
	const auto user = peer ? peer->asUser() : nullptr;
	if (!user || user->sharedMediaInfo() || user->isInaccessible()) {
		return;
	}

	user->session().changes().peerFlagsValue(
		user,
		Data::PeerUpdate::Flag::HasCalls
	) | rpl::filter([=] {
		return user->hasCalls();
	}) | rpl::take(
		1
	) | rpl::start_with_next([=] {
		_callsButton = base::make_unique_q<Ui::IconButton>(
			this,
			(wrap == Wrap::Layer
				? st::infoLayerTopBarCall
				: st::infoTopBarCall));
		_callsButton->addClickHandler([=] {
			Core::App().calls().startOutgoingCall(user, false);
		});

		widthValue() | rpl::start_with_next([=] {
			auto right = 0;
			if (_close) {
				right += _close->width();
			}
			if (_topBarMenuToggle) {
				right += _topBarMenuToggle->width();
			}
			_callsButton->moveToRight(right, 0);
		}, _callsButton->lifetime());
	}, lifetime());

	if (user && user->callsStatus() == UserData::CallsStatus::Unknown) {
		user->updateFull();
	}
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

void TopBar::setupAnimatedPattern() {
	_animatedPoints = GenerateAnimatedPattern(userpicGeometry());
}

void TopBar::paintAnimatedPattern(QPainter &p, const QRect &rect) {
	const auto collectible = _peer->emojiStatusId().collectible;
	if (!collectible || !_patternEmoji->ready()) {
		return;
	}

	{
		// TODO make it better.
		const auto currentUserpicRect = userpicGeometry();
		if (_lastUserpicRect != currentUserpicRect) {
			_lastUserpicRect = currentUserpicRect;
			setupAnimatedPattern();
		}
	}

	if (_basePatternImage.isNull()) {
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

	const auto userpicCenter = userpicGeometry().center();
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

void TopBar::setupPinnedToTopGifts() {
	const auto requestDone = crl::guard(this, [=](
			std::vector<DocumentId> ids) {
		_pinnedToTopGifts.clear();
		_pinnedToTopGifts.reserve(ids.size());
		_giftsLoadingLifetime.destroy();
		if (ids.empty()) {
			_lottiePlayer = nullptr;
		} else if (!_lottiePlayer) {
			_lottiePlayer = std::make_unique<Lottie::MultiPlayer>(
				Lottie::Quality::Default);
			_lottiePlayer->updates() | rpl::start_with_next([=] {
				update();
			}, lifetime());
		}

		constexpr auto kMaxPinnedToTopGifts = 6;

		auto positions = ranges::views::iota(
			0,
			kMaxPinnedToTopGifts) | ranges::to_vector;
		ranges::shuffle(positions);

		for (auto i = 0; i < ids.size() && i < kMaxPinnedToTopGifts; ++i) {
			const auto document = _peer->owner().document(ids[i]);
			auto entry = PinnedToTopGiftEntry();
			entry.media = document->createMediaView();
			entry.media->checkStickerSmall();
			entry.position = positions[i];
			_pinnedToTopGifts.push_back(std::move(entry));
		}

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
						Size(PinnedGiftSize()) * style::DevicePixelRatio());
				} else if (!entry.media->loaded()) {
					allLoaded = false;
				}
			}
			if (allLoaded) {
				_giftsLoadingLifetime.destroy();
			}
		}, _giftsLoadingLifetime);
	});
	_peer->session().recentSharedGifts().request(_peer, requestDone, true);
}

void TopBar::paintPinnedToTopGifts(QPainter &p, const QRect &rect) {
	if (_pinnedToTopGifts.empty()) {
		return;
	}

	const auto progress = _progress.current();
	const auto userpicRect = _lastUserpicRect;
	const auto acx = userpicRect.x() + userpicRect.width() / 2.;
	const auto acy = userpicRect.y() + userpicRect.height() / 2.;
	const auto aw = userpicRect.width();
	const auto ah = userpicRect.height();

	const auto sz = PinnedGiftSize();
	const auto halfSz = sz / 2.;

	for (const auto &gift : _pinnedToTopGifts) {
		if (!gift.animation) {
			continue;
		}

		auto giftPos = QPointF();
		auto delayValue = 0.;
		switch (gift.position) {
		case 0: // Left.
			giftPos = QPointF(
				acx / 2. - st::infoLayerProfileTopBarGiftLeft.x(),
				acy - st::infoLayerProfileTopBarGiftLeft.y());
			delayValue = 1.6;
			break;
		case 1: // Top left.
			giftPos = QPointF(
				acx * 2. / 3. - st::infoLayerProfileTopBarGiftTopLeft.x(),
				userpicRect.y() - st::infoLayerProfileTopBarGiftTopLeft.y());
			delayValue = 0.;
			break;
		case 2: // Bottom left.
			giftPos = QPointF(
				acx * 2. / 3. - st::infoLayerProfileTopBarGiftBottomLeft.x(),
				userpicRect.y() + ah - st::infoLayerProfileTopBarGiftBottomLeft.y());
			delayValue = 0.9;
			break;
		case 3: // Right.
			giftPos = QPointF(
				acx + aw / 2. + st::infoLayerProfileTopBarGiftRight.x(),
				acy - st::infoLayerProfileTopBarGiftRight.y());
			delayValue = 1.6;
			break;
		case 4: // Top right.
			giftPos = QPointF(
				acx + aw / 3. + st::infoLayerProfileTopBarGiftTopRight.x(),
				userpicRect.y() - st::infoLayerProfileTopBarGiftTopRight.y());
			delayValue = 0.9;
			break;
		default: // Bottom right.
			giftPos = QPointF(
				acx + aw / 3. + st::infoLayerProfileTopBarGiftBottomRight.x(),
				userpicRect.y() + ah - st::infoLayerProfileTopBarGiftBottomRight.y());
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
				p.drawImage(
					resultRect,
					frame);
				if (_lottiePlayer) {
					_lottiePlayer->markFrameShown();
				}
			}
		}
	}
	p.setOpacity(1.);
}

} // namespace Info::Profile
