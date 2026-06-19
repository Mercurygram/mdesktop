/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "history/view/controls/history_view_rich_draft_preview.h"

#include "core/ui_integration.h"
#include "data/data_drafts.h"
#include "data/data_session.h"
#include "main/main_session.h"
#include "ui/effects/spoiler_mess.h"
#include "ui/painter.h"
#include "ui/power_saving.h"
#include "ui/text/text_options.h"

#include <QtGui/QMouseEvent>

#include <algorithm>
#include <utility>

#include "styles/style_chat.h"
#include "styles/style_chat_helpers.h"

namespace HistoryView::Controls {

RichDraftPreview::RichDraftPreview(
	QWidget *parent,
	not_null<Main::Session*> session,
	Fn<bool()> paused,
	Fn<void()> activate)
: RpWidget(parent)
, _session(session)
, _paused(std::move(paused))
, _activate(std::move(activate)) {
	setCursor(style::cur_pointer);

	paintRequest(
	) | rpl::on_next([=](QRect clip) {
		paint(clip);
	}, lifetime());
}

void RichDraftPreview::setDraft(const Data::Draft &draft) {
	const auto context = Core::TextContext({
		.session = _session,
		.repaint = [=] { update(); },
		.customEmojiLoopLimit = 1,
	});
	_summary.setMarkedText(
		st::messageTextStyle,
		draft.richMessageSummary,
		Ui::DialogTextOptions(),
		context);
	update();
}

int RichDraftPreview::resizeGetHeight(
		int width,
		int minHeight,
		int maxHeight) {
	const auto height = std::min(
		std::max(contentHeightForWidth(width), minHeight),
		maxHeight);
	resize(width, height);
	return height;
}

void RichDraftPreview::paint(QRect clip) {
	Q_UNUSED(clip);

	Painter p(this);
	const auto paused = _paused && _paused();
	p.setInactive(paused);
	p.fillRect(rect(), st::historyComposeAreaBg);

	if (_summary.isEmpty()) {
		return;
	}

	const auto left = st::msgReplyPadding.left();
	const auto top = st::msgReplyPadding.top();
	const auto right = st::msgReplyPadding.right();
	const auto width = std::max(0, this->width() - left - right);
	const auto bodyHeight = height() - top - st::msgReplyPadding.top();
	const auto lines = std::max(1, bodyHeight / _summary.minHeight());
	p.setPen(st::historyComposeAreaFg);
	_summary.draw(p, {
		.position = QPoint(left, top),
		.availableWidth = width,
		.palette = &st::historyComposeAreaPalette,
		.spoiler = Ui::Text::DefaultSpoilerCache(),
		.now = crl::now(),
		.pausedEmoji = paused || On(PowerSaving::kEmojiChat),
		.pausedSpoiler = paused || On(PowerSaving::kChatSpoiler),
		.elisionLines = lines,
	});
}

int RichDraftPreview::contentHeightForWidth(int width) const {
	const auto left = st::msgReplyPadding.left();
	const auto right = st::msgReplyPadding.right();
	const auto available = std::max(0, width - left - right);
	const auto summaryHeight = _summary.isEmpty()
		? 0
		: std::min(
			_summary.countHeight(available),
			_summary.minHeight() * 2);
	return st::msgReplyPadding.top()
		+ summaryHeight
		+ st::msgReplyPadding.top();
}

void RichDraftPreview::mouseReleaseEvent(QMouseEvent *e) {
	if ((e->button() == Qt::LeftButton) && _activate) {
		_activate();
	}
	RpWidget::mouseReleaseEvent(e);
}

} // namespace HistoryView::Controls
