/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "settings/settings_common.h"

namespace Settings {

Type LocalPasscodeCreateId();
Type LocalPasscodeCheckId();
Type LocalPasscodeManageId();

namespace Builder {

extern SectionBuildMethod LocalPasscodeManageSection;

} // namespace Builder
} // namespace Settings
