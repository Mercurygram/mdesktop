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
#include "ui/wrap/vertical_layout.h"
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

} // namespace Settings::Builder
