/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "boxes/create_ai_tone_box.h"

#include "data/data_ai_compose_tones.h"
#include "data/data_session.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "ui/layers/generic_box.h"
#include "ui/vertical_list.h"
#include "ui/widgets/fields/input_field.h"

#include "styles/style_boxes.h"
#include "styles/style_layers.h"

namespace {

void SetupToneBox(
		not_null<Ui::GenericBox*> box,
		not_null<Main::Session*> session,
		const QString &initialName,
		const QString &initialPrompt,
		rpl::producer<QString> title,
		Fn<void(QString, QString)> submit) {
	box->setTitle(std::move(title));

	const auto name = box->addRow(object_ptr<Ui::InputField>(
		box,
		st::defaultInputField,
		tr::lng_ai_compose_tone_name(),
		initialName));

	Ui::AddSkip(box->verticalLayout());

	const auto promptSt = box->lifetime().make_state<style::InputField>(
		st::newGroupDescription);
	const auto prompt = box->addRow(object_ptr<Ui::InputField>(
		box,
		*promptSt,
		Ui::InputField::Mode::MultiLine,
		tr::lng_ai_compose_tone_prompt(),
		initialPrompt));

	box->setFocusCallback([=] {
		name->setFocusFast();
	});

	const auto save = [=] {
		const auto nameText = name->getLastText().trimmed();
		const auto promptText = prompt->getLastText().trimmed();
		if (nameText.isEmpty() || promptText.isEmpty()) {
			if (nameText.isEmpty()) {
				name->showError();
			}
			if (promptText.isEmpty()) {
				prompt->showError();
			}
			return;
		}
		submit(nameText, promptText);
	};

	box->addButton(tr::lng_ai_compose_tone_save(), save);
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

} // namespace

void CreateAiToneBox(
		not_null<Ui::GenericBox*> box,
		not_null<Main::Session*> session,
		Fn<void()> saved) {
	SetupToneBox(
		box,
		session,
		QString(),
		QString(),
		tr::lng_ai_compose_create_tone_title(),
		[=](const QString &name, const QString &prompt) {
			session->data().aiComposeTones().create(
				name,
				prompt,
				0,
				false,
				[=](Data::AiComposeTone) {
					box->closeBox();
					if (saved) {
						saved();
					}
				});
		});
}

void EditAiToneBox(
		not_null<Ui::GenericBox*> box,
		not_null<Main::Session*> session,
		const Data::AiComposeTone &tone,
		Fn<void()> saved) {
	const auto toneId = tone.id;
	const auto toneAccessHash = tone.accessHash;
	SetupToneBox(
		box,
		session,
		tone.title,
		tone.prompt,
		tr::lng_ai_compose_edit_tone_title(),
		[=](const QString &name, const QString &prompt) {
			auto toneCopy = Data::AiComposeTone();
			toneCopy.id = toneId;
			toneCopy.accessHash = toneAccessHash;
			session->data().aiComposeTones().update(
				toneCopy,
				name,
				prompt,
				std::nullopt,
				std::nullopt,
				[=](Data::AiComposeTone) {
					box->closeBox();
					if (saved) {
						saved();
					}
				});
		});
}
