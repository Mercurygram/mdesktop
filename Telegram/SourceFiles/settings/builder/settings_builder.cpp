/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "settings/builder/settings_builder.h"

#include "settings/settings_common.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
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

SectionBuilder::SectionBuilder(BuildContext context)
: _context(std::move(context)) {
}

Ui::RpWidget *SectionBuilder::addControl(ControlArgs &&args) {
	return v::match(_context, [&](const WidgetContext &ctx) {
		if (!args.factory) {
			return static_cast<Ui::RpWidget*>(nullptr);
		}
		auto widget = args.factory(ctx.container);
		const auto raw = widget.data();
		ctx.container->add(std::move(widget), args.margin);
		return raw;
	}, [&](const SearchContext &ctx) {
		if (!args.id.isEmpty()) {
			ctx.entries->push_back({
				.id = std::move(args.id),
				.title = ResolveTitle(std::move(args.title)),
				.keywords = std::move(args.keywords),
			});
		}
		return static_cast<Ui::RpWidget*>(nullptr);
	});
}

Ui::SettingsButton *SectionBuilder::addSettingsButton(ButtonArgs &&args) {
	const auto &st = args.st ? *args.st : st::settingsButton;
	const auto button = v::match(_context, [&](const WidgetContext &ctx) -> Ui::SettingsButton* {
		if (args.label) {
			return AddButtonWithLabel(
				ctx.container,
				rpl::duplicate(args.title),
				std::move(args.label),
				st,
				std::move(args.icon));
		} else {
			return ctx.container->add(CreateButtonWithIcon(
				ctx.container,
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

} // namespace Settings::Builder
