/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "chat_helpers/mg_field_actions.h"

#include "boxes/peer_list_controllers.h"
#include "boxes/translate_box.h"
#include "boxes/translate_box_content.h"
#include "chat_helpers/message_field.h"
#include "core/ui_integration.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "lang/lang_keys.h"
#include "lang/translate_provider.h"
#include "main/main_session.h"
#include "main/session/session_show.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/fields/input_field.h"

#include <QtWidgets/QMenu>

namespace MG {
namespace {

class MentionController final : public ContactsBoxController {
public:
	MentionController(
		not_null<Main::Session*> session,
		Fn<void(not_null<UserData*>)> chosen)
	: ContactsBoxController(session)
	, _chosen(std::move(chosen)) {
	}

	void rowClicked(not_null<PeerListRow*> row) override {
		if (const auto user = row->peer()->asUser()) {
			_chosen(user);
		}
	}

protected:
	std::unique_ptr<PeerListRow> createRow(
			not_null<UserData*> user) override {
		return user->isSelf()
			? nullptr
			: ContactsBoxController::createRow(user);
	}

private:
	const Fn<void(not_null<UserData*>)> _chosen;

};

// The menu is built from the selection, but the boxes are async: act only
// if that text is still where it was.
[[nodiscard]] bool SelectionUnchanged(
		not_null<Ui::InputField*> field,
		int from,
		int till,
		const QString &text) {
	return field->getTextWithTagsPart(from, till).text == text;
}

void ShowMentionBox(
		std::shared_ptr<Main::SessionShow> show,
		base::weak_qptr<Ui::InputField> weak,
		int from,
		int till,
		TextWithTags selected) {
	const auto box = std::make_shared<QPointer<PeerListBox>>();
	const auto chosen = [=](not_null<UserData*> user) {
		const auto field = weak.get();
		if (field && SelectionUnchanged(field, from, till, selected.text)) {
			field->commitMarkdownLinkEdit(
				{ .from = from, .till = till },
				selected,
				PrepareMentionTag(user));
		}
		if (const auto strong = box->data()) {
			strong->closeBox();
		}
	};
	show->showBox(Box<PeerListBox>(
		std::make_unique<MentionController>(&show->session(), chosen),
		[=](not_null<PeerListBox*> raw) {
			*box = raw.get();
			raw->setTitle(tr::lng_mg_mention_choose());
			raw->addButton(tr::lng_cancel(), [=] { raw->closeBox(); });
		}));
}

void ShowTranslateBox(
		std::shared_ptr<Main::SessionShow> show,
		base::weak_qptr<Ui::InputField> weak,
		not_null<PeerData*> peer,
		int from,
		int till,
		QString selected) {
	show->showBox(Box([=](not_null<Ui::GenericBox*> box) {
		struct State {
			std::unique_ptr<Ui::TranslateProvider> provider;
			rpl::variable<LanguageId> to;
			std::optional<TextWithEntities> result;
		};
		const auto session = &peer->session();
		const auto state = box->lifetime().make_state<State>();
		state->provider = Ui::CreateTranslateProvider(session);
		state->to = Ui::ChooseTranslateTo(peer->owner().history(peer));
		const auto request = std::make_shared<Ui::TranslateProviderRequest>(
			Ui::PrepareTranslateProviderRequest(
				state->provider.get(),
				peer,
				MsgId(),
				TextWithEntities{ selected }));

		Ui::TranslateBoxContent(box, {
			.text = request->text,
			.textContext = Core::TextContext({ .session = session }),
			.to = state->to.value(),
			.chooseTo = [=] {
				box->uiShow()->showBox(Ui::ChooseTranslateToBox(
					state->to.current(),
					crl::guard(box, [=](LanguageId id) {
						state->to = id;
					})));
			},
			.request = [=](
					LanguageId to,
					Fn<void(Ui::TranslateBoxContentResult)> done) {
				state->result = std::nullopt;
				state->provider->request(*request, to, [=](
						Ui::TranslateProviderResult result) {
					using ProviderError = Ui::TranslateProviderError;
					using UiError = Ui::TranslateBoxContentError;
					state->result = result.text;
					done({
						.text = std::move(result.text),
						.error = (result.error
								== ProviderError::LocalLanguagePackMissing)
							? UiError::LocalLanguagePackMissing
							: (result.error == ProviderError::None)
							? UiError::None
							: UiError::Unknown,
					});
				});
			},
		});
		box->addButton(tr::lng_mg_translate_use(), [=] {
			const auto field = weak.get();
			if (!state->result
				|| !field
				|| !SelectionUnchanged(field, from, till, selected)) {
				return;
			}
			// Rebuilt from the parts around the selection, so the tags
			// (formatting, custom emoji) outside it are kept.
			auto text = field->getTextWithTagsPart(0, from);
			text.text += state->result->text;
			const auto after = field->getTextWithTagsPart(till);
			const auto shift = int(text.text.size());
			for (auto tag : after.tags) {
				tag.offset += shift;
				text.tags.push_back(tag);
			}
			text.text += after.text;
			field->setTextWithTags(
				text,
				Ui::InputField::HistoryAction::NewEntry);
			box->closeBox();
		});
	}));
}

} // namespace

void AddFieldSelectionActions(
		std::shared_ptr<Main::SessionShow> show,
		not_null<Ui::InputField*> field,
		Fn<PeerData*()> peer) {
	const auto weak = base::make_weak(field);
	field->addContextMenuHook([=](Ui::InputField::ContextMenuRequest r) {
		const auto current = peer();
		const auto cursor = field->textCursor();
		const auto from = cursor.selectionStart();
		const auto till = cursor.selectionEnd();
		if (!current || current->isSecretChat() || from == till) {
			return;
		}
		const auto selected = field->getTextWithTagsPart(from, till);
		r.menu->addSeparator();
		r.menu->addAction(tr::lng_mg_mention(tr::now), [=] {
			ShowMentionBox(show, weak, from, till, selected);
		});
		r.menu->addAction(tr::lng_context_translate(tr::now), [=] {
			ShowTranslateBox(show, weak, current, from, till, selected.text);
		});
	});
}

} // namespace MG
