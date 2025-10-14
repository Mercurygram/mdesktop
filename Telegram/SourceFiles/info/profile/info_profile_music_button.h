/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "ui/widgets/buttons.h"
#include "ui/text/text.h"

namespace Info::Profile {

struct MusicButtonData {
	QString performer;
	QString title;
};

class MusicButton final : public Ui::RippleButton {
public:
	MusicButton(QWidget *parent, MusicButtonData data, Fn<void()> handler);
	~MusicButton();

	void updateData(MusicButtonData data);

private:
	void paintEvent(QPaintEvent *e) override;
	int resizeGetHeight(int newWidth) override;

	Ui::Text::String _performer;
	Ui::Text::String _title;

};

} // namespace Info::Profile
