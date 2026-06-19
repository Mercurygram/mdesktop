/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"
#include "ui/rp_widget.h"
#include "ui/text/text.h"

class QMouseEvent;
class QWidget;

namespace Data {
struct Draft;
} // namespace Data

namespace Main {
class Session;
} // namespace Main

namespace HistoryView::Controls {

class RichDraftPreview final : public Ui::RpWidget {
public:
	RichDraftPreview(
		QWidget *parent,
		not_null<Main::Session*> session,
		Fn<bool()> paused,
		Fn<void()> activate);

	void setDraft(const Data::Draft &draft);
	[[nodiscard]] int resizeGetHeight(
		int width,
		int minHeight,
		int maxHeight);

private:
	void paint(QRect clip);
	[[nodiscard]] int contentHeightForWidth(int width) const;
	void mouseReleaseEvent(QMouseEvent *e) override;

	const not_null<Main::Session*> _session;
	const Fn<bool()> _paused;
	const Fn<void()> _activate;
	Ui::Text::String _summary;

};

} // namespace HistoryView::Controls
