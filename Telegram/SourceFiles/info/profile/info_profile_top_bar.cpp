/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "info/profile/info_profile_top_bar.h"

#include "data/data_peer.h"
#include "info_profile_actions.h"
#include "ui/painter.h"
#include "styles/style_boxes.h"
#include "styles/style_info.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"

namespace Info::Profile {

TopBar::TopBar(
	not_null<Ui::RpWidget*> parent,
	not_null<PeerData*> peer)
: RpWidget(parent)
, _peer(peer) {
	setMinimumHeight(st::infoLayerTopBarHeight);
	setMaximumHeight(st::settingsPremiumTopHeight);
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

void TopBar::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	paintEdges(p);
}

} // namespace Info::Profile
