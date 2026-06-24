/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "dialogs/ui/dialogs_pill.h"

#include "ui/painter.h"

#include "styles/style_dialogs.h"

namespace Dialogs {

void PaintPillTopSheen(QPainter &p, const QRect &pill, int radius) {
	if (pill.isEmpty() || st::dialogsBg->c.lightness() >= 128) {
		return;
	}
	auto top = QColor(255, 255, 255, 40);
	auto mid = QColor(255, 255, 255, 0);
	auto bottom = QColor(255, 255, 255, 20);
	auto grad = QLinearGradient(0, pill.top(), 0, pill.bottom());
	grad.setColorAt(0., top);
	grad.setColorAt(0.5, mid);
	grad.setColorAt(1., bottom);
	p.setPen(QPen(QBrush(grad), st::lineWidth));
	p.setBrush(Qt::NoBrush);
	const auto half = 0.5 * st::lineWidth;
	const auto stroke = QRectF(pill).adjusted(half, half, -half, -half);
	p.drawRoundedRect(stroke, radius - half, radius - half);
}

void PaintTopFade(QPainter &p, int outerWidth, int fadeHeight, QColor bg) {
	if (fadeHeight <= 0) {
		return;
	}
	auto transparent = bg;
	transparent.setAlpha(0);
	auto grad = QLinearGradient(0, 0, 0, fadeHeight);
	grad.setColorAt(0, bg);
	grad.setColorAt(1, transparent);
	p.fillRect(QRect(0, 0, outerWidth, fadeHeight), grad);
}

} // namespace Dialogs
