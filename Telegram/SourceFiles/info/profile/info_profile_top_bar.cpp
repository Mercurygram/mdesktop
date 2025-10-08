/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "info/profile/info_profile_top_bar.h"

#include "styles/style_info.h"
#include "styles/style_settings.h"

namespace Info::Profile {

TopBar::TopBar(QWidget *parent)
: RpWidget(parent) {
	setMinimumHeight(st::infoLayerTopBarHeight);
	setMaximumHeight(st::settingsPremiumTopHeight);
}

void TopBar::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	p.fillRect(rect(), st::boxBg);
}

} // namespace Info::Profile
