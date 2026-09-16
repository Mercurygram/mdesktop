/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QString>

namespace MG {

// Both return their input unchanged when there is nothing to strip, and
// neither reads the "Strip tracking parameters" option: the call sites check
// it, so these stay testable without starting the application.
[[nodiscard]] QString StripTrackingFromUrl(const QString &url);
[[nodiscard]] QString StripTrackingInText(const QString &text);

} // namespace MG
