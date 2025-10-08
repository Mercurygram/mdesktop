/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "info/profile/info_profile_top_bar.h"

#include "calls/calls_instance.h"
#include "core/application.h"
#include "core/shortcuts.h"
#include "data/data_changes.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "info_profile_actions.h"
#include "info/info_controller.h"
#include "info/info_memento.h"
#include "info/profile/info_profile_status_label.h"
#include "info/profile/info_profile_values.h"
#include "main/main_session.h"
#include "ui/effects/animations.h"
#include "ui/painter.h"
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

namespace Info::Profile {

TopBar::TopBar(
	not_null<Ui::RpWidget*> parent,
	Descriptor descriptor)
: RpWidget(parent)
, _peer(descriptor.controller->key().peer())
, _st(st::infoTopBar)
, _title(this, Info::Profile::NameValue(_peer), _st.title)
, _status(this, QString(), _st.subtitle)
, _statusLabel(
	std::make_unique<StatusLabel>(_status.data(), _peer, rpl::single(0))) {
	QWidget::setMinimumHeight(st::infoLayerTopBarHeight);
	QWidget::setMaximumHeight(st::infoLayerProfileTopBarHeightMax);

	_peer->session().changes().peerFlagsValue(
		_peer,
		Data::PeerUpdate::Flag::OnlineStatus
			| Data::PeerUpdate::Flag::Members
	) | rpl::start_with_next([=] {
		_statusLabel->refresh();
	}, lifetime());

	setupButtons(descriptor.controller, descriptor.backToggles.value());
}

TopBar::~TopBar() = default;

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

	const auto titleTop = anim::interpolate(
		_st.titleWithSubtitlePosition.y(),
		st::infoLayerProfileTopBarTitleTop,
		progressCurrent);
	const auto statusTop = anim::interpolate(
		_st.subtitlePosition.y(),
		st::infoLayerProfileTopBarStatusTop,
		progressCurrent);

	const auto titleLeft = anim::interpolate(
		_st.titleWithSubtitlePosition.x(),
		(width() - _title->width()) / 2,
		progressCurrent);
	const auto statusLeft = anim::interpolate(
		_st.subtitlePosition.x(),
		(width() - _status->width()) / 2,
		progressCurrent);

	_title->moveToLeft(titleLeft, titleTop);
	_status->moveToLeft(statusLeft, statusTop);
}

void TopBar::resizeEvent(QResizeEvent *e) {
	updateLabelsPosition();
	RpWidget::resizeEvent(e);
}

void TopBar::paintUserpic(QPainter &p) {
	constexpr auto kMinScale = 0.25;
	const auto progressCurrent = _progress.current();
	const auto fullSize = st::infoLayerProfileTopBarPhotoSize;
	const auto minSize = fullSize * kMinScale;
	const auto size = anim::interpolate(minSize, fullSize, progressCurrent);
	const auto x = (width() - size) / 2;
	const auto minY = -minSize;
	const auto maxY = st::infoLayerProfileTopBarPhotoTop;
	const auto y = anim::interpolate(minY, maxY, progressCurrent);

	const auto key = _peer->userpicUniqueKey(_userpicView);
	if (_userpicUniqueKey != key) {
		_userpicUniqueKey = key;
		const auto scaled = fullSize * style::DevicePixelRatio();
		_cachedUserpic = PeerData::GenerateUserpicImage(
			_peer,
			_userpicView,
			scaled);
		_cachedUserpic.setDevicePixelRatio(style::DevicePixelRatio());
	}

	p.drawImage(QRect(x, y, size, size), _cachedUserpic);
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

} // namespace Info::Profile
