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
#include "core/application.h"
#include "core/shortcuts.h"
#include "data/data_changes.h"
#include "data/data_channel.h"
#include "data/data_emoji_statuses.h"
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
#include "main/main_session.h"
#include "settings/settings_premium.h"
#include "ui/boxes/show_or_premium_box.h"
#include "ui/controls/stars_rating.h"
#include "ui/effects/animations.h"
#include "ui/empty_userpic.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/rect.h"
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
#include "styles/style_info.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"

#include <QGraphicsOpacityEffect>

namespace Info::Profile {
namespace {

constexpr auto kWaitBeforeGiftBadge = crl::time(1000);
constexpr auto kGiftBadgeGlares = 3;

} // namespace

TopBar::TopBar(
	not_null<Ui::RpWidget*> parent,
	Descriptor descriptor)
: RpWidget(parent)
, _peer(descriptor.controller->key().peer())
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
	paintEdges(p);
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

} // namespace Info::Profile
