/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "data/data_star_gift.h"
#include "ui/effects/animations.h"
#include "ui/text/text_custom_emoji.h"

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
		QImage gift;
		QImage percentBadge;
		QImage removeButton;
		QImage addButton;
		QRect originalRect;
		bool hasGift = false;
	};
	std::array<CornerSnapshot, 4> corners;

	QImage forgeImage;
	QRect forgeRect;

	int containerHeight = 0;
	int craftingTop = 0;
	int craftingBottom = 0;
	int craftingAreaCenterY = 0;

	void paint(QPainter &p, QSize size, int craftingHeight, float64 slideProgress = 0.);
	void updateForGiftCount(int count);

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
	float64 velocityX = 0.;
	float64 velocityY = 0.;
	crl::time lastRotationUpdate = 0;

	int currentlyFlying = -1;
	int giftsLanded = 0;
	int totalGifts = 0;
	std::array<FacePlacement, 4> giftToSide;
	Animations::Simple flightAnimation;

	bool snapping = false;
	Animations::Simple snapAnimation;
	float64 snapTargetX = 0.;
	float64 snapTargetY = 0.;
	float64 snapStartX = 0.;
	float64 snapStartY = 0.;

	crl::time nearlyStoppedSince = 0;

};

void StartCraftAnimation(
	not_null<VerticalLayout*> container,
	std::shared_ptr<CraftState> state);

} // namespace Ui
