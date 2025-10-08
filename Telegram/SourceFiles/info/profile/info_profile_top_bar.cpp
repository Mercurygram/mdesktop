/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "info/profile/info_profile_top_bar.h"

#include "data/data_changes.h"
#include "data/data_peer.h"
#include "info_profile_actions.h"
#include "info/profile/info_profile_status_label.h"
#include "info/profile/info_profile_values.h"
#include "main/main_session.h"
#include "ui/effects/animations.h"
#include "ui/painter.h"
#include "ui/widgets/labels.h"
#include "styles/style_boxes.h"
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

} // namespace Info::Profile
