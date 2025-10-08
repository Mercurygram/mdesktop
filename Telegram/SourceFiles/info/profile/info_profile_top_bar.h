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

class QGraphicsOpacityEffect;

namespace Ui {
class FlatLabel;
class IconButton;
class PopupMenu;
class RoundButton;
class StarsRating;
template <typename Widget>
class FadeWrap;
} //namespace Ui

namespace Info {
class Controller;
enum class Wrap;
} //namespace Info

namespace Ui::Menu {
struct MenuCallback;
} //namespace Ui::Menu

namespace Info::Profile {

class StatusLabel;

class TopBar final : public Ui::RpWidget {
public:
	struct Descriptor {
		not_null<Controller*> controller;
		rpl::variable<bool> backToggles;
	};

	TopBar(not_null<Ui::RpWidget*> parent, Descriptor descriptor);
	~TopBar();

	void setRoundEdges(bool value);
	void setEnableBackButtonValue(rpl::producer<bool> &&producer);
	void addTopBarMenuButton(
		not_null<Controller*> controller,
		Wrap wrap);
	void addProfileCallsButton(not_null<Controller*> controller, Wrap wrap);

protected:
	void resizeEvent(QResizeEvent *e) override;
	void paintEvent(QPaintEvent *e) override;

private:
	void paintEdges(QPainter &p, const QBrush &brush) const;
	void paintEdges(QPainter &p) const;
	void updateLabelsPosition();
	void paintUserpic(QPainter &p);
	void showTopBarMenu(not_null<Controller*> controller, bool check);
	void fillTopBarMenu(
		not_null<Controller*> controller,
		const Ui::Menu::MenuCallback &addAction);
	void setupButtons(
		not_null<Controller*> controller,
		rpl::producer<bool> backToggles);
	void setupShowLastSeen(not_null<Controller*> controller);

	const not_null<PeerData*> _peer;
	const style::InfoTopBar &_st;

	object_ptr<Ui::FlatLabel> _title;
	std::unique_ptr<Ui::StarsRating> _starsRating;
	object_ptr<Ui::FlatLabel> _status;
	std::unique_ptr<StatusLabel> _statusLabel;
	rpl::variable<int> _statusShift = 0;
	object_ptr<Ui::RoundButton> _showLastSeen = { nullptr };
	QGraphicsOpacityEffect *_showLastSeenOpacity = nullptr;

	rpl::variable<int> _onlineCount = 0;
	rpl::variable<float64> _progress = 0.;
	bool _roundEdges = true;

	Ui::PeerUserpicView _userpicView;
	InMemoryKey _userpicUniqueKey;
	QImage _cachedUserpic;

	base::unique_qptr<Ui::IconButton> _close;
	base::unique_qptr<Ui::FadeWrap<Ui::IconButton>> _back;

	base::unique_qptr<Ui::IconButton> _topBarMenuToggle;
	base::unique_qptr<Ui::PopupMenu> _topBarMenu;

	base::unique_qptr<Ui::IconButton> _callsButton;

};

} // namespace Info::Profile
