/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/object_ptr.h"
#include "ui/rp_widget.h"
#include "info/profile/info_profile_badge.h"
#include "ui/userpic_view.h"

namespace Data {
class ForumTopic;
class DocumentMedia;
} // namespace Data

namespace Info::Profile {
class BadgeTooltip;
class TopicIconView;
} // namespace Info::Profile

namespace Lottie {
class Animation;
class MultiPlayer;
} // namespace Lottie

namespace Ui {
class VideoUserpicPlayer;
namespace Text {
class CustomEmoji;
} // namespace Text
} // namespace Ui

class PeerData;

namespace base {
class Timer;
} // namespace base

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
class HorizontalFitContainer;
} //namespace Ui

namespace Info {
class Controller;
enum class Wrap;
} //namespace Info

namespace Ui::Menu {
struct MenuCallback;
} //namespace Ui::Menu

namespace Info::Profile {

class Badge;
class StatusLabel;

class TopBar final : public Ui::RpWidget {
public:
	struct Descriptor {
		not_null<Controller*> controller;
		rpl::variable<bool> backToggles;
	};

	struct AnimatedPatternPoint {
		QPointF basePosition;
		float64 size;
		float64 startTime;
		float64 endTime;
	};

	TopBar(not_null<Ui::RpWidget*> parent, Descriptor descriptor);
	~TopBar();

	void setOnlineCount(rpl::producer<int> &&count);

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
	[[nodiscard]] int titleMostLeft() const;
	[[nodiscard]] int statusMostLeft() const;
	[[nodiscard]] QRect userpicGeometry() const;
	void updateUserpicButtonGeometry();
	void paintUserpic(QPainter &p);
	void updateVideoUserpic();
	void showTopBarMenu(not_null<Controller*> controller, bool check);
	void fillTopBarMenu(
		not_null<Controller*> controller,
		const Ui::Menu::MenuCallback &addAction);
	void setupUserpicButton(not_null<Controller*> controller);
	void setupActions(not_null<Controller*> controller);
	void setupButtons(
		not_null<Controller*> controller,
		rpl::producer<bool> backToggles);
	void setupShowLastSeen(not_null<Controller*> controller);
	void setupUniqueBadgeTooltip();
	void hideBadgeTooltip();
	void setupAnimatedPattern();
	void paintAnimatedPattern(QPainter &p, const QRect &rect);
	void setupPinnedToTopGifts();
	void paintPinnedToTopGifts(QPainter &p, const QRect &rect);

	const not_null<PeerData*> _peer;
	Data::ForumTopic *_topic = nullptr;
	const style::InfoTopBar &_st;

	std::unique_ptr<base::Timer> _badgeTooltipHide;
	const std::unique_ptr<Badge> _botVerify;
	rpl::variable<Badge::Content> _badgeContent;
	const std::unique_ptr<Badge> _badge;
	const std::unique_ptr<Badge> _verified;

	std::unique_ptr<BadgeTooltip> _badgeTooltip;
	std::vector<std::unique_ptr<BadgeTooltip>> _badgeOldTooltips;
	uint64 _badgeCollectibleId = 0;

	object_ptr<Ui::FlatLabel> _title;
	std::unique_ptr<Ui::StarsRating> _starsRating;
	object_ptr<Ui::FlatLabel> _status;
	std::unique_ptr<StatusLabel> _statusLabel;
	rpl::variable<int> _statusShift = 0;
	object_ptr<Ui::RoundButton> _showLastSeen = { nullptr };
	QGraphicsOpacityEffect *_showLastSeenOpacity = nullptr;

	rpl::variable<float64> _progress = 0.;
	bool _roundEdges = true;

	bool _hasBackground = false;
	QImage _cachedGradient;
	QPainterPath _cachedClipPath;
	std::unique_ptr<Ui::Text::CustomEmoji> _patternEmoji;
	QImage _basePatternImage;
	base::flat_map<float64, QImage> _patternEmojis;

	std::vector<AnimatedPatternPoint> _animatedPoints;
	QRect _lastUserpicRect;

	Ui::PeerUserpicView _userpicView;
	InMemoryKey _userpicUniqueKey;
	QImage _cachedUserpic;
	QImage _monoforumMask;
	std::unique_ptr<Ui::VideoUserpicPlayer> _videoUserpicPlayer;
	std::unique_ptr<TopicIconView> _topicIconView;

	base::unique_qptr<Ui::IconButton> _close;
	base::unique_qptr<Ui::FadeWrap<Ui::IconButton>> _back;

	base::unique_qptr<Ui::IconButton> _topBarMenuToggle;
	base::unique_qptr<Ui::PopupMenu> _topBarMenu;

	base::unique_qptr<Ui::IconButton> _callsButton;

	base::unique_qptr<Ui::AbstractButton> _userpicButton;

	base::unique_qptr<Ui::HorizontalFitContainer> _actions;

	std::unique_ptr<Lottie::MultiPlayer> _lottiePlayer;
	struct PinnedToTopGiftEntry {
		Lottie::Animation *animation = nullptr;
		std::shared_ptr<Data::DocumentMedia> media;
		QImage bg;
		int position = 0;
	};
	std::vector<PinnedToTopGiftEntry> _pinnedToTopGifts;
	rpl::lifetime _giftsLoadingLifetime;

};

} // namespace Info::Profile
