/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/mg_lock_on_hide.h"

#include "core/application.h"
#include "core/mg_settings.h"
#include "main/main_domain.h"
#include "storage/storage_domain.h"
#include "window/window_controller.h"

#include <QtWidgets/QApplication>

namespace MG {

void MaybeLockOnHide(not_null<QWidget*> hidden) {
	if (!LockOnHide()
		|| Core::Quitting()
		|| Core::App().passcodeLocked()
		|| !Core::App().domain().local().hasLocalPasscode()) {
		return;
	}
	// The lock covers every window at once, so it must wait until none of
	// them is on screen anymore, otherwise minimizing a separate window would
	// lock the main one the user is still typing in. The window that is going
	// away is skipped explicitly, its state is not necessarily committed yet.
	const auto shown = ranges::any_of(
		QApplication::topLevelWidgets(),
		[&](not_null<QWidget*> widget) {
			return (widget != hidden)
				&& widget->isVisible()
				&& !widget->isMinimized()
				&& Core::App().findWindow(widget);
		});
	if (!shown) {
		// Not maybeLockByPasscode(): that one asks the last active window
		// whether it may be closed, which silently skips the lock while a
		// voice message is being recorded, and pops the confirmation in a
		// window that isn't even the one being hidden.
		Core::App().lockByPasscode();
	}
}

} // namespace MG
