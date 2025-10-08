/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/object_ptr.h"
#include "ui/rp_widget.h"

class PeerData;

namespace style {
struct InfoTopBar;
} //namespace style

namespace Ui {
class FlatLabel;
} //namespace Ui

namespace Info::Profile {

class StatusLabel;

class TopBar final : public Ui::RpWidget {
public:
	TopBar(not_null<Ui::RpWidget*> parent, not_null<PeerData*> peer);
	~TopBar();

	void setRoundEdges(bool value);
	void setEnableBackButtonValue(rpl::producer<bool> &&producer);

protected:
	void paintEvent(QPaintEvent *e) override;

private:
	void paintEdges(QPainter &p, const QBrush &brush) const;
	void paintEdges(QPainter &p) const;

	const not_null<PeerData*> _peer;
	const style::InfoTopBar &_st;

	object_ptr<Ui::FlatLabel> _title;
	object_ptr<Ui::FlatLabel> _status;
	std::unique_ptr<StatusLabel> _statusLabel;

	rpl::variable<int> _onlineCount = 0;
	bool _roundEdges = true;

};

} // namespace Info::Profile
