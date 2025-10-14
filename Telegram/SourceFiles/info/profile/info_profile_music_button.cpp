/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "info/profile/info_profile_music_button.h"

#include "ui/effects/animation_value.h"
#include "ui/text/text_utilities.h"
#include "ui/painter.h"
#include "ui/rect.h"
#include "ui/ui_utility.h"
#include "styles/style_chat.h"
#include "styles/style_info.h"

namespace Info::Profile {

MusicButton::MusicButton(
	QWidget *parent,
	MusicButtonData data,
	Fn<void()> handler)
: RippleButton(parent, st::infoMusicButtonRipple) {
	updateData(std::move(data));
	setClickedCallback(std::move(handler));
}

MusicButton::~MusicButton() = default;

void MusicButton::updateData(MusicButtonData data) {
	_performer.setText(st::semiboldTextStyle, data.performer);
	_title.setText(
		st::defaultTextStyle,
		data.performer.isEmpty() ? data.title : (u"- "_q + data.title));
	update();
}

void MusicButton::setOverrideBg(std::optional<QColor> color) {
	_overrideBg = color;
	update();
}

void MusicButton::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);

	if (_overrideBg) {
		p.fillRect(e->rect(), Ui::BlendColors(
			*_overrideBg,
			Qt::black,
			st::infoProfileTopBarActionButtonBgOpacity));
		paintRipple(p, QPoint(), &st::universalRippleAnimation.color->c);
	} else {
		p.fillRect(e->rect(), st::windowBgOver);
		paintRipple(p, QPoint());
	}

	const auto &icon = st::topicButtonArrow;
	const auto iconWidth = icon.width();
	const auto iconHeight = icon.height();

	const auto padding = st::infoMusicButtonPadding;
	const auto noteSymbol = u"\u266B"_q + QChar(' ');
	const auto noteWidth = st::normalFont->width(noteSymbol);
	const auto skip = st::normalFont->spacew;

	const auto titleWidth = _title.maxWidth();
	const auto performerWidth = _performer.maxWidth();
	const auto totalNeeded = titleWidth + performerWidth + skip;
	const auto availableWidth = width()
		- rect::m::sum::h(padding)
		- iconWidth
		- skip
		- noteWidth;

	auto actualTitleWidth = 0;
	auto actualPerformerWidth = 0;
	if (totalNeeded <= availableWidth) {
		actualTitleWidth = titleWidth;
		actualPerformerWidth = performerWidth;
	} else {
		const auto ratio = float64(titleWidth) / totalNeeded;
		actualPerformerWidth = int(availableWidth * (1.0 - ratio));
		actualTitleWidth = availableWidth - actualPerformerWidth;
	}

	const auto totalContentWidth = noteWidth
		+ actualPerformerWidth
		+ skip
		+ actualTitleWidth
		+ skip
		+ iconWidth;
	const auto centerX = width() / 2;
	const auto contentStartX = centerX - totalContentWidth / 2;
	const auto textTop = (height() - st::normalFont->height) / 2;

	p.setPen(_overrideBg ? st::groupCallMembersFg : st::windowBoldFg);
	p.setFont(st::normalFont);
	p.drawText(contentStartX, textTop + st::normalFont->ascent, noteSymbol);

	_performer.draw(p, {
		.position = { contentStartX + noteWidth, textTop },
		.availableWidth = actualPerformerWidth,
		.now = crl::now(),
		.elisionLines = 1,
		.elisionMiddle = true,
	});

	p.setPen(_overrideBg ? st::groupCallVideoSubTextFg : st::windowSubTextFg);
	_title.draw(p, {
		.position = QPoint(
			contentStartX + noteWidth + actualPerformerWidth,
			textTop),
		.availableWidth = actualTitleWidth,
		.now = crl::now(),
		.elisionLines = 1,
		.elisionMiddle = true,
	});

	const auto iconLeft = contentStartX
		+ noteWidth
		+ actualPerformerWidth
		+ actualTitleWidth
		+ skip;
	const auto iconTop = (height() - iconHeight) / 2;
	icon.paint(p, iconLeft, iconTop, iconWidth, p.pen().color());
}

int MusicButton::resizeGetHeight(int newWidth) {
	const auto padding = st::infoMusicButtonPadding;
	const auto &font = st::defaultTextStyle.font;

	return padding.top() + font->height + padding.bottom();
}

} // namespace Info::Profile
