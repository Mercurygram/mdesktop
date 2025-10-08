/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/object_ptr.h"
#include "ui/rp_widget.h"
#include "ui/userpic_view.h"

class PeerData;

namespace style {
struct InfoTopBar;
} //namespace style

namespace Ui {
class FlatLabel;
} //namespace Ui

namespace Info {
class Controller;
} //namespace Info

namespace Info::Profile {

class StatusLabel;

class TopBar final : public Ui::RpWidget {
public:
	struct Descriptor {
		not_null<Controller*> controller;
	};

	TopBar(not_null<Ui::RpWidget*> parent, Descriptor descriptor);
	~TopBar();

	void setRoundEdges(bool value);
	void setEnableBackButtonValue(rpl::producer<bool> &&producer);

protected:
	void resizeEvent(QResizeEvent *e) override;
	void paintEvent(QPaintEvent *e) override;

private:
	void paintEdges(QPainter &p, const QBrush &brush) const;
	void paintEdges(QPainter &p) const;
	void updateLabelsPosition();
	void paintUserpic(QPainter &p);

	const not_null<PeerData*> _peer;
	const style::InfoTopBar &_st;

	object_ptr<Ui::FlatLabel> _title;
	object_ptr<Ui::FlatLabel> _status;
	std::unique_ptr<StatusLabel> _statusLabel;

	rpl::variable<int> _onlineCount = 0;
	rpl::variable<float64> _progress = 0.;
	bool _roundEdges = true;

	Ui::PeerUserpicView _userpicView;
	InMemoryKey _userpicUniqueKey;
	QImage _cachedUserpic;

};

} // namespace Info::Profile
