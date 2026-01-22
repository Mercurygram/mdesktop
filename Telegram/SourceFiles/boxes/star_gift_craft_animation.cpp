/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "boxes/star_gift_craft_animation.h"

#include "info/peer_gifts/info_peer_gifts_common.h"
#include "ui/image/image_prepare.h"
#include "ui/top_background_gradient.h"
#include "ui/painter.h"
#include "ui/wrap/vertical_layout.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"

namespace Ui {
namespace {

using namespace Info::PeerGifts;

[[nodiscard]] QImage CreateBgGradient(
		QSize size,
		const Data::UniqueGiftBackdrop &backdrop) {
	const auto ratio = style::DevicePixelRatio();
	auto result = QImage(size * ratio, QImage::Format_ARGB32_Premultiplied);
	result.setDevicePixelRatio(ratio);

	auto p = QPainter(&result);
	auto hq = PainterHighQualityEnabler(p);
	auto gradient = QRadialGradient(
		QPoint(size.width() / 2, size.height() / 2),
		size.height() / 2);
	gradient.setStops({
		{ 0., backdrop.centerColor },
		{ 1., backdrop.edgeColor },
	});
	p.setBrush(gradient);
	p.setPen(Qt::NoPen);
	p.drawRect(0, 0, size.width(), size.height());
	p.end();

	const auto mask = Images::CornersMask(st::boxRadius);
	return Images::Round(std::move(result), mask);
}

[[nodiscard]] std::optional<std::array<QPointF, 4>> ComputeCubeFaceCorners(
		QPointF center,
		float64 size,
		float64 rotationX,
		float64 rotationY,
		int faceIndex) {
	struct Vec3 {
		float64 x, y, z;

		Vec3 operator+(const Vec3 &o) const {
			return { x + o.x, y + o.y, z + o.z };
		}
		Vec3 operator-(const Vec3 &o) const {
			return { x - o.x, y - o.y, z - o.z };
		}
		Vec3 operator*(float64 s) const {
			return { x * s, y * s, z * s };
		}
		[[nodiscard]] Vec3 cross(const Vec3 &o) const {
			return {
				y * o.z - z * o.y,
				z * o.x - x * o.z,
				x * o.y - y * o.x
			};
		}
		[[nodiscard]] float64 dot(const Vec3 &o) const {
			return x * o.x + y * o.y + z * o.z;
		}
		[[nodiscard]] Vec3 normalized() const {
			const auto len = std::sqrt(x * x + y * y + z * z);
			return (len > 0.) ? Vec3{ x / len, y / len, z / len } : *this;
		}
	};

	const auto rotateY = [](Vec3 v, float64 angle) {
		const auto c = std::cos(angle);
		const auto s = std::sin(angle);
		return Vec3{ v.x * c + v.z * s, v.y, -v.x * s + v.z * c };
	};
	const auto rotateX = [](Vec3 v, float64 angle) {
		const auto c = std::cos(angle);
		const auto s = std::sin(angle);
		return Vec3{ v.x, v.y * c - v.z * s, v.y * s + v.z * c };
	};

	const auto half = size / 2.;
	constexpr auto kFocalLength = 800.;

	struct FaceDefinition {
		std::array<Vec3, 4> corners;
		Vec3 normal;
	};

	const auto faces = std::array<FaceDefinition, 6>{{
		{{{{ -half, -half, -half },
		   {  half, -half, -half },
		   {  half,  half, -half },
		   { -half,  half, -half }}},
		 { 0., 0., -1. }},

		{{{{  half, -half,  half },
		   { -half, -half,  half },
		   { -half,  half,  half },
		   {  half,  half,  half }}},
		 { 0., 0., 1. }},

		{{{{ -half, -half,  half },
		   { -half, -half, -half },
		   { -half,  half, -half },
		   { -half,  half,  half }}},
		 { -1., 0., 0. }},

		{{{{  half, -half, -half },
		   {  half, -half,  half },
		   {  half,  half,  half },
		   {  half,  half, -half }}},
		 { 1., 0., 0. }},

		{{{{ -half, -half,  half },
		   {  half, -half,  half },
		   {  half, -half, -half },
		   { -half, -half, -half }}},
		 { 0., -1., 0. }},

		{{{{ -half,  half, -half },
		   {  half,  half, -half },
		   {  half,  half,  half },
		   { -half,  half,  half }}},
		 { 0., 1., 0. }},
	}};

	if (faceIndex < 0 || faceIndex >= 6) {
		return std::nullopt;
	}

	const auto &face = faces[faceIndex];

	auto transformedNormal = rotateX(rotateY(face.normal, rotationY), rotationX);
	if (transformedNormal.z >= 0.) {
		return std::nullopt;
	}

	auto result = std::array<QPointF, 4>();
	for (auto i = 0; i != 4; ++i) {
		auto p = rotateX(rotateY(face.corners[i], rotationY), rotationX);

		const auto viewZ = p.z + half + kFocalLength;
		if (viewZ <= 0.) {
			return std::nullopt;
		}

		const auto scale = kFocalLength / viewZ;
		result[i] = QPointF(
			center.x() + p.x * scale,
			center.y() + p.y * scale);
	}

	return result;
}

void PaintCubeFace(
		QPainter &p,
		const QImage &face,
		const std::array<QPointF, 4> &corners,
		int rotation = 0) {
	if (face.isNull()) {
		return;
	}

	const auto ratio = style::DevicePixelRatio();
	const auto w = face.width() / ratio;
	const auto h = face.height() / ratio;

	const auto srcRect = QPolygonF({
		QPointF(0, 0),
		QPointF(w, 0),
		QPointF(w, h),
		QPointF(0, h)
	});

	const auto rotationSteps = (rotation / 90) % 4;
	auto rotatedCorners = corners;
	for (auto i = 0; i < rotationSteps; ++i) {
		rotatedCorners = {
			rotatedCorners[1],
			rotatedCorners[2],
			rotatedCorners[3],
			rotatedCorners[0],
		};
	}

	const auto dstRect = QPolygonF({
		rotatedCorners[0],
		rotatedCorners[1],
		rotatedCorners[2],
		rotatedCorners[3]
	});

	QTransform transform;
	if (!QTransform::quadToQuad(srcRect, dstRect, transform)) {
		return;
	}

	PainterHighQualityEnabler hq(p);
	p.save();
	p.setTransform(transform, true);
	p.drawImage(QRectF(0, 0, w, h), face);
	p.restore();
}

[[nodiscard]] std::vector<int> GetVisibleCubeFaces(
		float64 rotationX,
		float64 rotationY) {
	struct Vec3 {
		float64 x, y, z;

		Vec3 operator*(float64 s) const {
			return { x * s, y * s, z * s };
		}
	};

	const auto rotateY = [](Vec3 v, float64 angle) {
		const auto c = std::cos(angle);
		const auto s = std::sin(angle);
		return Vec3{ v.x * c + v.z * s, v.y, -v.x * s + v.z * c };
	};
	const auto rotateX = [](Vec3 v, float64 angle) {
		const auto c = std::cos(angle);
		const auto s = std::sin(angle);
		return Vec3{ v.x, v.y * c - v.z * s, v.y * s + v.z * c };
	};

	constexpr auto normals = std::array<Vec3, 6>{{
		{ 0., 0., -1. },
		{ 0., 0., 1. },
		{ -1., 0., 0. },
		{ 1., 0., 0. },
		{ 0., -1., 0. },
		{ 0., 1., 0. },
	}};

	struct FaceDepth {
		int index;
		float64 z;
	};
	auto visible = std::vector<FaceDepth>();
	visible.reserve(3);

	for (auto i = 0; i != 6; ++i) {
		auto transformed = rotateX(rotateY(normals[i], rotationY), rotationX);
		if (transformed.z < 0.) {
			visible.push_back({ i, transformed.z });
		}
	}

	ranges::sort(visible, ranges::greater(), &FaceDepth::z);

	auto result = std::vector<int>();
	result.reserve(visible.size());
	for (const auto &face : visible) {
		result.push_back(face.index);
	}
	return result;
}

void PaintCube(
		QPainter &p,
		const CraftAnimationState &animState,
		QPointF center,
		float64 size) {
	const auto &shared = animState.shared;

	const auto faces = GetVisibleCubeFaces(animState.rotationX, animState.rotationY);
	auto painted = false;

	for (const auto faceIndex : faces) {
		const auto corners = ComputeCubeFaceCorners(
			center,
			size,
			animState.rotationX,
			animState.rotationY,
			faceIndex);
		if (!corners) {
			continue;
		}

		const QImage *faceImage = &shared->forgeImage;
		auto faceRotation = 0;

		for (auto i = 0; i < 4; ++i) {
			if (i != animState.currentlyFlying
				&& animState.giftToSide[i].face == faceIndex
				&& shared->corners[i].hasGift
				&& !shared->corners[i].gift.isNull()) {
				faceImage = &shared->corners[i].gift;
				faceRotation = animState.giftToSide[i].rotation;
				break;
			}
		}

		if (!faceImage->isNull()) {
			PaintCubeFace(p, *faceImage, *corners, faceRotation);
			painted = true;
		}
	}
}

struct GiftFlightPosition {
	QPointF origin;
	float64 scale;
};

[[nodiscard]] GiftFlightPosition ComputeGiftFlightPosition(
		QRect originalRect,
		QPointF targetCenter,
		float64 targetSize,
		float64 progress,
		float64 startOffsetY) {
	const auto eased = progress;

	const auto startOrigin = QPointF(originalRect.topLeft()) + QPointF(0, startOffsetY);
	const auto targetOrigin = targetCenter - QPointF(targetSize, targetSize) / 2.;
	const auto currentOrigin = startOrigin + (targetOrigin - startOrigin) * eased;

	const auto originalSize = std::max(originalRect.width(), originalRect.height());
	const auto endScale = (originalSize > 0.)
		? (targetSize / float64(originalSize))
		: 1.;
	const auto currentScale = 1. + (endScale - 1.) * eased;

	return {
		.origin = currentOrigin,
		.scale = currentScale,
	};
}

void ApplyRotationImpulse(CraftAnimationState &state, int cornerIndex) {
	constexpr auto kImpulseStrength = 2.0;

	const auto impulseX = ((cornerIndex < 2) ? 1 : -1) * 0.5;
	const auto impulseY = ((cornerIndex % 2) ? 1 : -1) * 1.;

	state.velocityX += impulseX * kImpulseStrength;
	state.velocityY += impulseY * kImpulseStrength;

	if (!state.continuousAnimation.animating()) {
		state.lastRotationUpdate = 0;
		state.continuousAnimation.start();
	}
}

[[nodiscard]] bool IsCubeNearlyStopped(const CraftAnimationState &state) {
	constexpr auto kNearlyStoppedThreshold = 0.05;
	return (std::abs(state.velocityX) < kNearlyStoppedThreshold)
		&& (std::abs(state.velocityY) < kNearlyStoppedThreshold);
}

[[nodiscard]] FacePlacement GetCameraFacingFreeFace(
		const CraftAnimationState &state) {
	struct Vec3 {
		float64 x, y, z;
	};

	const auto rotateY = [](Vec3 v, float64 angle) {
		const auto c = std::cos(angle);
		const auto s = std::sin(angle);
		return Vec3{ v.x * c + v.z * s, v.y, -v.x * s + v.z * c };
	};
	const auto rotateX = [](Vec3 v, float64 angle) {
		const auto c = std::cos(angle);
		const auto s = std::sin(angle);
		return Vec3{ v.x, v.y * c - v.z * s, v.y * s + v.z * c };
	};

	constexpr auto normals = std::array<Vec3, 6>{{
		{ 0., 0., -1. },
		{ 0., 0., 1. },
		{ -1., 0., 0. },
		{ 1., 0., 0. },
		{ 0., -1., 0. },
		{ 0., 1., 0. },
	}};

	constexpr auto faceUpVectors = std::array<Vec3, 6>{{
		{ 0., -1., 0. },
		{ 0., -1., 0. },
		{ 0., -1., 0. },
		{ 0., -1., 0. },
		{ 0., 0., -1. },
		{ 0., 0., 1. },
	}};

	const auto isOccupied = [&](int faceIndex) {
		for (const auto &placement : state.giftToSide) {
			if (placement.face == faceIndex) {
				return true;
			}
		}
		return false;
	};

	auto bestFace = -1;
	auto bestZ = 0.;

	for (auto i = 0; i != 6; ++i) {
		if (isOccupied(i)) {
			continue;
		}
		const auto transformed = rotateX(
			rotateY(normals[i], state.rotationY),
			state.rotationX);
		if (bestFace < 0 || transformed.z < bestZ) {
			bestFace = i;
			bestZ = transformed.z;
		}
	}

	if (bestFace < 0) {
		return {};
	}

	const auto faceUp = rotateX(
		rotateY(faceUpVectors[bestFace], state.rotationY),
		state.rotationX);

	const auto screenUpY = faceUp.y;
	const auto screenUpX = faceUp.x;

	auto rotation = 0;
	if (std::abs(screenUpY) >= std::abs(screenUpX)) {
		rotation = (screenUpY < 0.) ? 0 : 180;
	} else {
		rotation = (screenUpX < 0.) ? 90 : 270;
	}

	return { .face = bestFace, .rotation = rotation };
}

void PaintFlyingGift(
		QPainter &p,
		const CraftState::CornerSnapshot &corner,
		const GiftFlightPosition &position) {
	if (corner.gift.isNull()) {
		return;
	}

	const auto ratio = style::DevicePixelRatio();
	const auto giftSize = QSizeF(corner.gift.size()) / ratio;
	const auto scaledSize = giftSize * position.scale;
	const auto giftTopLeft = position.origin;

	auto hq = PainterHighQualityEnabler(p);
	p.drawImage(QRectF(giftTopLeft, scaledSize), corner.gift);
}

void PaintSlideOutCorner(
		QPainter &p,
		const CraftState::CornerSnapshot &corner,
		float64 offsetY,
		float64 progress) {
	if (corner.originalRect.isEmpty()) {
		return;
	}

	const auto ratio = style::DevicePixelRatio();
	const auto originalTopLeft = QPointF(corner.originalRect.topLeft());
	const auto currentTopLeft = originalTopLeft + QPointF(0, offsetY);
	const auto giftSize = QSizeF(corner.gift.size()) / ratio;
	const auto giftTopLeft = currentTopLeft;

	const auto &extend = st::defaultDropdownMenu.wrap.shadow.extend;
	const auto innerTopLeft = giftTopLeft + QPointF(extend.left(), extend.top());
	const auto innerWidth = giftSize.width() - extend.left() - extend.right();

	if (corner.hasGift) {
		if (!corner.gift.isNull()) {
			p.drawImage(giftTopLeft, corner.gift);
		}

		const auto badgeFade = std::clamp(1. - progress / 0.7, 0., 1.);
		if (!corner.percentBadge.isNull() && badgeFade > 0.) {
			const auto badgeSize = QSizeF(corner.percentBadge.size()) / ratio;
			const auto badgePos = innerTopLeft - QPointF(badgeSize.width() / 3., badgeSize.height() / 3.);
			p.setOpacity(badgeFade);
			p.drawImage(badgePos, corner.percentBadge);
			p.setOpacity(1.);
		}

		if (!corner.removeButton.isNull() && badgeFade > 0.) {
			const auto removeSize = QSizeF(corner.removeButton.size()) / ratio;
			const auto removePos = innerTopLeft
				+ QPointF(innerWidth - removeSize.width() + removeSize.width() / 3., -removeSize.height() / 3.);
			p.setOpacity(badgeFade);
			p.drawImage(removePos, corner.removeButton);
			p.setOpacity(1.);
		}
	} else {
		if (!corner.addButton.isNull()) {
			const auto fadeProgress = std::clamp(progress * 1.5, 0., 1.);
			const auto addTopLeft = currentTopLeft;
			p.setOpacity(1. - fadeProgress);
			p.drawImage(addTopLeft, corner.addButton);
			p.setOpacity(1.);
		}
	}
}

void PaintSlideOutPhase(
		QPainter &p,
		const std::shared_ptr<CraftState> &state,
		QSize canvasSize,
		float64 progress) {
	const auto ratio = style::DevicePixelRatio();

	if (!state->topPart.isNull()) {
		const auto topSize = QSizeF(state->topPart.size()) / ratio;
		const auto slideDistance = topSize.height();
		const auto offsetY = slideDistance * progress;
		const auto opacity = 1. - progress;

		p.setOpacity(opacity);
		p.drawImage(state->topPartRect.topLeft() - QPointF(0, offsetY), state->topPart);
		p.setOpacity(1.);
	}

	if (!state->bottomPart.isNull()) {
		const auto slideDistance = QSizeF(state->bottomPart.size()).height() / ratio;
		const auto offsetY = slideDistance * progress;
		const auto opacity = 1. - progress;

		p.setOpacity(opacity);
		p.drawImage(QPointF(0, state->bottomPartY + offsetY), state->bottomPart);
		p.setOpacity(1.);
	}

	const auto targetCenterY = state->containerHeight / 2.;
	const auto craftingOffsetY = (targetCenterY - state->craftingAreaCenterY) * progress;

	for (const auto &corner : state->corners) {
		PaintSlideOutCorner(p, corner, craftingOffsetY, progress);
	}

	if (!state->forgeImage.isNull()) {
		const auto forgePos = QPointF(state->forgeRect.topLeft()) + QPointF(0, craftingOffsetY);
		p.drawImage(forgePos, state->forgeImage);
	}
}

[[nodiscard]] std::pair<float64, float64> FindSnapTarget(
		const CraftAnimationState &state) {
	constexpr auto kPiOver2 = M_PI / 2.;

	const auto isOccupied = [&](int faceIndex) {
		for (const auto &placement : state.giftToSide) {
			if (placement.face == faceIndex) {
				return true;
			}
		}
		return false;
	};

	const auto normalizeAngle = [](float64 angle) {
		auto result = angle;
		while (result > M_PI) {
			result -= 2. * M_PI;
		}
		while (result <= -M_PI) {
			result += 2. * M_PI;
		}
		return result;
	};

	const auto angleDiff = [&](float64 from, float64 to) {
		return normalizeAngle(to - from);
	};

	struct Vec3 {
		float64 x, y, z;
	};

	const auto rotateY = [](Vec3 v, float64 angle) {
		const auto c = std::cos(angle);
		const auto s = std::sin(angle);
		return Vec3{ v.x * c + v.z * s, v.y, -v.x * s + v.z * c };
	};
	const auto rotateX = [](Vec3 v, float64 angle) {
		const auto c = std::cos(angle);
		const auto s = std::sin(angle);
		return Vec3{ v.x, v.y * c - v.z * s, v.y * s + v.z * c };
	};

	constexpr auto normals = std::array<Vec3, 6>{{
		{ 0., 0., -1. },
		{ 0., 0., 1. },
		{ -1., 0., 0. },
		{ 1., 0., 0. },
		{ 0., -1., 0. },
		{ 0., 1., 0. },
	}};

	const auto getFrontFace = [&](float64 rotX, float64 rotY) {
		auto bestFace = 0;
		auto bestZ = 0.;
		for (auto i = 0; i != 6; ++i) {
			const auto transformed = rotateX(rotateY(normals[i], rotY), rotX);
			if (i == 0 || transformed.z < bestZ) {
				bestFace = i;
				bestZ = transformed.z;
			}
		}
		return bestFace;
	};

	auto bestTargetX = 0.;
	auto bestTargetY = 0.;
	auto bestDistance = std::numeric_limits<float64>::max();

	for (auto ix = 0; ix != 4; ++ix) {
		for (auto iy = 0; iy != 4; ++iy) {
			const auto targetX = ix * kPiOver2;
			const auto targetY = iy * kPiOver2;

			const auto frontFace = getFrontFace(targetX, targetY);
			if (isOccupied(frontFace)) {
				continue;
			}

			const auto diffX = angleDiff(state.rotationX, targetX);
			const auto diffY = angleDiff(state.rotationY, targetY);
			const auto distance = diffX * diffX + diffY * diffY;

			if (distance < bestDistance) {
				bestDistance = distance;
				bestTargetX = state.rotationX + diffX;
				bestTargetY = state.rotationY + diffY;
			}
		}
	}

	return { bestTargetX, bestTargetY };
}

void StartSnapAnimation(
		CraftAnimationState *animState,
		not_null<RpWidget*> canvas) {
	animState->snapStartX = animState->rotationX;
	animState->snapStartY = animState->rotationY;

	const auto [targetX, targetY] = FindSnapTarget(*animState);
	animState->snapTargetX = targetX;
	animState->snapTargetY = targetY;

	animState->velocityX = 0.;
	animState->velocityY = 0.;
	animState->snapping = true;

	animState->snapAnimation.start(
		[=] { canvas->update(); },
		0.,
		1.,
		crl::time(300),
		anim::easeOutCubic);
}

void StartGiftFlight(
		CraftAnimationState *animState,
		not_null<RpWidget*> canvas,
		int startIndex) {
	const auto &corners = animState->shared->corners;

	auto nextGift = -1;
	for (auto i = startIndex; i < 4; ++i) {
		if (corners[i].hasGift) {
			nextGift = i;
			break;
		}
	}

	if (nextGift < 0) {
		return;
	}

	animState->currentlyFlying = nextGift;
	animState->nearlyStoppedSince = 0;

	animState->giftToSide[nextGift] = GetCameraFacingFreeFace(*animState);

	animState->flightAnimation.start(
		[=] { canvas->update(); },
		0.,
		1.,
		crl::time(400),
		anim::easeInCirc);

	if (!animState->continuousAnimation.animating()) {
		animState->lastRotationUpdate = 0;
		animState->continuousAnimation.start();
	}
}

void LandCurrentGift(CraftAnimationState *animState) {
	if (animState->currentlyFlying < 0) {
		return;
	}

	const auto cornerIndex = animState->currentlyFlying;

	ApplyRotationImpulse(*animState, cornerIndex);

	++animState->giftsLanded;
	animState->currentlyFlying = -1;
	animState->nearlyStoppedSince = 0;
}

} // namespace

void CraftState::paint(QPainter &p, QSize size, int craftingHeight, float64 slideProgress) {
	const auto width = size.width();
	const auto getBackdrop = [&](BackdropView &backdrop) {
		const auto ratio = style::DevicePixelRatio();
		const auto gradientSize = size;
		auto &gradient = backdrop.gradient;
		if (gradient.size() != gradientSize * ratio) {
			gradient = CreateBgGradient(gradientSize, backdrop.colors);
		}
		return gradient;
	};
	auto patternOffsetY = 0.;
	if (slideProgress > 0. && containerHeight > 0 && craftingAreaCenterY > 0) {
		patternOffsetY = (containerHeight / 2. - craftingAreaCenterY) * slideProgress;
	}
	const auto paintPattern = [&](
			QPainter &q,
			PatternView &pattern,
			const BackdropView &backdrop,
			float64 shown) {
		const auto color = backdrop.colors.patternColor;
		const auto key = (color.red() << 16)
			| (color.green() << 8)
			| color.blue();
		if (patternOffsetY != 0.) {
			q.translate(0, patternOffsetY);
		}
		PaintBgPoints(
			q,
			PatternBgPoints(),
			pattern.emojis[key],
			pattern.emoji.get(),
			color,
			QRect(0, 0, width, st::boxTitleHeight + craftingHeight),
			shown);
		if (patternOffsetY != 0.) {
			q.translate(0, -patternOffsetY);
		}
	};
	auto animating = false;
	auto newEdgeColor = std::optional<QColor>();
	auto newButton1 = QColor();
	auto newButton2 = QColor();
	for (auto i = 0; i != 4; ++i) {
		auto &cover = covers[i];
		if (cover.shownAnimation.animating()) {
			animating = true;
		}
		const auto finalValue = cover.shown ? 1. : 0.;
		const auto shown = cover.shownAnimation.value(finalValue);
		if (shown <= 0.) {
			break;
		} else if (shown == 1.) {
			const auto next = i + 1;
			if (next < 4
				&& covers[next].shown
				&& !covers[next].shownAnimation.animating()) {
				continue;
			}
		}
		p.setOpacity(shown);
		p.drawImage(0, 0, getBackdrop(cover.backdrop));
		paintPattern(p, cover.pattern, cover.backdrop, 1.);
		if (coversAnimate) {
			const auto edge = cover.backdrop.colors.edgeColor;
			if (!newEdgeColor) {
				newEdgeColor = edge;
				newButton1 = cover.button1;
				newButton2 = cover.button2;
			} else {
				newEdgeColor = anim::color(*newEdgeColor, edge, shown);
				newButton1 = anim::color(newButton1, cover.button1, shown);
				newButton2 = anim::color(newButton2, cover.button2, shown);
			}
		}
	}
	if (newEdgeColor) {
		edgeColor = *newEdgeColor;
		button1 = newButton1;
		button2 = newButton2;
	}
	if (!animating) {
		coversAnimate = false;
	}
}

void CraftState::updateForGiftCount(int count) {
	for (auto i = 4; i != 0;) {
		auto &cover = covers[--i];
		const auto shown = (i < count);
		if (cover.shown != shown) {
			cover.shown = shown;
			const auto from = shown ? 0. : 1.;
			const auto to = shown ? 1. : 0.;
			cover.shownAnimation.start([this] {
				if (repaint) {
					repaint();
				}
			}, from, to, st::fadeWrapDuration);
			coversAnimate = true;
		}
	}
}

void StartCraftAnimation(
		not_null<VerticalLayout*> container,
		std::shared_ptr<CraftState> state) {
	while (container->count() > 0) {
		delete container->widgetAt(0);
	}

	const auto height = state->containerHeight;
	const auto craftingHeight = state->craftingBottom - state->craftingTop;

	auto canvas = object_ptr<RpWidget>(container);
	const auto raw = canvas.data();
	raw->resize(container->width(), height);

	const auto animState = raw->lifetime().make_state<CraftAnimationState>();
	animState->shared = std::move(state);
	animState->totalGifts = ranges::count(
		animState->shared->corners,
		true,
		&CraftState::CornerSnapshot::hasGift);

	raw->paintOn([=](QPainter &p) {
		const auto shared = animState->shared;
		const auto slideProgress = animState->slideOutAnimation.value(1.);
		shared->paint(p, raw->size(), craftingHeight, slideProgress);

		const auto craftingOffsetY = (shared->containerHeight / 2.)
			- shared->craftingAreaCenterY;
		const auto cubeSize = float64(shared->forgeRect.width());
		const auto cubeCenter = QPointF(shared->forgeRect.topLeft())
			+ QPointF(cubeSize, cubeSize) / 2.
			+ QPointF(0, craftingOffsetY);

		if (slideProgress < 1.) {
			PaintSlideOutPhase(p, shared, raw->size(), slideProgress);
		} else {
			for (auto i = 0; i < 4; ++i) {
				const auto &corner = shared->corners[i];
				if (corner.hasGift && animState->giftToSide[i].face < 0) {
					const auto giftTopLeft = QPointF(corner.originalRect.topLeft())
						+ QPointF(0, craftingOffsetY);
					p.drawImage(giftTopLeft, corner.gift);
				}
			}

			PaintCube(p, *animState, cubeCenter, cubeSize);

			if (animState->flightAnimation.animating()) {
				const auto flying = animState->currentlyFlying;
				if (flying >= 0 && flying < 4) {
					const auto &corner = shared->corners[flying];
					const auto progress = animState->flightAnimation.value(1.);
					const auto position = ComputeGiftFlightPosition(
						corner.originalRect,
						cubeCenter,
						cubeSize,
						progress,
						craftingOffsetY);
					PaintFlyingGift(p, corner, position);
				}
			}
		}
	});

	animState->slideOutAnimation.start(
		[=] {
			raw->update();

			const auto progress = animState->slideOutAnimation.value(1.);
			if (progress >= 1.
				&& animState->currentlyFlying < 0
				&& animState->giftsLanded == 0) {
				StartGiftFlight(animState, raw, 0);
			}
		},
		0.,
		1.,
		crl::time(300),
		anim::easeOutCubic);

	animState->continuousAnimation.init([=](crl::time now) {
		if (animState->lastRotationUpdate == 0) {
			animState->lastRotationUpdate = now;
			return true;
		}

		const auto dt = float64(now - animState->lastRotationUpdate);
		animState->lastRotationUpdate = now;

		constexpr auto kFrameDuration = 1000. / 60.;
		constexpr auto kVelocityDecay = 0.99;
		constexpr auto kDelayBeforeNextFlight = crl::time(100);

		if (animState->snapping) {
			const auto progress = animState->snapAnimation.value(1.);
			animState->rotationX = animState->snapStartX
				+ (animState->snapTargetX - animState->snapStartX) * progress;
			animState->rotationY = animState->snapStartY
				+ (animState->snapTargetY - animState->snapStartY) * progress;

			if (progress >= 1.) {
				animState->snapping = false;
				animState->rotationX = animState->snapTargetX;
				animState->rotationY = animState->snapTargetY;
			}
		} else if (animState->giftsLanded > 0
			|| animState->flightAnimation.animating()) {
			animState->rotationX += animState->velocityX * dt / 1000.;
			animState->rotationY += animState->velocityY * dt / 1000.;

			const auto decayFactor = std::pow(kVelocityDecay, dt / kFrameDuration);
			animState->velocityX *= decayFactor;
			animState->velocityY *= decayFactor;
		}

		raw->update();

		if (animState->currentlyFlying >= 0
			&& !animState->flightAnimation.animating()) {
			LandCurrentGift(animState);
		}

		if (!animState->flightAnimation.animating()
			&& animState->currentlyFlying < 0
			&& animState->giftsLanded < animState->totalGifts) {

			if (IsCubeNearlyStopped(*animState)) {
				if (animState->nearlyStoppedSince == 0) {
					animState->nearlyStoppedSince = now;
				} else if (now - animState->nearlyStoppedSince >= kDelayBeforeNextFlight) {
					auto nextIndex = 0;
					for (auto i = 0; i < 4; ++i) {
						if (animState->shared->corners[i].hasGift
							&& animState->giftToSide[i].face < 0) {
							nextIndex = i;
							break;
						}
					}
					StartGiftFlight(animState, raw, nextIndex);
				}
			} else {
				animState->nearlyStoppedSince = 0;
			}
		}

		if (!animState->snapping
			&& !animState->flightAnimation.animating()
			&& animState->currentlyFlying < 0
			&& animState->giftsLanded >= animState->totalGifts
			&& animState->totalGifts > 0) {

			if (IsCubeNearlyStopped(*animState)) {
				if (animState->nearlyStoppedSince == 0) {
					animState->nearlyStoppedSince = now;
				} else if (now - animState->nearlyStoppedSince >= kDelayBeforeNextFlight) {
					StartSnapAnimation(animState, raw);
				}
			} else {
				animState->nearlyStoppedSince = 0;
			}
		}

		const auto hasVelocity = (std::abs(animState->velocityX) > 0.001)
			|| (std::abs(animState->velocityY) > 0.001);
		const auto hasMoreFlights = (animState->giftsLanded < animState->totalGifts);
		const auto isFlying = animState->flightAnimation.animating();
		const auto isSnapping = animState->snapping;

		return hasVelocity || hasMoreFlights || isFlying || isSnapping;
	});

	container->add(std::move(canvas));
}

} // namespace Ui
