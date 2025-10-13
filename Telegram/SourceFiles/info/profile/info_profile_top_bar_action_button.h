/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "ui/widgets/buttons.h"

namespace Lottie {
class Icon;
} // namespace Lottie

namespace Info::Profile {

class TopBarActionButton final : public Ui::RippleButton {
public:
	TopBarActionButton(
		not_null<QWidget*> parent,
		const QString &text,
		const QString &lottieName);
	TopBarActionButton(
		not_null<QWidget*> parent,
		const QString &text,
		const style::icon &icon);

protected:
	void paintEvent(QPaintEvent *e) override;

	QImage prepareRippleMask() const override;
	QPoint prepareRippleStartPosition() const override;

private:
	void setupLottie(const QString &lottieName);

	QString _text;
	std::unique_ptr<Lottie::Icon> _lottie;
	const style::icon *_icon = nullptr;

};

} // namespace Info::Profile
