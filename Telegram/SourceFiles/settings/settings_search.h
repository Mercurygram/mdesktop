/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

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

class Search : public Section<Search> {
public:
	Search(QWidget *parent, not_null<Window::SessionController*> controller);

	[[nodiscard]] rpl::producer<QString> title() override;

	void setInnerFocus() override;

private:
	void setupContent();
	void rebuildResults(const QString &query);

	std::unique_ptr<Ui::SearchFieldController> _searchController;
	base::unique_qptr<Ui::RpWidget> _searchWrap;
	Ui::InputField *_searchField = nullptr;
	Ui::VerticalLayout *_resultsContainer = nullptr;

};

} // namespace Settings
