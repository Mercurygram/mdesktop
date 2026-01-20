/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "boxes/star_gift_craft_box.h"

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
#include "main/main_session.h"
#include "ui/boxes/about_cocoon_box.h" // AddUniqueCloseButton.
#include "ui/controls/button_labels.h"
#include "ui/controls/feature_list.h"
#include "ui/effects/numbers_animation.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/buttons.h"
#include "ui/painter.h"
#include "ui/top_background_gradient.h"
#include "ui/vertical_list.h"
#include "window/window_session_controller.h"
#include "styles/style_chat_helpers.h" // stickerPanDeleteIconFg
#include "styles/style_credits.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"

namespace Ui {
namespace {

using namespace Info::PeerGifts;

[[nodiscard]] std::array<Data::UniqueGiftBackdrop, 4> CraftBackdrops() {
	struct Colors {
		int center = 0;
		int edge = 0;
		int pattern = 0;
	};
	const auto hardcoded = [](Colors colors) {
		auto result = Data::UniqueGiftBackdrop();
		const auto color = [](int value) {
			return QColor(
				(uint32(value) >> 16) & 0xFF,
				(uint32(value) >> 8) & 0xFF,
				(uint32(value)) & 0xFF);
		};
		result.centerColor = color(colors.center);
		result.edgeColor = color(colors.edge);
		result.patternColor = color(colors.pattern);
		return result;
	};
	return {
		hardcoded({ 0x1b2d39, 0x141d2e, 0x121823 }),
		hardcoded({ 0x562f12, 0x2a160d, 0x1f130e }),
		hardcoded({ 0x1f363e, 0x121929, 0x10141c }),
		hardcoded({ 0x1b4140, 0x192e37, 0x183237 }),
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
	};
	const auto state = raw->lifetime().make_state<State>(session);
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
				p.setBrush(QColor(255, 255, 255, 32));

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
		p.setBrush(QColor(255, 255, 255, 32));

		p.drawRoundedRect(center, radius, radius);

		st::craftForge.paintInCenter(p, center, st::white->c);
	});

	MakeRadialPercent(
		raw,
		st::craftForgePercent,
		state->chancePermille.value()
	)->setGeometry(center.marginsRemoved({
		st::craftForgePadding,
		st::craftForgePadding,
		st::craftForgePadding,
		st::craftForgePadding,
	}));

	return {
		.widget = std::move(widget),
		.editRequests = state->editRequests.events(),
		.removeRequests = state->removeRequests.events(),
	};
}


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

void ShowSelectGiftBox(
		not_null<Window::SessionController*> controller,
		std::vector<Data::SavedStarGift> list,
		Fn<void(GiftForCraft)> chosen) {
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
			button->setClickedCallback([=] {
				chosen({ gift.info.unique, gift.manageId });
				box->closeBox();
			});
			button->show();
			button->setDescriptor(GiftTypeStars{
				.info = {
					.id = gift.info.id,
					.unique = gift.info.unique,
					.document = gift.info.unique->model.document,
				},
			}, GiftButton::Mode::CraftPreview);
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

void Craft(
		not_null<Window::SessionController*> controller,
		std::vector<GiftForCraft> gifts) {
#if 0
	auto inputs = QVector<MTPInputSavedStarGift>();
	for (const auto &gift : gifts) {
		inputs.push_back(
			Api::InputSavedStarGiftId(gift.manageId, gift.unique));
	}
	const auto weak = base::make_weak(controller);
	const auto session = &controller->session();
	session->api().request(MTPpayments_CraftStarGift(
		MTP_vector<MTPInputSavedStarGift>(inputs)
	)).done([=](const MTPUpdates &result) {
		session->api().applyUpdates(result);
		if (const auto strong = weak.get()) {
			strong->showPeerHistory(strong->session().user()->id);
		}
	}).send();
#endif
}

void MakeCraftContent(
		not_null<GenericBox*> box,
		not_null<Window::SessionController*> controller,
		GiftForCraft gift) {
	struct BackdropView {
		Data::UniqueGiftBackdrop colors;
		QImage gradient;
	};
	struct PatternView {
		std::unique_ptr<Text::CustomEmoji> emoji;
		base::flat_map<int, base::flat_map<float64, QImage>> emojis;
	};
	struct Cover {
		BackdropView backdrop;
		PatternView pattern;
		Ui::Animations::Simple shownAnimation;
		bool shown = false;
	};
	struct State {
		explicit State(not_null<Main::Session*> session)
		: delegate(session, GiftButtonMode::CraftPreview) {
		}

		Delegate delegate;
		std::array<Cover, 4> covers;
		rpl::variable<QColor> coverEdgeColor;
		bool coversAnimate = false;

		rpl::variable<std::vector<GiftForCraft>> chosen;
		rpl::variable<QString> name;
		rpl::variable<int> successPercentPermille;
		rpl::variable<QString> successPercentText;

		int requestingIndex = 0;
		bool crafting = false;
	};
	const auto session = &controller->session();
	const auto state = box->lifetime().make_state<State>(session);
	state->coversAnimate = true;

	{
		auto backdrops = CraftBackdrops();
		const auto emoji = Text::IconEmoji(&st::craftPattern);
		const auto data = emoji.entities.front().data();
		for (auto i = 0; i != backdrops.size(); ++i) {
			state->covers[i].backdrop.colors = backdrops[i];
			state->covers[i].pattern.emoji = Text::TryMakeSimpleEmoji(data);
		}
	}

	state->chosen = std::vector{ gift };
	state->covers[0].shown = true;

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

	state->chosen.value(
	) | rpl::on_next([=](const std::vector<GiftForCraft> &gifts) {
		const auto count = int(gifts.size());
		for (auto i = 4; i != 0;) {
			auto &cover = state->covers[--i];
			const auto shown = (i < count);
			if (cover.shown != shown) {
				cover.shown = shown;
				const auto from = shown ? 0. : 1.;
				const auto to = shown ? 1. : 0.;
				cover.shownAnimation.start([=] {
					raw->update();
				}, from, to, st::fadeWrapDuration);
				state->coversAnimate = true;
			}
		}
	}, box->lifetime());

	const auto title = raw->add(
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
		state->coverEdgeColor.value());
	raw->add(std::move(crafting.widget));
	std::move(crafting.removeRequests) | rpl::on_next([=](int index) {
		auto chosen = state->chosen.current();
		if (index < chosen.size()) {
			chosen.erase(begin(chosen) + index);
			state->chosen = std::move(chosen);
		}
	}, raw->lifetime());

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
			MTP_long(gift.unique->initialGiftId),
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
			});
		}).send();
	}, raw->lifetime());

	auto fullName = state->chosen.value(
	) | rpl::map([=](const std::vector<GiftForCraft> &list) {
		const auto unique = list.front().unique.get();
		return Data::SingleCustomEmoji(
			unique->model.document
		).append(' ').append(Data::UniqueGiftName(*unique));
	});
	auto aboutText = rpl::combine(
		tr::lng_gift_craft_about1(lt_gift, fullName, tr::rich),
		tr::lng_gift_craft_about2(tr::rich)
	) | rpl::map([=](TextWithEntities &&a, TextWithEntities &&b) {
		return a.append('\n').append('\n').append(b);
	});
	const auto about = raw->add(
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

	raw->paintOn([=](QPainter &p) {
		const auto width = raw->width();
		const auto getBackdrop = [&](BackdropView &backdrop) {
			const auto ratio = style::DevicePixelRatio();
			const auto gradientSize = QSize(width, raw->height());
			auto &gradient = backdrop.gradient;
			if (gradient.size() != gradientSize * ratio) {
				gradient = CreateBgGradient(gradientSize, backdrop.colors);
			}
			return gradient;
		};
		const auto paintPattern = [&](
				QPainter &p,
				PatternView &pattern,
				const BackdropView &backdrop,
				float64 shown) {
			const auto color = backdrop.colors.patternColor;
			const auto key = (color.red() << 16)
				| (color.green() << 8)
				| color.blue();
			PaintBgPoints(
				p,
				PatternBgPoints(),
				pattern.emojis[key],
				pattern.emoji.get(),
				color,
				QRect(0, 0, width, st::uniqueGiftSubtitleTop),
				shown);
		};
		auto animating = false;
		auto edgeColor = std::optional<QColor>();
		for (auto i = 0; i != 4; ++i) {
			auto &cover = state->covers[i];
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
					&& state->covers[next].shown
					&& !state->covers[next].shownAnimation.animating()) {
					continue;
				}
			}
			p.setOpacity(shown);
			p.drawImage(0, 0, getBackdrop(cover.backdrop));
			paintPattern(p, cover.pattern, cover.backdrop, 1.);
			if (state->coversAnimate) {
				const auto edge = cover.backdrop.colors.edgeColor;
				if (!edgeColor) {
					edgeColor = edge;
				} else {
					edgeColor = anim::color(*edgeColor, edge, shown);
				}
			}
		}
		if (edgeColor) {
			state->coverEdgeColor = *edgeColor;
		}
		if (!animating) {
			state->coversAnimate = false;
		}
	});

	const auto button = raw->add(
		object_ptr<RoundButton>(
			raw,
			rpl::single(QString()),
			st::giftBox.button),
		st::giftBox.buttonPadding);
	button->setFullRadius(true);

	button->setClickedCallback([=] {
		if (state->crafting) {
			return;
		}
		state->crafting = true;
		Craft(controller, state->chosen.current());
	});

	SetButtonTwoLabels(
		button,
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
		button->setFullWidth(width - padding.left() - padding.right());
	}, raw->lifetime());
}

void ShowGiftCraftBox(
		not_null<Window::SessionController*> controller,
		std::shared_ptr<Data::UniqueGift> gift,
		Data::SavedStarGiftId savedId) {
	controller->show(Box([=](not_null<GenericBox*> box) {
		box->setStyle(st::giftCraftBox);
		box->setWidth(st::boxWidth);
		box->setNoContentMargin(true);
		MakeCraftContent(box, controller, GiftForCraft{ gift, savedId });
		AddUniqueCloseButton(box);
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
			.pretitle = tr::lng_gift_craft_info_level(
				lt_n,
				rpl::single(QString::number(2))),
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
			ShowGiftCraftBox(controller, gift, savedId);
		});
	}));
}

} // namespace Ui
