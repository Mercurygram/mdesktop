/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "ui/top_background_gradient.h"

#include "data/data_emoji_statuses.h"
#include "data/data_credits.h"
#include "data/data_peer.h"
#include "data/data_star_gift.h"
#include "ui/image/image_prepare.h"
#include "ui/painter.h"
#include "styles/style_layers.h"

namespace Ui {

QImage CreateTopBgGradient(
		QSize size,
		QColor centerColor,
		QColor edgeColor,
		bool rounded) {
	const auto ratio = style::DevicePixelRatio();
	auto result = QImage(size * ratio, QImage::Format_ARGB32_Premultiplied);
	result.setDevicePixelRatio(ratio);

	auto p = QPainter(&result);
	auto hq = PainterHighQualityEnabler(p);
	auto gradient = QRadialGradient(
		QRect(QPoint(), size).center(),
		size.height() / 2);
	gradient.setStops({
		{ 0., centerColor },
		{ 1., edgeColor },
	});
	p.setBrush(gradient);
	p.setPen(Qt::NoPen);
	p.drawRect(QRect(QPoint(), size));
	p.end();

	if (rounded) {
		const auto mask = Images::CornersMask(st::boxRadius);
		return Images::Round(std::move(result), mask, RectPart::FullTop);
	}
	return result;
}

QImage CreateTopBgGradient(QSize size, const Data::UniqueGift &gift) {
	return CreateTopBgGradient(
		size,
		gift.backdrop.centerColor,
		gift.backdrop.edgeColor);
}

QImage CreateTopBgGradient(QSize size, not_null<PeerData*> peer) {
	if (const auto collectible = peer->emojiStatusId().collectible) {
		return CreateTopBgGradient(
			size,
			collectible->centerColor,
			collectible->edgeColor,
			false);
	} else {
		return QImage();
	}
}

} // namespace Ui
