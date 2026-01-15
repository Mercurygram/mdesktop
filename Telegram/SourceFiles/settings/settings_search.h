/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <any>

#include "settings/settings_common_session.h"

namespace Window {
class SessionController;
} // namespace Window

namespace Ui {
class InputField;
class RpWidget;
class SearchFieldController;
class VerticalLayout;
} // namespace Ui

namespace Settings {

struct SearchSectionState {
	QString query;
};

class Search : public Section<Search> {
public:
	Search(QWidget *parent, not_null<Window::SessionController*> controller);

	[[nodiscard]] rpl::producer<QString> title() override;

	void setInnerFocus() override;
	void setStepDataReference(std::any &data) override;
	[[nodiscard]] base::weak_qptr<Ui::RpWidget> createPinnedToTop(
		not_null<QWidget*> parent) override;

private:
	struct ResultCustomization {
		Fn<void(not_null<Ui::SettingsButton*>)> hook;
		const style::SettingsButton *st = nullptr;
	};

	void setupContent();
	void setupCustomizations();
	void rebuildResults(const QString &query);

	std::unique_ptr<Ui::SearchFieldController> _searchController;
	Ui::InputField *_searchField = nullptr;
	Ui::VerticalLayout *_resultsContainer = nullptr;
	base::flat_map<QString, ResultCustomization> _customizations;
	std::any *_stepData = nullptr;

};

} // namespace Settings
