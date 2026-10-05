/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "history/mg_message_history.h"

#include "base/unixtime.h"
#include "core/mg_settings.h"
#include "data/data_media_types.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "ui/layers/generic_box.h"
#include "ui/text/format_values.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "ui/vertical_list.h"
#include "window/window_session_controller.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"

namespace MG {
namespace {

// Per message, so a long-lived chat with a chatty editor stays bounded.
constexpr auto kMaxRevisions = 50;

struct Revision {
	TimeId date = 0;
	TextWithEntities text;
};

struct Entry {
	not_null<Data::Session*> owner;
	bool deleted = false;
	std::vector<Revision> revisions; // Oldest first.
};

// Keyed by item pointer and dropped when the item is destroyed, so it
// never outlives what it describes.
[[nodiscard]] std::map<const HistoryItem*, Entry> &Entries() {
	static auto result = std::map<const HistoryItem*, Entry>();
	return result;
}

[[nodiscard]] Entry &EntryFor(not_null<HistoryItem*> item) {
	static auto tracked = base::flat_set<not_null<Data::Session*>>();
	const auto owner = &item->history()->owner();
	if (tracked.emplace(owner).second) {
		owner->itemRemoved(
		) | rpl::on_next([](not_null<const HistoryItem*> item) {
			Entries().erase(item.get());
		}, owner->session().lifetime());
		// Items may go without itemRemoved() when the session ends, and
		// a later item could reuse an address: drop the session's entries.
		owner->session().lifetime().add([=] {
			tracked.remove(owner);
			for (auto i = begin(Entries()); i != end(Entries());) {
				i = (i->second.owner == owner) ? Entries().erase(i) : ++i;
			}
		});
	}
	return Entries().try_emplace(item.get(), Entry{ .owner = owner })
		.first->second;
}

[[nodiscard]] const Entry *FindEntry(not_null<const HistoryItem*> item) {
	const auto i = Entries().find(item.get());
	return (i != end(Entries())) ? &i->second : nullptr;
}

[[nodiscard]] bool Eligible(not_null<HistoryItem*> item) {
	if (!KeepDeletedMessages()
		|| item->isService()
		|| item->ttlDestroyAt()
		|| item->history()->peer->isSecretChat()) {
		return false;
	}
	const auto media = item->media();
	return !media || !media->ttlSeconds();
}

} // namespace

bool KeepDeletedMessage(not_null<HistoryItem*> item) {
	if (!Eligible(item)) {
		return false;
	}
	auto &entry = EntryFor(item);
	if (!entry.deleted) {
		entry.deleted = true;
		item->history()->owner().requestItemViewRefresh(item);
	}
	return true;
}

bool IsDeletedMessage(not_null<const HistoryItem*> item) {
	const auto entry = FindEntry(item);
	return entry && entry->deleted;
}

void RememberEditedText(not_null<HistoryItem*> item) {
	if (!Eligible(item)) {
		return;
	}
	const auto edited = item->Get<HistoryMessageEdited>();
	auto revision = Revision{
		.date = edited ? edited->date : item->date(),
		.text = item->originalText(),
	};
	auto &list = EntryFor(item).revisions;
	if (!list.empty() && list.back().text == revision.text) {
		return; // A media or reactions edit, the text did not change.
	}
	list.push_back(std::move(revision));
	if (list.size() > kMaxRevisions) {
		list.erase(begin(list));
	}
}

void AddEditHistoryAction(
		not_null<Ui::PopupMenu*> menu,
		not_null<HistoryItem*> item,
		not_null<Window::SessionController*> controller) {
	const auto entry = FindEntry(item);
	if (!entry || entry->revisions.empty()) {
		return;
	}
	const auto edited = item->Get<HistoryMessageEdited>();
	auto list = entry->revisions;
	list.push_back({
		.date = edited ? edited->date : item->date(),
		.text = item->originalText(),
	});
	menu->addAction(tr::lng_mg_edit_history(tr::now), [=] {
		controller->show(Box([=](not_null<Ui::GenericBox*> box) {
			box->setTitle(tr::lng_mg_edit_history());
			box->setWidth(st::boxWideWidth);
			const auto total = int(list.size()) - 1;
			for (auto i = total; i >= 0; --i) {
				const auto when = Ui::FormatDateTime(
					base::unixtime::parse(list[i].date));
				const auto title = (i == total)
					? tr::lng_mg_edit_history_current(tr::now)
					: tr::lng_mg_edit_history_revision(
						tr::now,
						lt_current,
						QString::number(i + 1),
						lt_total,
						QString::number(total));
				Ui::AddSubsectionTitle(
					box->verticalLayout(),
					rpl::single(title + u" · "_q + when));
				box->addRow(object_ptr<Ui::FlatLabel>(
					box,
					rpl::single(list[i].text),
					st::boxLabel));
			}
			box->addButton(tr::lng_close(), [=] { box->closeBox(); });
		}));
	}, &st::menuIconEdit);
}

} // namespace MG
