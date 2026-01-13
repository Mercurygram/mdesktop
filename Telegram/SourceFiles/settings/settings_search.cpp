/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "settings/settings_search.h"

#include "lang/lang_keys.h"
#include "settings/builder/settings_builder.h"
#include "settings/settings_common.h"
#include "ui/search_field_controller.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/padding_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_info.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"

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
	_searchField->setFocus();
}

void Search::setupContent() {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);

	auto searchController = std::make_unique<Ui::SearchFieldController>("");
	auto rowView = searchController->createRowView(
		content,
		st::infoLayerMediaSearch);
	_searchField = rowView.field;

	const auto searchContainer = content->add(
		object_ptr<Ui::FixedHeightWidget>(
			content,
			st::infoLayerMediaSearch.height));
	_searchWrap = std::move(rowView.wrap);
	_searchWrap->setParent(searchContainer);
	_searchWrap->show();

	searchContainer->widthValue(
	) | rpl::on_next([wrap = _searchWrap.get()](int width) {
		wrap->resizeToWidth(width);
		wrap->moveToLeft(0, 0);
	}, searchContainer->lifetime());

	_resultsContainer = content->add(
		object_ptr<Ui::VerticalLayout>(content));

	searchController->queryChanges() | rpl::on_next([=](QString &&query) {
		rebuildResults(std::move(query));
	}, content->lifetime());

	_searchController = std::move(searchController);

	Ui::ResizeFitChild(this, content);
}

void Search::rebuildResults(const QString &query) {
	while (_resultsContainer->count() > 0) {
		delete _resultsContainer->widgetAt(0);
	}

	if (query.trimmed().isEmpty()) {
		_resultsContainer->resizeToWidth(_resultsContainer->width());
		return;
	}

	const auto entries = Builder::SearchRegistry::Instance().collectAll();
	const auto results = FilterAndSort(entries, query);

	if (results.empty()) {
		_resultsContainer->add(
			object_ptr<Ui::FlatLabel>(
				_resultsContainer,
				tr::lng_contacts_not_found(),
				st::defaultSubsectionTitle),
			st::defaultSubsectionTitlePadding);
	} else {
		const auto showOther = showOtherMethod();
		for (const auto &result : results) {
			const auto entry = result.entry;
			const auto button = AddButtonWithIcon(
				_resultsContainer,
				rpl::single(entry.title),
				st::settingsButton,
				{ &st::menuIconInfo });
			const auto targetSection = entry.section;
			const auto controlId = entry.id;
			button->addClickHandler([=] {
				controller()->setHighlightControlId(controlId);
				showOther(targetSection);
			});
		}
	}

	_resultsContainer->resizeToWidth(_resultsContainer->width());
}

} // namespace Settings
