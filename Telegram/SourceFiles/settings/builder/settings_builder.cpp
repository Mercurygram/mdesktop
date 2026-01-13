/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "settings/builder/settings_builder.h"

#include "boxes/edit_privacy_box.h"
#include "main/main_session.h"
#include "settings/settings_common.h"
#include "settings/settings_privacy_security.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_settings.h"

namespace Settings::Builder {
namespace {

[[nodiscard]] QString ResolveTitle(rpl::producer<QString> title) {
	auto result = QString();
	auto lifetime = rpl::lifetime();
	std::move(title).start(
		[&](QString value) { result = std::move(value); },
		[](auto&&) {},
		[] {},
		lifetime);
	return result;
}

} // namespace

SearchRegistry &SearchRegistry::Instance() {
	static SearchRegistry instance;
	return instance;
}

void SearchRegistry::add(Type sectionType, SearchEntriesProvider provider) {
	_providers.push_back({ sectionType, provider });
}

std::vector<SearchEntry> SearchRegistry::collectAll() const {
	auto result = std::vector<SearchEntry>();
	for (const auto &entry : _providers) {
		auto entries = entry.provider();
		result.insert(result.end(), entries.begin(), entries.end());
	}
	return result;
}

SearchProviderRegistrar::SearchProviderRegistrar(
		Type sectionType,
		SearchEntriesProvider provider) {
	SearchRegistry::Instance().add(sectionType, provider);
}

void BuildHelper::build(
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*> controller,
		Fn<void(Type)> showOther,
		rpl::producer<> showFinished) const {
	auto &lifetime = container->lifetime();
	const auto highlights = lifetime.make_state<HighlightRegistry>();
	const auto isPaused = Window::PausedIn(
		controller,
		Window::GifPauseReason::Layer);
	auto builder = SectionBuilder(WidgetContext{
		.container = container,
		.controller = controller,
		.showOther = std::move(showOther),
		.isPaused = isPaused,
		.highlights = highlights,
	});
	_buildFunc(builder);

	std::move(showFinished) | rpl::on_next([=] {
		for (const auto &[id, entry] : *highlights) {
			if (entry.widget) {
				controller->checkHighlightControl(
					id,
					entry.widget,
					base::duplicate(entry.args));
			}
		}
	}, lifetime);
}

std::vector<SearchEntry> BuildHelper::index() const {
	auto entries = std::vector<SearchEntry>();
	auto builder = SectionBuilder(SearchContext{
		.sectionId = _sectionId,
		.entries = &entries,
	});
	_buildFunc(builder);
	return entries;
}

SectionBuilder::SectionBuilder(BuildContext context)
: _context(std::move(context)) {
}

void SectionBuilder::add(FnMut<void(const BuildContext &ctx)> method) {
	Expects(method != nullptr);

	method(_context);
}

Ui::VerticalLayout *SectionBuilder::scope(
		FnMut<void()> method,
		rpl::producer<bool> shown,
		FnMut<void(ToggledScopePtr)> hook) {
	auto result = (Ui::VerticalLayout*)nullptr;
	v::match(_context, [&](WidgetContext &wctx) {
		const auto outer = wctx.container;
		const auto toggled = (shown || hook);
		const auto wrap = toggled
			? outer->add(
				object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
					outer,
					object_ptr<Ui::VerticalLayout>(outer)))
			: nullptr;
		const auto inner = wrap
			? wrap->entity()
			: outer->add(object_ptr<Ui::VerticalLayout>(outer));
		wctx.container = inner;
		method();
		if (shown) {
			wrap->toggleOn(std::move(shown));
			wrap->finishAnimating();
		}
		if (hook) {
			hook(wrap);
		}
		wctx.container = outer;
		result = inner;
	}, [&](const SearchContext &sctx) {
		method();
	});
	return result;
}

Ui::RpWidget *SectionBuilder::add(
		FnMut<WidgetToAdd(const WidgetContext &ctx)> widget,
		FnMut<SearchEntry()> search) {
	auto result = (Ui::RpWidget*)nullptr;
	add([&](const BuildContext &ctx) {
		v::match(ctx, [&](const WidgetContext &wctx) {
			if (auto w = widget ? widget(wctx) : WidgetToAdd()) {
				result = w.widget.data();
				wctx.container->add(std::move(w.widget), w.margin, w.align);

				if (auto entry = search ? search() : SearchEntry()) {
					registerHighlight(
						entry.id,
						result,
						std::move(w.highlight));
				}
			}
		}, [&](const SearchContext &sctx) {
			if (auto entry = search ? search() : SearchEntry()) {
				entry.section = sctx.sectionId;
				sctx.entries->push_back(std::move(entry));
			}
		});
	});
	return result;
}

Ui::RpWidget *SectionBuilder::addControl(ControlArgs &&args) {
	return add([&](const WidgetContext &ctx) {
		return WidgetToAdd{
			.widget = args.factory ? args.factory(ctx.container) : nullptr,
			.margin = args.margin,
			.align = args.align,
			.highlight = std::move(args.highlight),
		};
	}, [&] {
		return SearchEntry{
			.id = std::move(args.id),
			.title = ResolveTitle(std::move(args.title)),
			.keywords = std::move(args.keywords),
		};
	});
}

Ui::SettingsButton *SectionBuilder::addSettingsButton(ButtonArgs &&args) {




	const auto &st = args.st ? *args.st : st::settingsButton;
	auto highlight = std::move(args.highlight);
	const auto id = args.id;
	const auto button = v::match(_context, [&](const WidgetContext &ctx) -> Ui::SettingsButton* {
		const auto target = args.container ? args.container : ctx.container.get();
		if (args.label) {
			return AddButtonWithLabel(
				target,
				rpl::duplicate(args.title),
				std::move(args.label),
				st,
				std::move(args.icon));
		} else {
			return target->add(CreateButtonWithIcon(
				target,
				rpl::duplicate(args.title),
				st,
				std::move(args.icon)));
		}
	}, [&](const SearchContext &ctx) -> Ui::SettingsButton* {
		if (!args.id.isEmpty()) {
			ctx.entries->push_back({
				.id = std::move(args.id),
				.title = ResolveTitle(std::move(args.title)),
				.keywords = std::move(args.keywords),
			});
		}
		return nullptr;
	});
	if (button && args.onClick) {
		button->addClickHandler(std::move(args.onClick));
	}
	if (button && !id.isEmpty()) {
		registerHighlight(id, button, std::move(highlight));
	}
	return button;
}

Ui::SettingsButton *SectionBuilder::addLabeledButton(ButtonArgs &&args) {
	return addSettingsButton(std::move(args));
}

Ui::SettingsButton *SectionBuilder::addSectionButton(SectionArgs &&args) {
	const auto button = addSettingsButton({
		.id = std::move(args.id),
		.title = std::move(args.title),
		.icon = std::move(args.icon),
		.keywords = std::move(args.keywords),
	});
	if (button) {
		const auto showOther = std::get<WidgetContext>(_context).showOther;
		const auto target = args.targetSection;
		button->addClickHandler([=] {
			showOther(target);
		});
	}
	return button;
}

void SectionBuilder::addDivider() {
	v::match(_context, [&](const WidgetContext &ctx) {
		Ui::AddDivider(ctx.container);
	}, [](const SearchContext &) {
	});
}

void SectionBuilder::addSkip() {
	v::match(_context, [&](const WidgetContext &ctx) {
		Ui::AddSkip(ctx.container);
	}, [](const SearchContext &) {
	});
}

void SectionBuilder::addSkip(int height) {
	v::match(_context, [&](const WidgetContext &ctx) {
		Ui::AddSkip(ctx.container, height);
	}, [](const SearchContext &) {
	});
}

void SectionBuilder::addDividerText(rpl::producer<QString> text) {
	v::match(_context, [&](const WidgetContext &ctx) {
		Ui::AddDividerText(ctx.container, std::move(text));
	}, [](const SearchContext &) {
	});
}

Ui::SlideWrap<Ui::SettingsButton> *SectionBuilder::addSlideButton(
		SlideButtonArgs &&args) {
	return v::match(_context, [&](const WidgetContext &ctx)
			-> Ui::SlideWrap<Ui::SettingsButton>* {
		const auto &st = args.st ? *args.st : st::settingsButton;
		const auto wrap = ctx.container->add(
			object_ptr<Ui::SlideWrap<Ui::SettingsButton>>(
				ctx.container,
				CreateButtonWithIcon(
					ctx.container,
					std::move(args.title),
					st,
					std::move(args.icon))));
		if (args.shown) {
			wrap->toggleOn(std::move(args.shown));
		}
		const auto button = wrap->entity();
		if (args.onClick) {
			button->addClickHandler(std::move(args.onClick));
		}
		return wrap;
	}, [&](const SearchContext &ctx) -> Ui::SlideWrap<Ui::SettingsButton>* {
		if (!args.id.isEmpty()) {
			ctx.entries->push_back({
				.id = std::move(args.id),
				.title = ResolveTitle(std::move(args.title)),
				.keywords = std::move(args.keywords),
			});
		}
		return nullptr;
	});
}

Ui::SlideWrap<Ui::SettingsButton> *SectionBuilder::addSlideLabeledButton(
		SlideLabeledButtonArgs &&args) {
	return v::match(_context, [&](const WidgetContext &ctx)
			-> Ui::SlideWrap<Ui::SettingsButton>* {
		const auto &st = args.st ? *args.st : st::settingsButton;
		auto button = object_ptr<Ui::SettingsButton>(
			ctx.container,
			rpl::duplicate(args.title),
			st);
		const auto raw = button.data();
		if (args.icon) {
			AddButtonIcon(raw, st, std::move(args.icon));
		}
		if (args.label) {
			CreateRightLabel(
				raw,
				std::move(args.label),
				st,
				rpl::duplicate(args.title));
		}
		const auto wrap = ctx.container->add(
			object_ptr<Ui::SlideWrap<Ui::SettingsButton>>(
				ctx.container,
				std::move(button)));
		if (args.shown) {
			wrap->toggleOn(std::move(args.shown));
		}
		if (args.onClick) {
			raw->addClickHandler(std::move(args.onClick));
		}
		return wrap;
	}, [&](const SearchContext &ctx) -> Ui::SlideWrap<Ui::SettingsButton>* {
		if (!args.id.isEmpty()) {
			ctx.entries->push_back({
				.id = std::move(args.id),
				.title = ResolveTitle(std::move(args.title)),
				.keywords = std::move(args.keywords),
			});
		}
		return nullptr;
	});
}

Ui::SettingsButton *SectionBuilder::addPremiumButton(PremiumButtonArgs &&args) {
	return v::match(_context, [&](const WidgetContext &ctx)
			-> Ui::SettingsButton* {
		Ui::SettingsButton *button = nullptr;
		if (args.label) {
			button = AddButtonWithLabel(
				ctx.container,
				rpl::duplicate(args.title),
				std::move(args.label),
				st::settingsButton);
		} else {
			button = AddButtonWithIcon(
				ctx.container,
				rpl::duplicate(args.title),
				st::settingsButton);
		}
		[[maybe_unused]] const auto decorated = AddPremiumStar(
			button,
			args.credits,
			ctx.isPaused);
		if (args.onClick) {
			button->addClickHandler(std::move(args.onClick));
		}
		return button;
	}, [&](const SearchContext &ctx) -> Ui::SettingsButton* {
		if (!args.id.isEmpty()) {
			ctx.entries->push_back({
				.id = std::move(args.id),
				.title = ResolveTitle(std::move(args.title)),
				.keywords = std::move(args.keywords),
			});
		}
		return nullptr;
	});
}

Ui::SettingsButton *SectionBuilder::addPrivacyButton(PrivacyButtonArgs &&args) {
	const auto id = args.id;
	const auto premium = args.premium;
	auto title = std::move(args.title);
	return v::match(_context, [&](const WidgetContext &ctx)
			-> Ui::SettingsButton* {
		const auto button = AddPrivacyButton(
			ctx.controller,
			ctx.container,
			rpl::duplicate(title),
			{},
			args.key,
			std::move(args.controllerFactory));
		if (premium) {
			AddPrivacyPremiumStar(
				button,
				&ctx.controller->session(),
				rpl::duplicate(title),
				st::settingsButtonNoIcon.padding);
		}
		if (!id.isEmpty()) {
			registerHighlight(id, button, {});
		}
		return button;
	}, [&](const SearchContext &ctx) -> Ui::SettingsButton* {
		if (!args.id.isEmpty()) {
			ctx.entries->push_back({
				.id = std::move(args.id),
				.title = ResolveTitle(std::move(title)),
				.keywords = std::move(args.keywords),
			});
		}
		return nullptr;
	});
}

Ui::SettingsButton *SectionBuilder::addToggle(ToggleArgs &&args) {
	auto highlight = std::move(args.highlight);
	const auto id = args.id;
	const auto button = v::match(_context, [&](const WidgetContext &ctx)
			-> Ui::SettingsButton* {
		const auto &st = args.st ? *args.st : st::settingsButton;
		const auto target = args.container ? args.container : ctx.container.get();
		const auto button = target->add(CreateButtonWithIcon(
			target,
			rpl::duplicate(args.title),
			st,
			std::move(args.icon)));
		button->toggleOn(std::move(args.toggled));
		return button;
	}, [&](const SearchContext &ctx) -> Ui::SettingsButton* {
		if (!args.id.isEmpty()) {
			ctx.entries->push_back({
				.id = std::move(args.id),
				.title = ResolveTitle(std::move(args.title)),
				.keywords = std::move(args.keywords),
			});
		}
		return nullptr;
	});
	if (button && !id.isEmpty()) {
		registerHighlight(id, button, std::move(highlight));
	}
	return button;
}

Ui::SlideWrap<Ui::SettingsButton> *SectionBuilder::addSlideToggle(
		SlideToggleArgs &&args) {
	return v::match(_context, [&](const WidgetContext &ctx)
			-> Ui::SlideWrap<Ui::SettingsButton>* {
		const auto &st = args.st ? *args.st : st::settingsButton;
		const auto wrap = ctx.container->add(
			object_ptr<Ui::SlideWrap<Ui::SettingsButton>>(
				ctx.container,
				CreateButtonWithIcon(
					ctx.container,
					rpl::duplicate(args.title),
					st,
					std::move(args.icon))));
		if (args.shown) {
			wrap->toggleOn(std::move(args.shown));
		}
		const auto button = wrap->entity();
		button->toggleOn(std::move(args.toggled));
		return wrap;
	}, [&](const SearchContext &ctx) -> Ui::SlideWrap<Ui::SettingsButton>* {
		if (!args.id.isEmpty()) {
			ctx.entries->push_back({
				.id = std::move(args.id),
				.title = ResolveTitle(std::move(args.title)),
				.keywords = std::move(args.keywords),
			});
		}
		return nullptr;
	});
}

Ui::Checkbox *SectionBuilder::addCheckbox(CheckboxArgs &&args) {
	return v::match(_context, [&](const WidgetContext &ctx) -> Ui::Checkbox* {
		const auto checkbox = ctx.container->add(
			object_ptr<Ui::Checkbox>(
				ctx.container,
				ResolveTitle(rpl::duplicate(args.title)),
				args.checked,
				st::settingsCheckbox),
			st::settingsCheckboxPadding);
		return checkbox;
	}, [&](const SearchContext &ctx) -> Ui::Checkbox* {
		if (!args.id.isEmpty()) {
			ctx.entries->push_back({
				.id = std::move(args.id),
				.title = ResolveTitle(std::move(args.title)),
				.keywords = std::move(args.keywords),
			});
		}
		return nullptr;
	});
}

Ui::SlideWrap<Ui::Checkbox> *SectionBuilder::addSlideCheckbox(
		SlideCheckboxArgs &&args) {
	return v::match(_context, [&](const WidgetContext &ctx)
			-> Ui::SlideWrap<Ui::Checkbox>* {
		const auto wrap = ctx.container->add(
			object_ptr<Ui::SlideWrap<Ui::Checkbox>>(
				ctx.container,
				object_ptr<Ui::Checkbox>(
					ctx.container,
					ResolveTitle(rpl::duplicate(args.title)),
					args.checked,
					st::settingsCheckbox),
				st::settingsCheckboxPadding));
		if (args.shown) {
			wrap->toggleOn(std::move(args.shown));
		}
		return wrap;
	}, [&](const SearchContext &ctx) -> Ui::SlideWrap<Ui::Checkbox>* {
		if (!args.id.isEmpty()) {
			ctx.entries->push_back({
				.id = std::move(args.id),
				.title = ResolveTitle(std::move(args.title)),
				.keywords = std::move(args.keywords),
			});
		}
		return nullptr;
	});
}

Ui::SlideWrap<Ui::VerticalLayout> *SectionBuilder::addSlideSection(
		SlideSectionArgs &&args) {
	return v::match(_context, [&](const WidgetContext &ctx)
			-> Ui::SlideWrap<Ui::VerticalLayout>* {
		const auto wrap = ctx.container->add(
			object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
				ctx.container,
				object_ptr<Ui::VerticalLayout>(ctx.container)));
		if (args.shown) {
			wrap->toggleOn(std::move(args.shown));
		}
		if (args.fill) {
			args.fill(wrap->entity());
		}
		return wrap;
	}, [](const SearchContext &) -> Ui::SlideWrap<Ui::VerticalLayout>* {
		return nullptr;
	});
}

void SectionBuilder::addSubsectionTitle(rpl::producer<QString> text) {
	v::match(_context, [&](const WidgetContext &ctx) {
		AddSubsectionTitle(ctx.container, std::move(text));
	}, [](const SearchContext &) {
	});
}

Ui::VerticalLayout *SectionBuilder::container() const {
	return v::match(_context, [](const WidgetContext &ctx) {
		return ctx.container.get();
	}, [](const SearchContext &) -> Ui::VerticalLayout* {
		return nullptr;
	});
}

Window::SessionController *SectionBuilder::controller() const {
	return v::match(_context, [](const WidgetContext &ctx) {
		return ctx.controller.get();
	}, [](const SearchContext &) -> Window::SessionController* {
		return nullptr;
	});
}

Fn<void(Type)> SectionBuilder::showOther() const {
	return v::match(_context, [](const WidgetContext &ctx) {
		return ctx.showOther;
	}, [](const SearchContext &) -> Fn<void(Type)> {
		return nullptr;
	});
}

HighlightRegistry *SectionBuilder::highlights() const {
	return v::match(_context, [](const WidgetContext &ctx) {
		return ctx.highlights;
	}, [](const SearchContext &) -> HighlightRegistry* {
		return nullptr;
	});
}

void SectionBuilder::registerHighlight(
		QString id,
		QWidget *widget,
		HighlightArgs &&args) {
	v::match(_context, [&](const WidgetContext &ctx) {
		if (ctx.highlights && widget) {
			ctx.highlights->push_back({
				std::move(id),
				{ widget, std::move(args) },
			});
		}
	}, [](const SearchContext &) {
	});
}

} // namespace Settings::Builder
