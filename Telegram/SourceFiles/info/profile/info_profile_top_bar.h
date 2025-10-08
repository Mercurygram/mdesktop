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

	void setRoundEdges(bool value);

protected:
	void paintEvent(QPaintEvent *e) override;

private:
	void paintEdges(QPainter &p, const QBrush &brush) const;
	void paintEdges(QPainter &p) const;

	bool _roundEdges = true;

};

} // namespace Info::Profile
