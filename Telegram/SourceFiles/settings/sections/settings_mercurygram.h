/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "settings/settings_common.h"

namespace Settings {

namespace Builder {
class SectionBuilder;
} // namespace Builder

[[nodiscard]] Type MercurygramId();

// Wires a settings toggle row to an MG::Xxx() / MG::SetXxx() pair.
void AddBoolToggle(
	Builder::SectionBuilder &builder,
	const QString &id,
	rpl::producer<QString> title,
	QStringList keywords,
	Fn<bool()> getter,
	Fn<void(bool)> setter,
	IconDescriptor icon = {});

} // namespace Settings
