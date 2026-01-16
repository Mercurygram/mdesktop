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
#include "ui/layers/generic_box.h"
#include "ui/widgets/buttons.h"
#include "ui/painter.h"
#include "ui/top_background_gradient.h"
#include "ui/vertical_list.h"
#include "window/window_session_controller.h"
#include "styles/style_credits.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"

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

void ShowSelectGiftBox(
		not_null<Window::SessionController*> controller,
		std::vector<Data::SavedStarGift> list,
		Fn<void(std::optional<Data::SavedStarGift>)> chosen,
		bool exists) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Choose Gift"_q));
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
				chosen(gift);
				box->closeBox();
			});
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
		if (exists) {
			box->addLeftButton(rpl::single(u"Remove"_q), [=] {
				chosen(std::nullopt);
				box->closeBox();
			});
		}
		box->setMaxHeight(st::boxWideWidth);
	}));
}

[[nodiscard]] object_ptr<RpWidget> MakeCraftContent(
		not_null<GenericBox*> box,
		not_null<Window::SessionController*> controller,
		std::shared_ptr<Data::UniqueGift> gift,
		Data::SavedStarGiftId savedId) {
	auto result = object_ptr<RpWidget>(box->verticalLayout());
	const auto raw = result.data();

	const auto width = st::boxWidth;
	const auto height = st::boxWideWidth;

	struct BackdropView {
		Data::UniqueGiftBackdrop colors;
		QImage gradient;
	};
	struct PatternView {
		DocumentData *document = nullptr;
		std::unique_ptr<Text::CustomEmoji> emoji;
		base::flat_map<int, base::flat_map<float64, QImage>> emojis;
	};
	struct Entry {
		Data::SavedStarGiftId id;
		std::shared_ptr<Data::UniqueGift> gift;
		GiftButton *button = nullptr;
		AbstractButton *add = nullptr;
	};
	struct State {
		explicit State(not_null<Main::Session*> session)
		: delegate(session, GiftButtonMode::Minimal) {
		}

		Delegate delegate;
		BackdropView backdrop;
		PatternView pattern;
		std::vector<Entry> entries;
		rpl::event_stream<> changes;
		rpl::variable<int> successPercentPermille;
		rpl::variable<QString> successPercentText;

		int requestingIndex = 0;
		bool crafting = false;
	};
	const auto session = &controller->session();
	const auto state = raw->lifetime().make_state<State>(session);
	state->backdrop.colors.centerColor = QColor(42, 61, 82, 255);
	state->backdrop.colors.edgeColor = QColor(35, 45, 63, 255);
	state->entries.push_back(Entry{ savedId, gift });
	for (auto i = 0; i != 3; ++i) {
		state->entries.emplace_back();
	}

	state->successPercentPermille = state->changes.events_starting_with_copy(
		rpl::empty
	) | rpl::map([=] {
		auto result = 0;
		for (const auto &entry : state->entries) {
			if (const auto gift = entry.gift.get()) {
				result += gift->craftChancePermille;
			}
		}
		return result;
	});
	state->successPercentText = state->successPercentPermille.value(
	) | rpl::map([](int permille) {
		return QString::number(permille / 10.) + '%';
	});

	const auto editEntry = [=](int index) {
		const auto guard = base::make_weak(raw);
		if (state->requestingIndex) {
			state->requestingIndex = index;
			return;
		}
		state->requestingIndex = index;
		session->api().request(MTPpayments_GetCraftStarGifts(
			MTP_long(gift->initialGiftId),
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
			ShowSelectGiftBox(controller, list, [=](std::optional<Data::SavedStarGift> chosen) {
				if (chosen) {
					state->entries[index].gift = chosen->info.unique;
					state->entries[index].id = chosen->manageId;
				} else {
					state->entries[index].gift = nullptr;
					state->entries[index].id = Data::SavedStarGiftId();
				}
				state->changes.fire({});
			}, (state->entries[index].gift != nullptr));
		}).send();
	};
	const auto refreshButton = [=](int index) {
		Expects(index >= 0 && index < state->entries.size());

		auto &entry = state->entries[index];
		const auto single = state->delegate.buttonSize();
		const auto skip = st::boxTitleClose.width;
		const auto geometry = QRect(
			(index % 2) ? (width - skip - single.width()) : skip,
			(index < 2) ? skip : (height - st::giftBox.buttonPadding.bottom() - st::giftBox.buttonHeight - st::giftBox.buttonPadding.top() - single.height()),
			single.width(),
			single.height());
		delete base::take(entry.add);
		delete base::take(entry.button);
		if (entry.gift) {
			entry.button = CreateChild<GiftButton>(raw, &state->delegate);
			entry.button->setDescriptor(GiftTypeStars{
				.info = {
					.id = entry.gift->initialGiftId,
					.unique = entry.gift,
					.document = entry.gift->model.document,
				},
			}, GiftButton::Mode::Minimal);
			entry.button->show();
			if (index > 0) {
				entry.button->setClickedCallback([=] {
					editEntry(index);
				});
			} else {
				entry.button->setAttribute(Qt::WA_TransparentForMouseEvents);
			}
			entry.button->setGeometry(
				geometry,
				state->delegate.buttonExtend());
		} else {
			entry.add = CreateChild<AbstractButton>(raw);
			entry.add->show();
			entry.add->paintOn([=](QPainter &p) {
				auto hq = PainterHighQualityEnabler(p);
				const auto radius = st::boxRadius;
				p.setPen(Qt::NoPen);
				p.setBrush(QColor(57, 77, 99));
				p.drawRoundedRect(
					QRect(QPoint(), geometry.size()),
					radius,
					radius);
				const auto addSize = geometry.width() / 3;
				p.setBrush(QColor(255, 255, 255));
				p.drawEllipse(addSize, (geometry.height() - addSize) / 2, addSize, addSize);

				const auto plusSize = geometry.width() / 6;
				p.setPen(QPen(QBrush(QColor(57, 77, 99)), plusSize / 5));
				p.setBrush(Qt::NoBrush);
				p.drawLine((geometry.width() - plusSize) / 2, geometry.height() / 2, (geometry.width() - plusSize) / 2 + plusSize, geometry.height() / 2);
				p.drawLine(geometry.width() / 2, (geometry.height() - plusSize) / 2, geometry.width() / 2, (geometry.height() - plusSize) / 2 + plusSize);
			});
			entry.add->setClickedCallback([=] {
				editEntry(index);
			});
			entry.add->setGeometry(geometry);
		}
	};
	state->changes.events_starting_with_copy(rpl::empty) | rpl::on_next([=] {
		for (auto i = 0; i != 4; ++i) {
			refreshButton(i);
		}
	}, raw->lifetime());

	const auto setupPattern = [=](
		PatternView &to,
		const Data::UniqueGiftPattern &pattern) {
		const auto document = pattern.document;
		const auto callback = [=] {
			if (state->pattern.document == document) {
				raw->update();
			}
		};
		to.document = document;
		to.emoji = document->owner().customEmojiManager().create(
			document,
			callback,
			Data::CustomEmojiSizeTag::Large);
		[[maybe_unused]] const auto preload = to.emoji->ready();
	};

	setupPattern(state->pattern, gift->pattern);
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
		p.drawImage(0, 0, getBackdrop(state->backdrop));
		paintPattern(p, state->pattern, state->backdrop, 1.);
	});

	raw->resize(width, height);

	const auto button = CreateChild<RoundButton>(
		raw,
		rpl::single(QString()),
		st::giftBox.button);
	button->setClickedCallback([=] {
		if (state->crafting) {
			return;
		}
		state->crafting = true;
		auto inputs = QVector<MTPInputSavedStarGift>();
		for (const auto &entry : state->entries) {
			if (entry.gift) {
				inputs.push_back(
					Api::InputSavedStarGiftId(entry.id, entry.gift));
			}
		}
		const auto weak = base::make_weak(controller);
		session->api().request(MTPpayments_CraftStarGift(
			MTP_vector<MTPInputSavedStarGift>(inputs)
		)).done([=](const MTPUpdates &result) {
			session->api().applyUpdates(result);
			if (const auto strong = weak.get()) {
				strong->showPeerHistory(strong->session().user()->id);
			}
		}).send();
	});
	raw->sizeValue() | rpl::on_next([=](QSize size) {
		const auto width = size.width();
		const auto height = size.height();
		const auto padding = st::giftBox.buttonPadding;
		button->setFullWidth(width - padding.left() - padding.right());
		button->moveToLeft(
			padding.left(),
			height - padding.bottom() - button->height());
	}, raw->lifetime());
	SetButtonTwoLabels(
		button,
		tr::lng_gift_craft_button(
			lt_gift,
			rpl::single(tr::marked(Data::UniqueGiftName(*gift))),
			tr::marked),
		tr::lng_gift_craft_button_chance(
			lt_percent,
			state->successPercentText.value() | rpl::map(tr::marked),
			tr::marked),
		st::resaleButtonTitle,
		st::resaleButtonSubtitle);

	return result;
}

void ShowGiftCraftBox(
		not_null<Window::SessionController*> controller,
		std::shared_ptr<Data::UniqueGift> gift,
		Data::SavedStarGiftId savedId) {
	controller->show(Box([=](not_null<GenericBox*> box) {
		box->setStyle(st::giftCraftBox);
		box->setWidth(st::boxWidth);
		box->setNoContentMargin(true);
		box->addRow(
			MakeCraftContent(box, controller, gift, savedId), QMargins());
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
				st::menuIconUnique,
				tr::lng_gift_craft_combine_title(tr::now),
				tr::lng_gift_craft_combine_about(tr::now, tr::rich),
			},
			{
				st::menuIconTradable,
				tr::lng_gift_craft_input_title(tr::now),
				tr::lng_gift_craft_input_about(tr::now, tr::marked),
			},
			{
				st::menuIconNftWear,
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
