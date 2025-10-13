/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "info/profile/info_profile_top_bar_action_button.h"

#include "ui/effects/ripple_animation.h"
#include "ui/painter.h"
#include "ui/rect.h"
#include "lottie/lottie_icon.h"
#include "styles/style_layers.h"
#include "styles/style_info.h"

namespace Info::Profile {
namespace {

constexpr auto kIconFadeStart = 0.4;
constexpr auto kIconFadeRange = 1.0 - kIconFadeStart;
constexpr auto kBgOpacity = 40;

} // namespace

TopBarActionButton::TopBarActionButton(
	not_null<QWidget*> parent,
	const QString &text,
	const QString &lottieName)
: RippleButton(parent, st::universalRippleAnimation)
, _text(text) {
	setupLottie(lottieName);
}

void TopBarActionButton::setupLottie(const QString &lottieName) {
	_lottie = std::make_unique<Lottie::Icon>(Lottie::IconDescriptor{
		.name = lottieName,
		.sizeOverride = Size(st::infoProfileTopBarActionButtonLottieSize),
	});
	_lottie->animate([=] { update(); }, 0, _lottie->framesCount() - 1);
}

void TopBarActionButton::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);

	const auto progress = float64(height())
		/ st::infoProfileTopBarActionButtonSize;
	p.setOpacity(progress);

	const auto bg = QColor(0, 0, 0, kBgOpacity);
	p.setPen(Qt::NoPen);
	p.setBrush(bg);
	{
		auto hq = PainterHighQualityEnabler(p);
		p.drawRoundedRect(rect(), st::boxRadius, st::boxRadius);
	}

	paintRipple(p, 0, 0);

	const auto iconSize = st::infoProfileTopBarActionButtonIconSize;
	const auto iconTop = st::infoProfileTopBarActionButtonIconTop;

	if (_lottie) {
		const auto iconScale = (progress > kIconFadeStart)
			? (progress - kIconFadeStart) / kIconFadeRange
			: 0.0;
		p.setOpacity(iconScale);
		p.save();
		const auto iconLeft = (width() - iconSize) / 2;
		const auto half = iconSize / 2;
		const auto iconCenter = QPoint(iconLeft + half, iconTop + half);
		p.translate(iconCenter);
		p.scale(iconScale, iconScale);
		p.translate(-iconCenter);
		_lottie->paint(p, iconLeft, iconTop);
		p.restore();
		p.setOpacity(progress);
	}

	const auto skip = st::infoProfileTopBarActionButtonTextSkip;

	p.setClipRect(0, 0, width(), height() - skip);
	p.setPen(st::premiumButtonFg);
	p.setFont(st::normalFont);

	const auto textScale = std::max(kIconFadeStart, progress);
	const auto textRect = rect() - QMargins(0, iconTop + iconSize, 0, 0);
	const auto textCenter = rect::center(textRect);
	p.translate(textCenter);
	p.scale(textScale, textScale);
	p.translate(-textCenter);
	p.drawText(textRect, _text, style::al_top);
}

QImage TopBarActionButton::prepareRippleMask() const {
	return Ui::RippleAnimation::RoundRectMask(size(), st::boxRadius);
}

QPoint TopBarActionButton::prepareRippleStartPosition() const {
	return mapFromGlobal(QCursor::pos());
}

} // namespace Info::Profile
