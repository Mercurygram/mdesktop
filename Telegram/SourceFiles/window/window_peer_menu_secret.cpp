/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "window/window_peer_menu_secret.h"

#include "api/api_encrypted_chats.h"
#include "apiwrap.h"
#include "boxes/secret_chat_key_box.h"
#include "data/data_secret_chat.h"
#include "data/data_user.h"
#include "history/history.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "menu/menu_checked_action.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"

namespace Window {
namespace {

void AddSecretChatKey(
		not_null<SessionController*> controller,
		PeerData *peer,
		const PeerMenuCallback &addAction) {
	const auto chat = peer ? peer->asSecretChat() : nullptr;
	if (!chat || !chat->hasKey()) {
		return;
	}
	addAction(tr::lng_secret_chat_key_menu(tr::now), [=] {
		controller->show(Box(SecretChatKeyBox, chat));
	}, &st::menuIconLock);
}

void AddSecretChatTtl(
		not_null<SessionController*> controller,
		PeerData *peer,
		const PeerMenuCallback &addAction) {
	const auto chat = peer ? peer->asSecretChat() : nullptr;
	if (!chat || !chat->hasKey()) {
		return;
	}
	const auto seconds = chat->ttl();
	const auto label = [](int seconds) {
		return seconds
			? Api::SecretChatTtlDuration(seconds)
			: tr::lng_secret_chat_ttl_off(tr::now);
	};
	// Secret-chat self-destruct presets (mobile-compatible short durations).
	const auto presets = std::vector<int>{
		0, 5, 30, 60, 60 * 60, 86400, 7 * 86400 };
	addAction(PeerMenuCallback::Args{
		.text = tr::lng_secret_chat_ttl_menu(tr::now),
		.handler = nullptr,
		.icon = &st::menuIconTTL,
		.fillSubmenu = [=](not_null<Ui::PopupMenu*> menu) {
			for (const auto value : presets) {
				Menu::AddCheckedAction(menu, label(value), [=] {
					chat->session().api().encryptedChats()
						.setSelfDestructTimer(chat, value);
				}, nullptr, (value == seconds));
			}
		},
	});
}

void AddReportSecretChatSpam(
		not_null<SessionController*> controller,
		PeerData *peer,
		const PeerMenuCallback &addAction) {
	const auto chat = peer ? peer->asSecretChat() : nullptr;
	if (!chat) {
		return;
	}
	addAction(tr::lng_report_spam(tr::now), [=] {
		controller->show(Ui::MakeConfirmBox({
			.text = tr::lng_secret_chat_report_sure(),
			.confirmed = [=](Fn<void()> &&close) {
				close();
				controller->showToast(tr::lng_report_spam_done(tr::now));
				// Defer: reportSpam() discards (clears history) and
				// showBackFromStack() tears down the open section -- must not
				// run inside the box callback (destroys widgets mid-dispatch
				// -> use-after-free).
				crl::on_main(&chat->session(), [=] {
					chat->session().api().encryptedChats().reportSpam(chat);
					const auto active
						= controller->activeChatCurrent().history();
					if (active && active->peer == chat) {
						controller->showBackFromStack();
					}
				});
			},
			.confirmText = tr::lng_report_spam_ok(),
			.confirmStyle = &st::attentionBoxButton,
		}));
	}, &st::menuIconReport);
}

void AddDeleteSecretChat(
		not_null<SessionController*> controller,
		PeerData *peer,
		const PeerMenuCallback &addAction) {
	const auto chat = peer ? peer->asSecretChat() : nullptr;
	if (!chat) {
		return;
	}
	addAction({
		.text = tr::lng_secret_chat_delete(tr::now),
		.handler = [=] {
			controller->show(Box([=](not_null<Ui::GenericBox*> box) {
				box->setTitle(tr::lng_secret_chat_delete());
				box->addRow(object_ptr<Ui::FlatLabel>(
					box,
					tr::lng_secret_chat_delete_sure(),
					st::boxLabel));
				box->addSkip(st::boxMediumSkip);
				// Android parity: a checkbox lets the user also wipe the
				// partner's copy. Checked -> discardEncryption(delete_history)
				// -> the server relays encryptedChatDiscarded(history_deleted)
				// and the partner deletes its local history too.
				const auto both = box->addRow(object_ptr<Ui::Checkbox>(
					box,
					tr::lng_secret_chat_delete_for_both(
						tr::now,
						lt_user,
						chat->name()),
					true,
					st::defaultBoxCheckbox));
				box->addButton(tr::lng_box_delete(), [=] {
					const auto deleteForBoth = both->checked();
					box->closeBox();
					// Defer: discard() clears the history and
					// showBackFromStack() tears down the open section, which
					// must not run inside this button's click event (it would
					// destroy widgets mid-dispatch -> use-after-free).
					crl::on_main(&chat->session(), [=] {
						chat->session().api().encryptedChats().discard(
							chat,
							deleteForBoth);
						// Discarding drops the chat from the list; if we are
						// viewing it, leave the now-dead conversation.
						const auto active
							= controller->activeChatCurrent().history();
						if (active && active->peer == chat) {
							controller->showBackFromStack();
						}
					});
				}, st::attentionBoxButton);
				box->addButton(tr::lng_cancel(), [=] {
					box->closeBox();
				});
			}));
		},
		.icon = &st::menuIconDeleteAttention,
		.isAttention = true,
	});
}

} // namespace

void AddStartSecretChatAction(
		not_null<SessionController*> controller,
		PeerData *peer,
		const PeerMenuCallback &addAction) {
	const auto user = peer ? peer->asUser() : nullptr;
	if (!user
		|| user->isSelf()
		|| user->isBot()
		|| user->isInaccessible()
		|| user->isRepliesChat()
		|| user->isVerifyCodes()) {
		return;
	}
	addAction(tr::lng_profile_start_secret_chat(tr::now), [=] {
		user->session().api().encryptedChats().create(user);
	}, &st::menuIconLock);
}

void AddSecretChatActions(
		not_null<SessionController*> controller,
		PeerData *peer,
		const PeerMenuCallback &addAction) {
	AddSecretChatKey(controller, peer, addAction);
	AddSecretChatTtl(controller, peer, addAction);
	AddReportSecretChatSpam(controller, peer, addAction);
	AddDeleteSecretChat(controller, peer, addAction);
}

} // namespace Window
