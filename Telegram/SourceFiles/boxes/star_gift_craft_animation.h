/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/unique_qptr.h"
#include "data/data_star_gift.h"
#include "ui/effects/animations.h"
#include "ui/text/text_custom_emoji.h"

namespace Info::PeerGifts {
class GiftButton;
} // namespace Info::PeerGifts

namespace Ui {

class VerticalLayout;

struct BackdropView {
	Data::UniqueGiftBackdrop colors;
	QImage gradient;

};

struct PatternView {
	std::unique_ptr<Text::CustomEmoji> emoji;
	base::flat_map<int, base::flat_map<float64, QImage>> emojis;

};

struct CraftState {
	struct Cover {
		BackdropView backdrop;
		PatternView pattern;
		QColor button1;
		QColor button2;
		Animations::Simple shownAnimation;
		bool shown = false;
	};

	std::array<Cover, 4> covers;
	rpl::variable<QColor> edgeColor;
	QColor button1;
	QColor button2;
	bool coversAnimate = false;
	Fn<void()> repaint;

	QImage topPart;
	QRect topPartRect;
	QImage bottomPart;
	int bottomPartY = 0;

	struct CornerSnapshot {
		base::unique_qptr<Info::PeerGifts::GiftButton> giftButton;
		mutable QImage giftFrame;
		QImage percentBadge;
		QImage removeButton;
		QImage addButton;
		QRect originalRect;
		mutable bool giftFrameFinal = false;

		[[nodiscard]] QImage gift(float64 progress) const;
	};
	std::array<CornerSnapshot, 4> corners;

	QRect forgeRect;

	QColor forgeBgOverlay;
	QColor forgeBg1;
	QColor forgeBg2;
	QImage forgePercent;

	std::array<QImage, 6> forgeImages;

	int containerHeight = 0;
	int craftingTop = 0;
	int craftingBottom = 0;
	int craftingAreaCenterY = 0;
	int craftingOffsetY = 0;

	void paint(QPainter &p, QSize size, int craftingHeight, float64 slideProgress = 0.);
	void updateForGiftCount(int count);
	[[nodiscard]] QImage prepareForgeImage(int index) const;

};

struct FacePlacement {
	int face = -1;
	int rotation = 0;

};

struct CraftAnimationState {
	std::shared_ptr<CraftState> shared;

	Animations::Simple slideOutAnimation;
	Animations::Basic continuousAnimation;

	float64 rotationX = 0.;
	float64 rotationY = 0.;

	int currentlyFlying = -1;
	int giftsLanded = 0;
	int totalGifts = 0;
	std::array<FacePlacement, 4> giftToSide;
	Animations::Simple flightAnimation;

	int currentConfigIndex = -1;
	crl::time animationStartTime = 0;
	float64 initialRotationX = 0.;
	float64 initialRotationY = 0.;
	int nextFaceIndex = 0;
	int nextFaceRotation = 0;

};

void StartCraftAnimation(
	not_null<VerticalLayout*> container,
	std::shared_ptr<CraftState> state);

} // namespace Ui
