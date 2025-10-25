/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "ui/peer/color_sample.h"

#include "ui/chat/chat_style.h"
#include "ui/color_contrast.h"
#include "ui/painter.h"
#include "ui/text/text_utilities.h"
#include "styles/style_settings.h"

namespace Ui {
namespace {

constexpr auto kSelectAnimationDuration = crl::time(150);

} // namespace

ColorSample::ColorSample(
	not_null<QWidget*> parent,
	Fn<Ui::Text::MarkedContext()> contextProvider,
	Fn<TextWithEntities(uint64)> emojiProvider,
	std::shared_ptr<Ui::ChatStyle> style,
	rpl::producer<uint8> colorIndex,
	rpl::producer<std::shared_ptr<Ui::ColorCollectible>> collectible,
	const QString &name)
: AbstractButton(parent)
, _style(style) {
	rpl::combine(
		std::move(colorIndex),
		std::move(collectible)
	) | rpl::start_with_next([=](
			uint8 index,
			std::shared_ptr<Ui::ColorCollectible> collectible) {
		_index = index;
		_collectible = std::move(collectible);
		if (const auto raw = _collectible.get()) {
			auto context = contextProvider();
			context.repaint = [=] { update(); };
			_name.setMarkedText(
				st::semiboldTextStyle,
				emojiProvider(raw->giftEmojiId),
				kMarkupTextOptions,
				std::move(context));
		} else {
			_name.setText(st::semiboldTextStyle, name);
		}
		setNaturalWidth([&] {
			if (_name.isEmpty() || _style->colorPatternIndex(_index)) {
				return st::settingsColorSampleSize;
			}
			const auto padding = st::settingsColorSamplePadding;
			return std::max(
				padding.left() + _name.maxWidth() + padding.right(),
				padding.top() + st::semiboldFont->height + padding.bottom());
		}());
		update();
	}, lifetime());
}

ColorSample::ColorSample(
	not_null<QWidget*> parent,
	std::shared_ptr<Ui::ChatStyle> style,
	uint8 colorIndex,
	bool selected)
: AbstractButton(parent)
, _style(style)
, _index(colorIndex)
, _selected(selected)
, _simple(true) {
	setNaturalWidth(st::settingsColorSampleSize);
}

void ColorSample::setSelected(bool selected) {
	if (_selected == selected) {
		return;
	}
	_selected = selected;
	_selectAnimation.start(
		[=] { update(); },
		_selected ? 0. : 1.,
		_selected ? 1. : 0.,
		kSelectAnimationDuration);
}

void ColorSample::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);
	auto hq = PainterHighQualityEnabler(p);
	const auto colors = _style->coloredValues(false, _index);
	if (!_simple && !colors.outlines[1].alpha()) {
		const auto radius = height() / 2;
		p.setPen(Qt::NoPen);
		if (const auto raw = _collectible.get()) {
			const auto withBg = [&](const QColor &color) {
				return Ui::CountContrast(st::windowBg->c, color);
			};
			const auto dark = (withBg({ 0, 0, 0 })
				< withBg({ 255, 255, 255 }));
			const auto name = (dark && raw->darkAccentColor.alpha() > 0)
				? raw->darkAccentColor
				: raw->accentColor;
			auto bg = name;
			bg.setAlpha(0.12 * 255);
			p.setBrush(bg);
		} else {
			p.setBrush(colors.bg);
		}
		p.drawRoundedRect(rect(), radius, radius);

		const auto padding = st::settingsColorSamplePadding;
		p.setPen(colors.name);
		p.setBrush(Qt::NoBrush);
		p.setFont(st::semiboldFont);
		_name.drawLeftElided(
			p,
			padding.left(),
			padding.top(),
			width() - padding.left() - padding.right(),
			width(),
			1,
			style::al_top);
	} else {
		const auto size = float64(width());
		const auto half = size / 2.;
		const auto full = QRectF(-half, -half, size, size);
		p.translate(size / 2., size / 2.);
		p.setPen(Qt::NoPen);
		if (colors.outlines[1].alpha()) {
			p.rotate(-45.);
			p.setClipRect(-size, 0, 3 * size, size);
			p.setBrush(colors.outlines[1]);
			p.drawEllipse(full);
			p.setClipRect(-size, -size, 3 * size, size);
		}
		p.setBrush(colors.outlines[0]);
		p.drawEllipse(full);
		p.setClipping(false);
		if (colors.outlines[2].alpha()) {
			const auto multiplier = size / st::settingsColorSampleSize;
			const auto center = st::settingsColorSampleCenter * multiplier;
			const auto radius = st::settingsColorSampleCenterRadius
				* multiplier;
			p.setBrush(colors.outlines[2]);
			p.drawRoundedRect(
				QRectF(-center / 2., -center / 2., center, center),
				radius,
				radius);
		}
		const auto selected = _selectAnimation.value(_selected ? 1. : 0.);
		if (selected > 0) {
			const auto line = st::settingsColorRadioStroke * 1.;
			const auto thickness = selected * line;
			auto pen = st::boxBg->p;
			pen.setWidthF(thickness);
			p.setBrush(Qt::NoBrush);
			p.setPen(pen);
			const auto skip = 1.5 * line;
			p.drawEllipse(full.marginsRemoved({ skip, skip, skip, skip }));
		}
	}
}

uint8 ColorSample::index() const {
	return _index;
}

} // namespace Ui