// This file is part of Telegram Desktop,
// the official desktop application for the Telegram messaging service.
//
// For license and copyright information please follow this link:
// https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
//
#include "ui/controls/dynamic_images_strip.h"

#include "ui/dynamic_image.h"
#include "ui/painter.h"
#include "styles/style_chat_helpers.h"
#include "styles/style_widgets.h"

namespace Ui {
namespace {

constexpr auto kDimmedAlpha = 0.6;
constexpr auto kHoverScaleIncrease = st::topPeersSelectorUserpicExpand;

} // namespace

DynamicImagesStrip::DynamicImagesStrip(
	QWidget *parent,
	std::vector<std::shared_ptr<DynamicImage>> thumbnails,
	int userpicSize,
	int gap)
: RpWidget(parent)
, _thumbnails(std::move(thumbnails))
, _userpicSize(userpicSize)
, _gap(gap)
, _scales(_thumbnails.size(), 0.)
, _alphas(_thumbnails.size(), 1.)
, _scaleTargets(_thumbnails.size(), 0.)
, _alphaTargets(_thumbnails.size(), 1.)
, _animation([=](crl::time now) {
	const auto dt = (now - _animation.started())
		/ float64(st::universalDuration);
	auto finished = true;
	for (auto i = 0; i < int(_thumbnails.size()); ++i) {
		const auto progress = std::clamp(dt, 0., 1.);
		_scales[i] += (_scaleTargets[i] - _scales[i]) * progress;
		_alphas[i] += (_alphaTargets[i] - _alphas[i]) * progress;
		if (std::abs(_scales[i] - _scaleTargets[i]) > 0.01
			|| std::abs(_alphas[i] - _alphaTargets[i]) > 0.01) {
			finished = false;
		}
	}
	update();
	return !finished;
}) {
	for (const auto &thumbnail : _thumbnails) {
		thumbnail->subscribeToUpdates([=] { update(); });
	}
	setMouseTracking(true);
}

void DynamicImagesStrip::setProgress(float64 progress) {
	_progress = progress;
	update();
}

void DynamicImagesStrip::setClickCallback(Fn<void(int)> callback) {
	_clickCallback = std::move(callback);
}

rpl::producer<HoveredItemInfo> DynamicImagesStrip::hoveredItemValue() const {
	return _hoveredItem.events();
}

void DynamicImagesStrip::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	{
		const auto shift = (height() - _userpicSize) / 2;
		p.translate(shift, shift);
	}
	auto x = 0;
	const auto count = int(_thumbnails.size());
	const auto duration = 0.25;
	const auto overlap = 0.15;
	auto hq = PainterHighQualityEnabler(p);
	for (auto i = 0; i < count; ++i) {
		const auto center = count / 2;
		const auto offset = (i <= center)
			? (center - i)
			: (i - center);
		const auto start = offset * (duration - overlap);
		const auto scale = std::clamp(
			(_progress - start) / duration,
			0.,
			1.);
		if (scale > 0.) {
			const auto hoverScale = 1. + kHoverScaleIncrease * _scales[i];
			const auto alpha = _alphas[i];
			const auto cx = x + _userpicSize / 2;
			const auto cy = height() / 2;
			p.save();
			p.setOpacity(alpha);
			p.translate(cx, cy);
			p.scale(scale * hoverScale, scale * hoverScale);
			p.translate(-cx, -cy);
			const auto image = _thumbnails[i]->image(_userpicSize);
			p.drawImage(x, 0, image);
			p.restore();
		}
		x += _userpicSize + _gap;
	}
}

void DynamicImagesStrip::mouseMoveEvent(QMouseEvent *e) {
	const auto pos = e->pos().x();
	const auto step = _userpicSize + _gap;
	const auto count = int(_thumbnails.size());
	auto newIndex = -1;
	for (auto i = 0; i < count; ++i) {
		const auto start = i * step;
		const auto end = start + _userpicSize + _gap / 2;
		if (pos >= start && pos < end) {
			newIndex = i;
			break;
		}
	}
	if (_hoveredIndex != newIndex) {
		const auto prev = _hoveredIndex;
		_hoveredIndex = newIndex;
		if (prev >= 0) {
			_scaleTargets[prev] = 0.;
			_alphaTargets[prev] = kDimmedAlpha;
		}
		if (newIndex >= 0) {
			_scaleTargets[newIndex] = 1.;
			_alphaTargets[newIndex] = 1.;
			for (auto i = 0; i < count; ++i) {
				if (i != newIndex && _alphaTargets[i] > kDimmedAlpha) {
					_alphaTargets[i] = kDimmedAlpha;
				}
			}
			updateHoveredItem(newIndex);
		} else {
			updateHoveredItem(-1);
		}
		startAnimation();
	}
}

void DynamicImagesStrip::mousePressEvent(QMouseEvent *e) {
	if (_hoveredIndex >= 0 && _clickCallback) {
		_clickCallback(_hoveredIndex);
	}
}

void DynamicImagesStrip::leaveEventHook(QEvent *e) {
	if (_hoveredIndex >= 0) {
		_hoveredIndex = -1;
		for (auto i = 0; i < int(_thumbnails.size()); ++i) {
			_scaleTargets[i] = 0.;
			_alphaTargets[i] = 1.;
		}
		updateHoveredItem(-1);
		startAnimation();
	}
}

void DynamicImagesStrip::startAnimation() {
	if (!_animation.animating()) {
		_animation.start();
	}
}

void DynamicImagesStrip::updateHoveredItem(int index) {
	if (index < 0) {
		_hoveredItem.fire({ .index = -1, .globalPos = {} });
		return;
	}
	const auto step = _userpicSize + _gap;
	const auto shift = (height() - _userpicSize) / 2;
	const auto x = index * step + shift;
	const auto avatarRect = QRect(x, shift, _userpicSize, _userpicSize);
	const auto globalRect = QRect(
		mapToGlobal(avatarRect.topLeft()),
		avatarRect.size());
	_hoveredItem.fire({
		.index = index,
		.globalPos = globalRect.center() });
}

} // namespace Ui
