/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "boxes/star_gift_craft_box.h"

#include "base/call_delayed.h"
#include "base/random.h"
#include "boxes/star_gift_craft_animation.h"
#include "apiwrap.h"
#include "api/api_credits.h"
#include "api/api_premium.h"
#include "boxes/star_gift_box.h"
#include "core/ui_integration.h"
#include "data/stickers/data_custom_emoji.h"
#include "data/data_document.h"
#include "data/data_session.h"
#include "data/data_star_gift.h"
#include "data/data_user.h"
#include "info/peer_gifts/info_peer_gifts_common.h"
#include "lang/lang_keys.h"
#include "main/main_app_config.h"
#include "main/main_session.h"
#include "ui/boxes/about_cocoon_box.h" // AddUniqueCloseButton.
#include "ui/controls/button_labels.h"
#include "ui/controls/feature_list.h"
#include "ui/effects/numbers_animation.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/gradient_round_button.h"
#include "ui/painter.h"
#include "ui/top_background_gradient.h"
#include "ui/ui_utility.h"
#include "ui/vertical_list.h"
#include "window/window_session_controller.h"
#include "styles/style_chat_helpers.h" // stickerPanDeleteIconFg
#include "styles/style_credits.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"

namespace Ui {
namespace {

using namespace Info::PeerGifts;

struct ColorScheme {
	Data::UniqueGiftBackdrop backdrop;
	QColor button1;
	QColor button2;
};

[[nodiscard]] QColor ForgeBgOverlay() {
	return QColor(0xBA, 0xDF, 0xFF, 32);
}

[[nodiscard]] std::array<ColorScheme, 4> CraftBackdrops() {
	struct Colors {
		int center = 0;
		int edge = 0;
		int pattern = 0;
		int button1 = 0;
		int button2 = 0;
	};
	const auto hardcoded = [](Colors colors) {
		auto result = ColorScheme();
		const auto color = [](int value) {
			return QColor(
				(uint32(value) >> 16) & 0xFF,
				(uint32(value) >> 8) & 0xFF,
				(uint32(value)) & 0xFF);
		};
		result.backdrop.centerColor = color(colors.center);
		result.backdrop.edgeColor = color(colors.edge);
		result.backdrop.patternColor = color(colors.pattern);
		result.button1 = color(colors.button1);
		result.button2 = color(colors.button2);
		return result;
	};
	return {
		hardcoded({ 0x2C4359, 0x232E3F, 0x040C1A, 0x10A5DF, 0x2091E9 }),
		hardcoded({ 0x2C4359, 0x232E3F, 0x040C1A, 0x10A5DF, 0x2091E9 }),
		hardcoded({ 0x2C4359, 0x232E3F, 0x040C1A, 0x10A5DF, 0x2091E9 }),
		hardcoded({ 0x1C4843, 0x1A2E37, 0x040C1A, 0x3ACA49, 0x007D9E }),
	};
}

struct GiftForCraft {
	std::shared_ptr<Data::UniqueGift> unique;
	Data::SavedStarGiftId manageId;

	explicit operator bool() const {
		return unique != nullptr;
	}
	friend inline bool operator==(
		const GiftForCraft &,
		const GiftForCraft &) = default;
};

struct CraftingView {
	object_ptr<RpWidget> widget;
	rpl::producer<int> editRequests;
	rpl::producer<int> removeRequests;
	Fn<void(std::shared_ptr<CraftState>)> grabForAnimation;
};

[[nodiscard]] QString FormatPercent(int permille) {
	const auto rounded = (permille + 5) / 10;
	return QString::number(rounded) + '%';
}

[[nodiscard]] not_null<RpWidget*> MakeRadialPercent(
		not_null<RpWidget*> parent,
		const style::CraftRadialPercent &st,
		rpl::producer<int> permille) {
	auto raw = CreateChild<RpWidget>(parent);

	struct State {
		State(const style::CraftRadialPercent &st, Fn<void()> callback)
		: numbers(st.font, std::move(callback)) {
			numbers.setDisabledMonospace(true);
		}

		Animations::Simple animation;
		NumbersAnimation numbers;
		int permille = 0;
	};
	const auto state = raw->lifetime().make_state<State>(
		st,
		[=] { raw->update(); });

	std::move(permille) | rpl::on_next([=](int value) {
		if (state->permille == value) {
			return;
		}
		state->animation.start([=] {
			raw->update();
		}, state->permille, value, st::slideWrapDuration);
		state->permille = value;
		state->numbers.setText(FormatPercent(value), value);
	}, raw->lifetime());
	state->animation.stop();
	state->numbers.finishAnimating();

	raw->show();
	raw->setAttribute(Qt::WA_TransparentForMouseEvents);
	raw->paintOn([=, &st](QPainter &p) {
		static constexpr auto kArcSkip = arc::kFullLength / 4;
		static constexpr auto kArcStart = -(arc::kHalfLength - kArcSkip) / 2;
		static constexpr auto kArcLength = arc::kFullLength - kArcSkip;

		const auto paint = [&](QColor color, float64 permille) {
			p.setPen(QPen(color, st.stroke, Qt::SolidLine, Qt::RoundCap));
			p.setBrush(Qt::NoBrush);
			const auto part = kArcLength * (permille / 1000.);
			const auto length = int(base::SafeRound(part));
			const auto inner = raw->rect().marginsRemoved(
				{ st.stroke, st.stroke, st.stroke, st.stroke });
			p.drawArc(inner, kArcStart + kArcLength - length, length);
		};

		auto hq = PainterHighQualityEnabler(p);

		auto inactive = QColor(255, 255, 255, 64);
		paint(inactive, 1000.);
		paint(st::white->c, state->animation.value(state->permille));

		state->numbers.paint(
			p,
			(raw->width() - state->numbers.countWidth()) / 2,
			raw->height() - st.font->height,
			raw->width());
	});

	return raw;
}

AbstractButton *MakeCornerButton(
		not_null<RpWidget*> parent,
		not_null<GiftButton*> button,
		object_ptr<RpWidget> content,
		style::align align,
		const GiftForCraft &gift,
		rpl::producer<QColor> edgeColor) {
	Expects(content != nullptr);

	const auto result = CreateChild<AbstractButton>(parent);
	result->show();

	const auto inner = content.release();
	inner->setParent(result);
	inner->show();
	inner->sizeValue() | rpl::on_next([=](QSize size) {
		result->resize(size);
	}, result->lifetime());
	inner->move(0, 0);

	rpl::combine(
		button->geometryValue(),
		result->sizeValue()
	) | rpl::on_next([=](QRect geometry, QSize size) {
		const auto extend = st::defaultDropdownMenu.wrap.shadow.extend;
		geometry = geometry.marginsRemoved(extend);
		const auto out = QPoint(size.width(), size.height()) / 3;
		const auto left = (align == style::al_left)
			? (geometry.x() - out.x())
			: (geometry.x() + geometry.width() - size.width() + out.x());
		const auto top = geometry.y() - out.y();
		result->move(left, top);
	}, result->lifetime());

	struct State {
		rpl::variable<QColor> edgeColor;
		QColor buttonEdgeColor;
	};
	const auto state = result->lifetime().make_state<State>();
	state->edgeColor = std::move(edgeColor);
	state->buttonEdgeColor = gift.unique->backdrop.edgeColor;
	result->paintOn([=](QPainter &p) {
		const auto right = result->width();
		const auto bottom = result->height();
		const auto add = QPoint(right, bottom) / 3;
		const auto radius = bottom / 2.;
		auto gradient = QLinearGradient(
			(align == style::al_left) ? -add.x() : (right + add.x()),
			-add.y(),
			(align == style::al_left) ? (right + add.x()) : -add.x(),
			bottom + add.y());
		gradient.setColorAt(0, state->edgeColor.current());
		gradient.setColorAt(1, state->buttonEdgeColor);
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(gradient);
		p.drawRoundedRect(result->rect(), radius, radius);
	});

	return result;
}

AbstractButton *MakePercentButton(
		not_null<RpWidget*> parent,
		not_null<GiftButton*> button,
		const GiftForCraft &gift,
		rpl::producer<QColor> edgeColor) {
	auto label = object_ptr<FlatLabel>(
		parent,
		FormatPercent(gift.unique->craftChancePermille),
		st::craftPercentLabel);
	label->setTextColorOverride(st::white->c);
	const auto result = MakeCornerButton(
		parent,
		button,
		std::move(label),
		style::al_left,
		gift,
		std::move(edgeColor));
	result->setAttribute(Qt::WA_TransparentForMouseEvents);
	return result;
}

AbstractButton *MakeRemoveButton(
		not_null<RpWidget*> parent,
		not_null<GiftButton*> button,
		int size,
		const GiftForCraft &gift,
		Fn<void()> onClick,
		rpl::producer<QColor> edgeColor) {
	auto remove = object_ptr<RpWidget>(parent);
	const auto &icon = st::stickerPanDeleteIconFg;
	const auto add = (size - icon.width()) / 2;
	remove->resize(icon.size() + QSize(add, add) * 2);
	remove->paintOn([=](QPainter &p) {
		const auto &icon = st::stickerPanDeleteIconFg;
		icon.paint(p, add, add, add * 2 + icon.width(), st::white->c);
	});
	remove->setAttribute(Qt::WA_TransparentForMouseEvents);
	const auto result = MakeCornerButton(
		parent,
		button,
		std::move(remove),
		style::al_right,
		gift,
		std::move(edgeColor));
	result->setClickedCallback(std::move(onClick));
	return result;
}

[[nodiscard]] CraftingView MakeCraftingView(
		not_null<RpWidget*> parent,
		not_null<Main::Session*> session,
		rpl::producer<std::vector<GiftForCraft>> chosen,
		rpl::producer<QColor> edgeColor) {
	const auto width = st::boxWidth;

	const auto buttonPadding = st::craftPreviewPadding;
	const auto buttonSize = st::giftBoxGiftTiny;
	const auto height = 2
		* (buttonPadding.top() + buttonSize + buttonPadding.bottom());

	auto widget = object_ptr<FixedHeightWidget>(parent, height);
	const auto raw = widget.data();

	struct Entry {
		GiftForCraft gift;
		GiftButton *button = nullptr;
		AbstractButton *add = nullptr;
		AbstractButton *percent = nullptr;
		AbstractButton *remove = nullptr;
	};
	struct State {
		explicit State(not_null<Main::Session*> session)
		: delegate(session, GiftButtonMode::CraftPreview) {
		}

		Delegate delegate;
		std::array<Entry, 4> entries;
		Fn<void(int)> refreshButton;
		rpl::event_stream<int> editRequests;
		rpl::event_stream<int> removeRequests;
		rpl::variable<int> chancePermille;
		rpl::variable<QColor> edgeColor;
		RpWidget *forgeRadial = nullptr;
	};
	const auto state = parent->lifetime().make_state<State>(session);
	state->edgeColor = std::move(edgeColor);

	state->refreshButton = [=](int index) {
		Expects(index >= 0 && index < state->entries.size());

		auto &entry = state->entries[index];
		const auto single = state->delegate.buttonSize();
		const auto geometry = QRect(
			((index % 2)
				? (width - buttonPadding.left() - single.width())
				: buttonPadding.left()),
			((index < 2)
				? buttonPadding.top()
				: (height - buttonPadding.top() - single.height())),
			single.width(),
			single.height());
		delete base::take(entry.add);
		delete base::take(entry.button);
		delete base::take(entry.percent);
		delete base::take(entry.remove);

		if (entry.gift) {
			entry.button = CreateChild<GiftButton>(raw, &state->delegate);
			entry.button->setDescriptor(GiftTypeStars{
				.info = {
					.id = entry.gift.unique->initialGiftId,
					.unique = entry.gift.unique,
					.document = entry.gift.unique->model.document,
				},
			}, GiftButton::Mode::CraftPreview);
			entry.button->show();
			if (index > 0) {
				entry.button->setClickedCallback([=] {
					state->editRequests.fire_copy(index);
				});
			} else {
				entry.button->setAttribute(Qt::WA_TransparentForMouseEvents);
			}
			entry.button->setGeometry(
				geometry,
				state->delegate.buttonExtend());

			entry.percent = MakePercentButton(
				raw,
				entry.button,
				entry.gift,
				state->edgeColor.value());
		} else {
			entry.add = CreateChild<AbstractButton>(raw);
			entry.add->show();
			entry.add->paintOn([=](QPainter &p) {
				auto hq = PainterHighQualityEnabler(p);
				const auto radius = st::boxRadius;
				p.setPen(Qt::NoPen);
				p.setBrush(ForgeBgOverlay());

				const auto rect = QRect(QPoint(), geometry.size());
				p.drawRoundedRect(rect, radius, radius);

				const auto &icon = st::craftAddIcon;
				icon.paintInCenter(p, rect, st::white->c);
			});
			entry.add->setClickedCallback([=] {
				state->editRequests.fire_copy(index);
			});
			entry.add->setGeometry(geometry);
		}

		const auto count = 4 - ranges::count(
			state->entries,
			nullptr,
			&Entry::button);
		const auto canRemove = (count > 1);
		for (auto i = 0; i != 4; ++i) {
			auto &entry = state->entries[i];
			if (entry.button) {
				if (!canRemove) {
					delete base::take(entry.remove);
				} else if (!entry.remove) {
					entry.remove = MakeRemoveButton(
						raw,
						entry.button,
						entry.percent->height(),
						entry.gift,
						[=] { state->removeRequests.fire_copy(i); },
						state->edgeColor.value());
				}
			}
		}
	};

	std::move(
		chosen
	) | rpl::on_next([=](const std::vector<GiftForCraft> &gifts) {
		auto chance = 0;
		for (auto i = 0; i != 4; ++i) {
			auto &entry = state->entries[i];
			const auto gift = (i < gifts.size()) ? gifts[i] : GiftForCraft();
			chance += gift.unique ? gift.unique->craftChancePermille : 0;
			if (entry.gift == gift && (entry.button || entry.add)) {
				continue;
			}
			entry.gift = gift;
			state->refreshButton(i);
		}
		state->chancePermille = chance;
	}, raw->lifetime());

	const auto center = [&] {
		const auto buttonPadding = st::craftPreviewPadding;
		const auto buttonSize = st::giftBoxGiftTiny;
		const auto left = buttonPadding.left()
			+ buttonSize
			+ buttonPadding.right();
		const auto center = (width - 2 * left);
		const auto top = (height - center) / 2;
		return QRect(left, top, center, center);
	}();
	raw->paintOn([=](QPainter &p) {
		auto hq = PainterHighQualityEnabler(p);

		const auto radius = st::boxRadius;

		p.setPen(Qt::NoPen);
		p.setBrush(ForgeBgOverlay());

		p.drawRoundedRect(center, radius, radius);

		st::craftForge.paintInCenter(p, center, st::white->c);
	});

	state->forgeRadial = MakeRadialPercent(
		raw,
		st::craftForgePercent,
		state->chancePermille.value());
	state->forgeRadial->setGeometry(center.marginsRemoved({
		st::craftForgePadding,
		st::craftForgePadding,
		st::craftForgePadding,
		st::craftForgePadding,
	}));

	auto grabForAnimation = [=](std::shared_ptr<CraftState> craftState) {
		craftState->forgeRect = center;
		craftState->forgePercent = GrabWidgetToImage(state->forgeRadial);

		auto giftsTotal = 0;
		for (auto i = 0; i != 4; ++i) {
			auto &entry = state->entries[i];
			auto &corner = craftState->corners[i];

			if (entry.button) {
				corner.originalRect = entry.button->geometry();
				if (entry.percent) {
					corner.percentBadge = GrabWidgetToImage(entry.percent);
				}
				if (entry.remove) {
					corner.removeButton = GrabWidgetToImage(entry.remove);
				}
				corner.giftButton.reset(entry.button);
				entry.button->setParent(parent);
				base::take(entry.button)->hide();

				++giftsTotal;
			} else if (entry.add) {
				corner.addButton = GrabWidgetToImage(entry.add);
				corner.originalRect = entry.add->geometry();
			}
		}

		const auto overlayBg = craftState->forgeBgOverlay = ForgeBgOverlay();
		const auto backdrop = CraftBackdrops()[giftsTotal - 1].backdrop;
		craftState->forgeBg1 = anim::color(
			backdrop.centerColor,
			QColor(overlayBg.red(), overlayBg.green(), overlayBg.blue()),
			overlayBg.alphaF());
		craftState->forgeBg2 = anim::color(
			backdrop.edgeColor,
			QColor(overlayBg.red(), overlayBg.green(), overlayBg.blue()),
			overlayBg.alphaF());
		for (auto i = 0; i != 6; ++i) {
			craftState->forgeSides[i] = craftState->prepareEmptySide(i);
		}
	};

	return {
		.widget = std::move(widget),
		.editRequests = state->editRequests.events(),
		.removeRequests = state->removeRequests.events(),
		.grabForAnimation = std::move(grabForAnimation),
	};
}

void ShowSelectGiftBox(
		not_null<Window::SessionController*> controller,
		std::vector<Data::SavedStarGift> list,
		Fn<void(GiftForCraft)> chosen,
		std::vector<GiftForCraft> selected) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(tr::lng_gift_craft_select_title());
		box->setWidth(st::boxWideWidth);

		struct Entry {
			Data::SavedStarGift gift;
			GiftButton *button = nullptr;
		};
		struct State {
			explicit State(not_null<Main::Session*> session)
			: delegate(session, GiftButtonMode::Minimal) {
			}

			Delegate delegate;
			std::vector<Entry> entries;
			rpl::event_stream<> changes;
			rpl::variable<int> successPercentPermille;
			rpl::variable<QString> successPercentText;

			int requestingIndex = 0;
			bool crafting = false;
		};
		const auto session = &controller->session();
		const auto state = box->lifetime().make_state<State>(session);

		AddSubsectionTitle(
			box->verticalLayout(),
			tr::lng_gift_craft_select_your());

		if (list.empty()) {
			box->addRow(
				object_ptr<FlatLabel>(
					box,
					tr::lng_gift_craft_select_none(),
					st::craftYourListEmpty),
				style::al_top
			)->setTryMakeSimilarLines(true);
		}

		const auto single = state->delegate.buttonSize();
		const auto extend = state->delegate.buttonExtend();
		const auto full = single.grownBy(extend).height();
		const auto skip = st::boxRowPadding.left() / 2;
		auto row = (RpWidget*)nullptr;
		for (auto &gift : list) {
			if (!row) {
				row = box->addRow(object_ptr<RpWidget>(box), QMargins());
				row->resize(row->width(), full);
			}
			const auto col = (state->entries.size() % 3);
			state->entries.push_back(Entry{
				.gift = gift,
				.button = CreateChild<GiftButton>(
					row,
					&state->delegate),
			});
			const auto button = state->entries.back().button;
			const auto proj = &GiftForCraft::manageId;
			if (ranges::contains(selected, gift.manageId, proj)) {
				button->toggleSelected(
					true,
					GiftSelectionMode::Inset,
					anim::type::instant);
				button->setAttribute(Qt::WA_TransparentForMouseEvents);
			} else {
				button->setClickedCallback([=] {
					chosen({ gift.info.unique, gift.manageId });
					box->closeBox();
				});
			}
			button->show();
			button->setDescriptor(GiftTypeStars{
				.info = {
					.id = gift.info.id,
					.unique = gift.info.unique,
					.document = gift.info.unique->model.document,
				},
			}, GiftButton::Mode::Minimal);
			const auto width = (st::boxWideWidth - 2 * skip - st::boxRowPadding.left() - st::boxRowPadding.right()) / 3;
			const auto left = st::boxRowPadding.left() + (width + skip) * col;
			button->setGeometry(QRect(left, extend.top(), width, single.height()), extend);
			if (col == 2) {
				row = nullptr;
			}
		}

		box->addButton(tr::lng_box_ok(), [=] {
			box->closeBox();
		});
		box->setMaxHeight(st::boxWideWidth);
	}));
}

[[nodiscard]] object_ptr<RpWidget> MakeRarityExpectancyPreview(
		not_null<RpWidget*> parent,
		not_null<Main::Session*> session,
		rpl::producer<std::vector<GiftForCraft>> gifts) {
	auto result = object_ptr<RpWidget>(parent);
	const auto raw = result.data();

	struct State {
		rpl::variable<int> backdropPermille;
		rpl::variable<int> patternPermille;

		Animations::Simple backdropAnimation;
		Data::UniqueGiftBackdrop wasBackdrop;
		Data::UniqueGiftBackdrop nowBackdrop;
		QImage wasFrame;
		QImage nowFrame;

		Animations::Simple patternAnimation;
		DocumentData *wasPattern = nullptr;
		DocumentData *nowPattern = nullptr;
		std::unique_ptr<Text::CustomEmoji> wasEmoji;
		std::unique_ptr<Text::CustomEmoji> nowEmoji;
	};

	const auto state = raw->lifetime().make_state<State>();

	const auto permilles = session->appConfig().craftAttributePermilles();
	const auto permille = [=](int count) {
		Expects(count > 0);

		return (count <= permilles.size()) ? permilles[count - 1] : 1000;
	};

	std::move(
		gifts
	) | rpl::on_next([=](const std::vector<GiftForCraft> &list) {
		struct Backdrop {
			Data::UniqueGiftBackdrop fields;
			int count = 0;
		};
		struct Pattern {
			not_null<DocumentData*> document;
			int count = 0;
		};
		auto backdrops = std::vector<Backdrop>();
		auto patterns = std::vector<Pattern>();
		for (const auto &gift : list) {
			const auto &backdrop = gift.unique->backdrop;
			const auto &pattern = gift.unique->pattern.document;
			const auto proj1 = &Backdrop::fields;
			const auto i = ranges::find(backdrops, backdrop, proj1);
			if (i != end(backdrops)) {
				++i->count;
			} else {
				backdrops.push_back({ backdrop, 1 });
			}
			const auto proj2 = &Pattern::document;
			const auto j = ranges::find(patterns, pattern, proj2);
			if (j != end(patterns)) {
				++j->count;
			} else {
				patterns.push_back({ pattern, 1 });
			}
		}
		ranges::sort(backdrops, ranges::greater(), &Backdrop::count);
		ranges::sort(patterns, ranges::greater(), &Pattern::count);

		const auto &backdrop = backdrops.front();
		state->backdropPermille = permille(backdrop.count);
		if (state->nowBackdrop != backdrop.fields) {
			if (!state->nowFrame.isNull()) {
				state->wasBackdrop = state->nowBackdrop;
				state->wasFrame = base::take(state->nowFrame);
				state->backdropAnimation.stop();
				state->backdropAnimation.start([=] {
					raw->update();
				}, 0., 1., st::fadeWrapDuration);
			}
			state->nowBackdrop = backdrop.fields;
		}

		const auto &pattern = patterns.front();
		state->patternPermille = permille(pattern.count);
		if (state->nowPattern != pattern.document) {
			if (state->nowEmoji) {
				state->wasPattern = state->nowPattern;
				state->wasEmoji = base::take(state->nowEmoji);
				state->patternAnimation.stop();
				state->patternAnimation.start([=] {
					raw->update();
				}, 0., 1., st::fadeWrapDuration);
			}
			state->nowPattern = pattern.document;
		}
	}, raw->lifetime());

	const auto single = st::craftAttributeSize;
	const auto width = 2 * single + st::craftAttributeSkip;
	MakeRadialPercent(
		raw,
		st::craftAttributePercent,
		state->backdropPermille.value()
	)->setGeometry(0, 0, single, single);
	MakeRadialPercent(
		raw,
		st::craftAttributePercent,
		state->patternPermille.value()
	)->setGeometry(width - single, 0, single, single);

	raw->setNaturalWidth(width);
	raw->resize(width, single);
	raw->paintOn([=](QPainter &p) {
		const auto sub = st::craftAttributePadding;
		const auto backdrop = QRect(0, 0, single, single).marginsRemoved(
			{ sub, sub, sub, sub });
		const auto backdropProgress = state->backdropAnimation.value(1.);
		if (backdropProgress < 1.) {
			p.drawImage(backdrop.topLeft(), state->wasFrame);
			p.setOpacity(backdropProgress);
		}
		if (state->nowFrame.isNull()) {
			const auto ratio = style::DevicePixelRatio();
			state->nowFrame = QImage(
				backdrop.size() * ratio,
				QImage::Format_ARGB32_Premultiplied);
			state->nowFrame.fill(Qt::transparent);
			state->nowFrame.setDevicePixelRatio(ratio);
			auto q = QPainter(&state->nowFrame);
			auto hq = PainterHighQualityEnabler(q);
			auto gradient = QLinearGradient(
				QPointF(backdrop.width(), 0),
				QPointF(0, backdrop.height()));
			gradient.setColorAt(0., state->nowBackdrop.centerColor);
			gradient.setColorAt(1., state->nowBackdrop.edgeColor);
			q.setPen(Qt::NoPen);
			q.setBrush(gradient);
			q.drawEllipse(0, 0, backdrop.width(), backdrop.height());

			q.setCompositionMode(QPainter::CompositionMode_Source);
			q.setBrush(Qt::transparent);
			const auto max = u"100%"_q;
			const auto &font = st::craftAttributePercent.font;
			const auto width = font->width(max);
			q.drawRoundedRect(
				(backdrop.width() - width) / 2,
				backdrop.height() + sub - font->height,
				width,
				font->height,
				font->height / 2.,
				font->height / 2.);
		}
		p.drawImage(backdrop.topLeft(), state->nowFrame);
		p.setOpacity(1);

		const auto pattern = QRect(width - single, 0, single, single);
		const auto center = pattern.center();
		const auto shift = (single - Emoji::GetCustomSizeNormal()) / 2;
		const auto position = pattern.topLeft() + QPoint(shift, shift);
		const auto patternProgress = state->patternAnimation.value(1.);
		if (patternProgress < 1.) {
			p.translate(center);
			p.save();
			p.setOpacity(1. - patternProgress);
			p.scale(1. - patternProgress, 1. - patternProgress);
			p.translate(-center);
			state->wasEmoji->paint(p, {
				.textColor = st::white->c,
				.position = position,
			});
			p.restore();
			p.scale(patternProgress, patternProgress);
			p.setOpacity(patternProgress);
			p.translate(-center);
		}
		if (!state->nowEmoji) {
			state->nowEmoji = session->data().customEmojiManager().create(
				state->nowPattern,
				[=] { raw->update(); });
		}
		state->nowEmoji->paint(p, {
			.textColor = st::white->c,
			.position = position,
		});
	});

	return result;
}

void Craft(
		not_null<VerticalLayout*> container,
		std::shared_ptr<CraftState> state,
		const std::vector<GiftForCraft> &gifts) {
	auto startRequest = [=](CraftResultCallback done) {
		constexpr auto kDelays = std::array<crl::time, 7>{
			100, 200, 300, 400, 500, 1000, 2000
		};
		const auto delay = kDelays[base::RandomIndex(kDelays.size())];
		const auto giftsCopy = gifts;

		base::call_delayed(delay, container, [=] {
			const auto shouldSucceed = (base::RandomIndex(5) != 0);
			if (shouldSucceed && !giftsCopy.empty()) {
				const auto &chosen = giftsCopy[base::RandomIndex(giftsCopy.size())];
				auto info = Data::StarGift{
					.id = chosen.unique->initialGiftId,
					.unique = chosen.unique,
					.document = chosen.unique->model.document,
				};
				auto result = std::make_shared<Data::GiftUpgradeResult>(
					Data::GiftUpgradeResult{
						.info = std::move(info),
						.manageId = chosen.manageId,
					});
				done(std::move(result));
			} else {
				done(nullptr);
			}
		});
	};
	StartCraftAnimation(container, std::move(state), std::move(startRequest));
}

void MakeCraftContent(
		not_null<GenericBox*> box,
		not_null<Window::SessionController*> controller,
		std::vector<GiftForCraft> gifts,
		bool autoStartCraft) {
	struct State {
		std::shared_ptr<CraftState> craftState;
		GradientButton *button = nullptr;

		FlatLabel *title = nullptr;
		FlatLabel *about = nullptr;
		RpWidget *attributes = nullptr;
		RpWidget *craftingView = nullptr;
		Fn<void(std::shared_ptr<CraftState>)> grabCraftingView;

		rpl::variable<std::vector<GiftForCraft>> chosen;
		rpl::variable<QString> name;
		rpl::variable<int> successPercentPermille;
		rpl::variable<QString> successPercentText;

		int requestingIndex = 0;
		bool crafting = false;
	};
	const auto session = &controller->session();
	const auto state = box->lifetime().make_state<State>();

	state->craftState = std::make_shared<CraftState>();
	state->craftState->coversAnimate = true;

	{
		auto backdrops = CraftBackdrops();
		const auto emoji = Text::IconEmoji(&st::craftPattern);
		const auto data = emoji.entities.front().data();
		for (auto i = 0; i != backdrops.size(); ++i) {
			auto &cover = state->craftState->covers[i];
			cover.backdrop.colors = backdrops[i].backdrop;
			cover.pattern.emoji = Text::TryMakeSimpleEmoji(data);
			cover.button1 = backdrops[i].button1;
			cover.button2 = backdrops[i].button2;
		}
	}

	state->chosen = std::move(gifts);
	for (auto i = 0; i != int(state->chosen.current().size()); ++i) {
		state->craftState->covers[i].shown = true;
	}

	state->name = state->chosen.value(
	) | rpl::map([=](const std::vector<GiftForCraft> &gifts) {
		return Data::UniqueGiftName(*gifts.front().unique);
	});
	state->successPercentPermille = state->chosen.value(
	) | rpl::map([=](const std::vector<GiftForCraft> &gifts) {
		auto result = 0;
		for (const auto &entry : gifts) {
			if (const auto gift = entry.unique.get()) {
				result += gift->craftChancePermille;
			}
		}
		return result;
	});
	state->successPercentText = state->successPercentPermille.value(
	) | rpl::map(FormatPercent);

	const auto raw = box->verticalLayout();

	state->craftState->repaint = [=] { raw->update(); };
	state->chosen.value(
	) | rpl::on_next([=](const std::vector<GiftForCraft> &gifts) {
		state->craftState->updateForGiftCount(int(gifts.size()));
	}, box->lifetime());

	const auto title = state->title = raw->add(
		object_ptr<Ui::FlatLabel>(
			box,
			tr::lng_gift_craft_title(),
			st::uniqueGiftTitle),
		st::craftTitleMargin,
		style::al_top);
	title->setTextColorOverride(QColor(255, 255, 255));

	auto crafting = MakeCraftingView(
		raw,
		session,
		state->chosen.value(),
		state->craftState->edgeColor.value());
	const auto craftingHeight = crafting.widget->height();
	state->craftingView = crafting.widget.data();
	state->grabCraftingView = std::move(crafting.grabForAnimation);
	raw->add(std::move(crafting.widget));
	std::move(crafting.removeRequests) | rpl::on_next([=](int index) {
		auto chosen = state->chosen.current();
		if (index < chosen.size()) {
			chosen.erase(begin(chosen) + index);
			state->chosen = std::move(chosen);
		}
	}, raw->lifetime());

	const auto initialGiftId = state->chosen.current().front().unique->initialGiftId;
	std::move(
		crafting.editRequests
	) | rpl::on_next([=](int index) {
		const auto guard = base::make_weak(raw);
		if (state->requestingIndex) {
			state->requestingIndex = index;
			return;
		}
		state->requestingIndex = index;
		session->api().request(MTPpayments_GetCraftStarGifts(
			MTP_long(initialGiftId),
			MTP_string(),
			MTP_int(30)
		)).done([=](const MTPpayments_SavedStarGifts &result) {
			if (!guard) {
				return;
			}
			const auto user = session->user();
			const auto owner = &session->data();
			const auto &data = result.data();
			owner->processUsers(data.vusers());
			owner->processChats(data.vchats());

			auto list = std::vector<Data::SavedStarGift>();
			list.reserve(data.vgifts().v.size());
			for (const auto &gift : data.vgifts().v) {
				if (auto parsed = Api::FromTL(user, gift)) {
					list.push_back(std::move(*parsed));
				}
			}

			const auto index = base::take(state->requestingIndex);
			ShowSelectGiftBox(controller, list, [=](GiftForCraft chosen) {
				auto copy = state->chosen.current();
				if (index < copy.size()) {
					copy[index] = chosen;
				} else {
					copy.push_back(chosen);
				}
				state->chosen = std::move(copy);
			}, state->chosen.current());
		}).send();
	}, raw->lifetime());

	auto fullName = state->chosen.value(
	) | rpl::map([=](const std::vector<GiftForCraft> &list) {
		const auto unique = list.front().unique.get();
		return Data::SingleCustomEmoji(
			unique->model.document
		).append(' ').append(tr::bold(Data::UniqueGiftName(*unique)));
	});
	auto aboutText = rpl::combine(
		tr::lng_gift_craft_about1(lt_gift, fullName, tr::rich),
		tr::lng_gift_craft_about2(tr::rich)
	) | rpl::map([=](TextWithEntities &&a, TextWithEntities &&b) {
		return a.append('\n').append('\n').append(b);
	});
	const auto about = state->about = raw->add(
		object_ptr<FlatLabel>(
			raw,
			std::move(aboutText),
			st::craftAbout,
			st::defaultPopupMenu,
			Core::TextContext({ .session = session })),
		st::craftAboutMargin,
		style::al_top);
	about->setTextColorOverride(st::white->c);
	about->setTryMakeSimilarLines(true);

	state->attributes = raw->add(
		MakeRarityExpectancyPreview(raw, session, state->chosen.value()),
		style::al_top);

	raw->paintOn([=](QPainter &p) {
		const auto &cs = state->craftState;
		const auto wasButton1 = cs->button1;
		const auto wasButton2 = cs->button2;
		cs->paint(p, raw->size(), craftingHeight);
		if (cs->button1 != wasButton1 || cs->button2 != wasButton2) {
			state->button->setStops({
				{ 0., cs->button1 },
				{ 1., cs->button2 },
			});
		}
	});

	const auto button = state->button = raw->add(
		object_ptr<GradientButton>(raw, QGradientStops()),
		st::giftBox.buttonPadding);
	button->setFullRadius(true);
	button->startGlareAnimation();

	const auto startCrafting = [=] {
		if (state->crafting) {
			return;
		}
		state->crafting = true;

		const auto &cs = state->craftState;

		cs->topPart = GrabWidgetToImage(state->title);
		cs->topPartRect = state->title->geometry();

		if (state->grabCraftingView) {
			state->grabCraftingView(cs);
		}

		const auto craftingPos = state->craftingView->pos();
		cs->craftingTop = craftingPos.y();
		cs->craftingBottom = craftingPos.y() + state->craftingView->height();

		for (auto &corner : cs->corners) {
			corner.originalRect.translate(craftingPos);
		}
		cs->forgeRect.translate(craftingPos);
		cs->craftingAreaCenterY = cs->forgeRect.center().y();

		const auto aboutPos = state->about->pos();
		const auto buttonBottom = state->button->pos().y()
			+ state->button->height();
		const auto bottomRect = QRect(
			0,
			aboutPos.y(),
			raw->width(),
			buttonBottom - aboutPos.y());

		const auto ratio = style::DevicePixelRatio();
		auto bottomPart = QImage(
			bottomRect.size() * ratio,
			QImage::Format_ARGB32_Premultiplied);
		bottomPart.fill(Qt::transparent);
		bottomPart.setDevicePixelRatio(ratio);

		const auto renderWidget = [&](QWidget *widget) {
			const auto pos = widget->pos() - bottomRect.topLeft();
			widget->render(&bottomPart, pos, QRegion(), QWidget::DrawChildren);
		};
		renderWidget(state->about);
		renderWidget(state->attributes);
		renderWidget(state->button);

		cs->bottomPart = std::move(bottomPart);
		cs->bottomPartY = aboutPos.y();
		cs->containerHeight = raw->height();

		Craft(raw, state->craftState, state->chosen.current());
	};
	button->setClickedCallback(startCrafting);

	SetButtonTwoLabels(
		button,
		st::giftBox.button.textTop,
		tr::lng_gift_craft_button(
			lt_gift,
			state->name.value() | rpl::map(tr::marked),
			tr::marked),
		tr::lng_gift_craft_button_chance(
			lt_percent,
			state->successPercentText.value() | rpl::map(tr::marked),
			tr::marked),
		st::resaleButtonTitle,
		st::resaleButtonSubtitle);

	raw->widthValue() | rpl::on_next([=](int width) {
		const auto padding = st::giftBox.buttonPadding;
		button->setNaturalWidth(width - padding.left() - padding.right());
		button->resize(button->naturalWidth(), st::giftBox.button.height);
	}, raw->lifetime());

	if (autoStartCraft) {
		base::call_delayed(crl::time(1000), raw, startCrafting);
	}
}

void ShowGiftCraftBoxInternal(
		not_null<Window::SessionController*> controller,
		std::vector<GiftForCraft> gifts,
		bool autoStartCraft) {
	controller->show(Box([=](not_null<GenericBox*> box) {
		box->setStyle(st::giftCraftBox);
		box->setWidth(st::boxWidth);
		box->setNoContentMargin(true);
		MakeCraftContent(box, controller, gifts, autoStartCraft);
		AddUniqueCloseButton(box);

#if _DEBUG
		if (autoStartCraft) {
			static const auto full = gifts;
			box->boxClosing() | rpl::on_next([=] {
				base::call_delayed(1000, controller, [=] {
					auto copy = gifts;
					if (copy.size() > 1) {
						copy.pop_back();
					} else {
						copy = full;
					}
					ShowGiftCraftBoxInternal(controller, copy, true);
				});
			}, box->lifetime());
		}
#endif
	}));
}

} // namespace

void ShowGiftCraftInfoBox(
		not_null<Window::SessionController*> controller,
		std::shared_ptr<Data::UniqueGift> gift,
		Data::SavedStarGiftId savedId) {
	controller->show(Box([=](not_null<GenericBox*> box) {
		const auto container = box->verticalLayout();
		auto cover = tr::lng_gift_craft_info_title(
		) | rpl::map([=](const QString &title) {
			auto result = UniqueGiftCover{ *gift };
			result.values.title = title;
			return result;
		});
		AddUniqueGiftCover(container, std::move(cover), {
			.subtitle = tr::lng_gift_craft_info_about(tr::marked),
		});
		AddSkip(container);

		AddUniqueCloseButton(box);

		const auto features = std::vector<FeatureListEntry>{
			{
				st::menuIconNftWear,
				tr::lng_gift_craft_combine_title(tr::now),
				tr::lng_gift_craft_combine_about(tr::now, tr::rich),
			},
			{
				st::menuIconTradable,
				tr::lng_gift_craft_input_title(tr::now),
				tr::lng_gift_craft_input_about(tr::now, tr::marked),
			},
			{
				st::menuIconUnique,
				tr::lng_gift_craft_exclusive_title(tr::now),
				tr::lng_gift_craft_exclusive_about(tr::now, tr::marked),
			},
		};
		for (const auto &feature : features) {
			container->add(
				MakeFeatureListEntry(container, feature),
				st::boxRowPadding);
		}

		box->setStyle(st::giftBox);
		box->setWidth(st::boxWideWidth);
		box->setNoContentMargin(true);

		box->addButton(tr::lng_gift_craft_start_button(), [=] {
			ShowGiftCraftBoxInternal(
				controller,
				{ GiftForCraft{ gift, savedId } },
				false);
		});
	}));
}

void ShowTestGiftCraftBox(
		not_null<Window::SessionController*> controller,
		std::vector<GiftForCraftEntry> gifts) {
	auto converted = std::vector<GiftForCraft>();
	converted.reserve(gifts.size());
	for (auto &gift : gifts) {
		auto entry = GiftForCraft();
		entry.unique = std::move(gift.unique);
		entry.manageId = std::move(gift.manageId);
		converted.push_back(std::move(entry));
	}
	ShowGiftCraftBoxInternal(controller, std::move(converted), true);
}

} // namespace Ui
