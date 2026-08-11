/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace MG {

// Relock the app when the last window goes away (hidden to tray, closed to
// background or minimized), so a passcode user doesn't have to press the
// padlock by hand. Opt-in, and a no-op without a local passcode.
void MaybeLockOnHide(not_null<QWidget*> hidden);

} // namespace MG
