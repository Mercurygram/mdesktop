/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "settings/settings_search.h"

#include "core/application.h"
#include "core/click_handler_types.h"
#include "lang/lang_keys.h"
#include "settings/settings_builder.h"
#include "settings/settings_common.h"
#include "ui/painter.h"
#include "ui/search_field_controller.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/padding_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_info.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"

namespace Settings {
namespace {

struct SearchResult {
	Builder::SearchEntry entry;
	int score = 0;
};

[[nodiscard]] int CalculateScore(
		const Builder::SearchEntry &entry,
		const QString &query) {
	const auto queryLower = query.toLower();
	const auto titleLower = entry.title.toLower();
	if (titleLower.startsWith(queryLower)) {
		return 100;
	}
	if (titleLower.contains(queryLower)) {
		return 80;
	}
	for (const auto &keyword : entry.keywords) {
		if (keyword.toLower().startsWith(queryLower)) {
			return 60;
		}
		if (keyword.toLower().contains(queryLower)) {
			return 40;
		}
	}
	if (entry.id.toLower().contains(queryLower)) {
		return 20;
	}
	return 0;
}

[[nodiscard]] std::vector<SearchResult> FilterAndSort(
		const std::vector<Builder::SearchEntry> &entries,
		const QString &query) {
	auto results = std::vector<SearchResult>();
	for (const auto &entry : entries) {
		const auto score = CalculateScore(entry, query);
		if (score > 0) {
			results.push_back({ entry, score });
		}
	}
	ranges::sort(results, [](const SearchResult &a, const SearchResult &b) {
		if (a.score != b.score) {
			return a.score > b.score;
		}
		return a.entry.title < b.entry.title;
	});
	return results;
}

void SetupCheckIcon(
		not_null<Ui::SettingsButton*> button,
		Builder::SearchEntryCheckIcon checkIcon,
		const style::SettingsButton &st) {
	struct CheckWidget {
		CheckWidget(QWidget *parent, bool checked)
		: widget(parent)
		, view(st::defaultCheck, checked) {
			view.finishAnimating();
		}
		Ui::RpWidget widget;
		Ui::CheckView view;
	};
	const auto checked = (checkIcon == Builder::SearchEntryCheckIcon::Checked);
	const auto check = button->lifetime().make_state<CheckWidget>(
		button,
		checked);
	check->widget.setAttribute(Qt::WA_TransparentForMouseEvents);
	check->widget.resize(check->view.getSize());
	check->widget.show();

	button->sizeValue(
	) | rpl::on_next([=, left = st.iconLeft](QSize size) {
		check->widget.moveToLeft(
			left,
			(size.height() - check->widget.height()) / 2,
			size.width());
	}, check->widget.lifetime());

	check->widget.paintRequest(
	) | rpl::on_next([=](QRect clip) {
		auto p = QPainter(&check->widget);
		check->view.paint(p, 0, 0, check->widget.width());
		p.setOpacity(0.5);
		p.fillRect(clip, st::boxBg);
	}, check->widget.lifetime());
}

[[nodiscard]] not_null<Ui::SettingsButton*> CreateSearchResultButton(
		not_null<Ui::VerticalLayout*> container,
		const QString &title,
		const QString &subtitle,
		const style::SettingsButton &st,
		IconDescriptor &&icon,
		Builder::SearchEntryCheckIcon checkIcon) {
	const auto button = AddButtonWithIcon(
		container,
		rpl::single(title),
		st,
		std::move(icon));
	if (checkIcon != Builder::SearchEntryCheckIcon::None) {
		SetupCheckIcon(button, checkIcon, st);
	}
	const auto details = Ui::CreateChild<Ui::FlatLabel>(
		button.get(),
		subtitle,
		st::settingsSearchResultDetails);
	details->show();
	details->moveToLeft(
		st.padding.left(),
		st.padding.top() + st.height - details->height());
	details->setAttribute(Qt::WA_TransparentForMouseEvents);
	return button;
}

} // namespace

Search::Search(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent, controller) {
	setupContent();
}

rpl::producer<QString> Search::title() {
	return tr::lng_dlg_filter();
}

void Search::setInnerFocus() {
	if (_searchField) {
		_searchField->setFocus();
	}
}

base::weak_qptr<Ui::RpWidget> Search::createPinnedToTop(
		not_null<QWidget*> parent) {
	_searchController = std::make_unique<Ui::SearchFieldController>("");
	auto rowView = _searchController->createRowView(
		parent,
		st::infoLayerMediaSearch);
	_searchField = rowView.field;

	const auto searchContainer = Ui::CreateChild<Ui::FixedHeightWidget>(
		parent.get(),
		st::infoLayerMediaSearch.height);
	const auto wrap = rowView.wrap.release();
	wrap->setParent(searchContainer);
	wrap->show();

	searchContainer->widthValue(
	) | rpl::on_next([=](int width) {
		wrap->resizeToWidth(width);
		wrap->moveToLeft(0, 0);
	}, searchContainer->lifetime());

	_searchController->queryChanges() | rpl::on_next([=](QString &&query) {
		rebuildResults(std::move(query));
	}, searchContainer->lifetime());

	return base::make_weak(not_null<Ui::RpWidget*>{ searchContainer });
}

void Search::setupContent() {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);

	_resultsContainer = content->add(
		object_ptr<Ui::VerticalLayout>(content));

	setupCustomizations();
	rebuildResults(QString());

	Ui::ResizeFitChild(this, content);
}

void Search::setupCustomizations() {
	const auto isPaused = Window::PausedIn(
		controller(),
		Window::GifPauseReason::Layer);
	const auto add = [&](const QString &id, ResultCustomization value) {
		_customizations[id] = std::move(value);
	};

	add(u"main/credits"_q, {
		.hook = [=](not_null<Ui::SettingsButton*> b) {
			AddPremiumStar(b, true, isPaused);
		},
		.st = &st::settingsSearchResult,
	});
	add(u"main/premium"_q, {
		.hook = [=](not_null<Ui::SettingsButton*> b) {
			AddPremiumStar(b, false, isPaused);
		},
		.st = &st::settingsSearchResult,
	});
}

void Search::rebuildResults(const QString &query) {
	while (_resultsContainer->count() > 0) {
		delete _resultsContainer->widgetAt(0);
	}

	const auto entries = Builder::SearchRegistry::Instance().collectAll(
		&controller()->session());
	const auto results = query.trimmed().isEmpty()
		? ranges::views::all(entries)
			| ranges::views::transform([](const auto &e) {
				return SearchResult{ e, 0 };
			})
			| ranges::to<std::vector<SearchResult>>()
		: FilterAndSort(entries, query);

	if (results.empty()) {
		_resultsContainer->add(
			object_ptr<Ui::FlatLabel>(
				_resultsContainer,
				tr::lng_search_tab_no_results(),
				st::settingsSearchNoResults),
			st::settingsSearchNoResultsPadding);
	} else {
		const auto showOther = showOtherMethod();
		const auto &registry = Builder::SearchRegistry::Instance();

		for (const auto &result : results) {
			const auto &entry = result.entry;
			const auto subtitle = registry.sectionPath(entry.section);
			const auto hasIcon = entry.icon.icon != nullptr;
			const auto hasCheckIcon = !hasIcon
				&& (entry.checkIcon != Builder::SearchEntryCheckIcon::None);

			const auto it = _customizations.find(entry.id);
			const auto custom = (it != _customizations.end())
				? &it->second
				: nullptr;

			const auto &st = custom && custom->st
				? *custom->st
				: (hasIcon || hasCheckIcon)
				? st::settingsSearchResult
				: st::settingsSearchResultNoIcon;

			const auto button = CreateSearchResultButton(
				_resultsContainer,
				entry.title,
				subtitle,
				st,
				IconDescriptor{ entry.icon.icon },
				hasCheckIcon ? entry.checkIcon : Builder::SearchEntryCheckIcon::None);

			if (custom && custom->hook) {
				custom->hook(button);
			}

			const auto targetSection = entry.section;
			const auto controlId = entry.id;
			const auto deeplink = entry.deeplink;
			button->addClickHandler([=] {
				if (!deeplink.isEmpty()) {
					Core::App().openInternalUrl(
						deeplink,
						QVariant::fromValue(ClickHandlerContext{
							.sessionWindow = base::make_weak(controller()),
						}));
				} else {
					controller()->setHighlightControlId(controlId);
					showOther(targetSection);
				}
			});
		}
	}

	_resultsContainer->resizeToWidth(_resultsContainer->width());
}

} // namespace Settings
