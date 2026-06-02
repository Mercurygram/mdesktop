/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "history/view/controls/history_view_ttl_button.h"

#include "data/data_changes.h"
#include "data/data_peer.h"
#include "data/data_secret_chat.h"
#include "main/main_session.h"
#include "menu/menu_ttl_validator.h"
#include "ui/text/format_values.h"
#include "ui/text/text_utilities.h"
#include "ui/widgets/popup_menu.h"
#include "window/window_peer_menu_secret.h"
#include "styles/style_chat_helpers.h"
#include "styles/style_menu_icons.h"

namespace HistoryView::Controls {

TTLButton::TTLButton(
	not_null<Ui::RpWidget*> parent,
	std::shared_ptr<Ui::Show> show,
	not_null<PeerData*> peer)
: _peer(peer)
, _button(parent, st::historyMessagesTTL) {

	if (const auto secret = peer->asSecretChat()) {
		// The secret-chat timer is a per-chat E2E setting with its own
		// presets, not messages.setHistoryTTL: reuse the peer menu list.
		_button.setClickedCallback([=] {
			_menu = base::make_unique_q<Ui::PopupMenu>(
				parent,
				st::popupMenuWithIcons);
			Window::FillSecretChatTtlMenu(_menu.get(), secret);
			_menu->popup(QCursor::pos());
		});
	} else {
		const auto validator = TTLMenu::TTLValidator(std::move(show), peer);
		_button.setClickedCallback([=] {
			if (!validator.can()) {
				validator.showToast();
				return;
			}
			validator.showBox();
		});
	}

	peer->session().changes().peerFlagsValue(
		peer,
		Data::PeerUpdate::Flag::MessagesTTL
	) | rpl::on_next([=] {
		// Secret chat with the timer off: bare clock icon, no "0s".
		const auto ttl = peer->messagesTTL();
		_button.setText(ttl ? Ui::FormatTTLTiny(ttl) : QString());
	}, _button.lifetime());
}

TTLButton::~TTLButton() = default;

void TTLButton::show() {
	_button.show();
}

void TTLButton::hide() {
	_button.hide();
}

void TTLButton::setVisible(bool visible) {
	_button.setVisible(visible);
}

bool TTLButton::isVisible() const {
	return _button.isVisible();
}

void TTLButton::move(int x, int y) {
	_button.move(x, y);
}

int TTLButton::width() const {
	return _button.width();
}

} // namespace HistoryView::Controls
