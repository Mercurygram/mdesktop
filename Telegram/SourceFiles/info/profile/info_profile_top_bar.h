/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "ui/rp_widget.h"

namespace Info::Profile {

class TopBar final : public Ui::RpWidget {
public:
	TopBar(QWidget *parent);

protected:
	void paintEvent(QPaintEvent *e) override;

};

} // namespace Info::Profile
