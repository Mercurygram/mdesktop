/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Data {
struct UniqueGift;
} // namespace Data

class PeerData;

namespace Ui {

[[nodiscard]] QImage CreateTopBgGradient(
	QSize size,
	const Data::UniqueGift &gift);

[[nodiscard]] QImage CreateTopBgGradient(
	QSize size,
	QColor centerColor,
	QColor edgeColor,
	bool rounded = true);

[[nodiscard]] QImage CreateTopBgGradient(
	QSize size,
	not_null<PeerData*> peer);

} // namespace Ui
