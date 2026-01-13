/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "api/api_user_privacy.h"
#include "base/object_ptr.h"
#include "settings/settings_common.h"
#include "settings/settings_type.h"

#include <variant>
#include <vector>

class EditPrivacyController;

namespace Ui {
class RpWidget;
class VerticalLayout;
class SettingsButton;
class Checkbox;
} // namespace Ui

namespace Main {
class Session;
} // namespace Main

namespace Window {
class SessionController;
} // namespace Window

namespace style {
struct SettingsButton;
} // namespace style

namespace Ui {
template <typename Widget>
class SlideWrap;
} // namespace Ui

namespace Settings::Builder {

class SectionBuilder;

struct SearchEntry {
	QString id;
	QString title;
	QStringList keywords;
	Type section;

	explicit operator bool() const {
		return !id.isEmpty();
	}
};

using SearchEntriesIndexer = std::function<std::vector<SearchEntry>(
	not_null<::Main::Session*> session)>;

struct SearchIndexerEntry {
	Type sectionId;
	SearchEntriesIndexer indexer;
};

class SearchRegistry {
public:
	static SearchRegistry &Instance();

	void add(
		Type sectionId,
		Type parentSectionId,
		SearchEntriesIndexer indexer);

	[[nodiscard]] std::vector<SearchEntry> collectAll(
		not_null<::Main::Session*> session) const;

private:
	std::vector<SearchIndexerEntry> _indexers;
	base::flat_map<Type, Type> _parentSections;

};

class BuildHelper {
public:
	BuildHelper(
		Type sectionId,
		FnMut<void(SectionBuilder&)> method,
		Type parentSectionId = nullptr);

	const SectionBuildMethod build;

	[[nodiscard]] std::vector<SearchEntry> index(
		not_null<::Main::Session*> session) const;

private:
	Type _sectionId;
	Type _parentSectionId;
	mutable FnMut<void(SectionBuilder &)> _method;

};

struct WidgetContext {
	not_null<Ui::VerticalLayout*> container;
	not_null<Window::SessionController*> controller;
	Fn<void(Type)> showOther;
	Fn<bool()> isPaused;
	HighlightRegistry *highlights = nullptr;
};

struct SearchContext {
	Type sectionId;
	not_null<::Main::Session*> session;
	not_null<std::vector<SearchEntry>*> entries;
};

using BuildContext = std::variant<WidgetContext, SearchContext>;

class SectionBuilder {
public:
	explicit SectionBuilder(BuildContext context);

	void add(FnMut<void(const BuildContext &ctx)> method);

	using ToggledScopePtr = not_null<Ui::SlideWrap<Ui::VerticalLayout>*>;
	Ui::VerticalLayout *scope(
		FnMut<void()> method,
		rpl::producer<bool> shown = nullptr,
		FnMut<void(ToggledScopePtr)> hook = nullptr);

	struct WidgetToAdd {
		object_ptr<Ui::RpWidget> widget = { nullptr };
		QMargins margin;
		style::align align = style::al_left;
		HighlightArgs highlight;

		explicit operator bool() const {
			return widget != nullptr;
		}
	};
	Ui::RpWidget *add(
		FnMut<WidgetToAdd(const WidgetContext &ctx)> widget,
		FnMut<SearchEntry()> search = nullptr);

	struct ControlArgs {
		Fn<object_ptr<Ui::RpWidget>(not_null<Ui::VerticalLayout*>)> factory;
		QString id;
		rpl::producer<QString> title;
		QStringList keywords;
		style::margins margin;
		style::align align = style::al_left;
		HighlightArgs highlight;
		rpl::producer<bool> shown;
	};
	Ui::RpWidget *addControl(ControlArgs &&args);

	struct ButtonArgs {
		QString id;
		rpl::producer<QString> title;
		const style::SettingsButton *st = nullptr;
		IconDescriptor icon;
		Ui::VerticalLayout *container = nullptr;
		rpl::producer<QString> label;
		Fn<void()> onClick;
		QStringList keywords;
		HighlightArgs highlight;
		rpl::producer<bool> shown;
	};
	Ui::SettingsButton *addButton(ButtonArgs &&args);

	struct SectionArgs {
		QString id;
		rpl::producer<QString> title;
		Type targetSection;
		IconDescriptor icon;
		QStringList keywords;
	};
	Ui::SettingsButton *addSectionButton(SectionArgs &&args);

	struct PremiumButtonArgs {
		QString id;
		rpl::producer<QString> title;
		rpl::producer<QString> label;
		bool credits = false;
		Fn<void()> onClick;
		QStringList keywords;
	};
	Ui::SettingsButton *addPremiumButton(PremiumButtonArgs &&args);

	struct PrivacyButtonArgs {
		QString id;
		rpl::producer<QString> title;
		Api::UserPrivacy::Key key;
		Fn<std::unique_ptr<EditPrivacyController>()> controllerFactory;
		bool premium = false;
		QStringList keywords;
	};
	Ui::SettingsButton *addPrivacyButton(PrivacyButtonArgs &&args);

	struct ToggleArgs {
		QString id;
		rpl::producer<QString> title;
		const style::SettingsButton *st = nullptr;
		IconDescriptor icon;
		Ui::VerticalLayout *container = nullptr;
		rpl::producer<bool> toggled;
		QStringList keywords;
		HighlightArgs highlight;
	};
	Ui::SettingsButton *addToggle(ToggleArgs &&args);

	struct CheckboxArgs {
		QString id;
		rpl::producer<QString> title;
		bool checked = false;
		QStringList keywords;
	};
	Ui::Checkbox *addCheckbox(CheckboxArgs &&args);

	struct SlideCheckboxArgs {
		QString id;
		rpl::producer<QString> title;
		bool checked = false;
		rpl::producer<bool> shown;
		QStringList keywords;
	};
	Ui::SlideWrap<Ui::Checkbox> *addSlideCheckbox(SlideCheckboxArgs &&args);

	void addSubsectionTitle(rpl::producer<QString> text);
	void addDivider();
	void addDividerText(rpl::producer<QString> text);
	void addSkip();
	void addSkip(int height);

	[[nodiscard]] Ui::VerticalLayout *container() const;
	[[nodiscard]] Window::SessionController *controller() const;
	[[nodiscard]] not_null<::Main::Session*> session() const;
	[[nodiscard]] Fn<void(Type)> showOther() const;
	[[nodiscard]] HighlightRegistry *highlights() const;

private:
	void registerHighlight(
		QString id,
		QWidget *widget,
		HighlightArgs &&args);

	BuildContext _context;

};

} // namespace Settings::Builder
