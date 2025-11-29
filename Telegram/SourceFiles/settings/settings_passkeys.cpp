/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "settings/settings_passkeys.h"

#include "settings/settings_common_session.h"
#include "data/components/passkeys.h"
#include "data/data_session.h"
#include "window/window_session_controller.h"
#include "main/main_session.h"
#include "platform/platform_webauthn.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/rect.h"
#include "ui/text/text_utilities.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/wrap/vertical_layout.h"
#include "lang/lang_keys.h"
#include "lottie/lottie_icon.h"
#include "styles/style_channel_earn.h"
#include "styles/style_chat.h"
#include "styles/style_layers.h"
#include "styles/style_premium.h"
#include "styles/style_settings.h"

namespace Settings {

class Passkeys : public Section<Passkeys> {
public:
	Passkeys(
		QWidget *parent,
		not_null<Window::SessionController*> controller);

	[[nodiscard]] rpl::producer<QString> title() override;

private:
	void setupContent(not_null<Window::SessionController*> controller);

};

void PasskeysNoneBox(
		not_null<Ui::GenericBox*> box,
		not_null<Main::Session*> session) {
	box->setWidth(st::boxWideWidth);
	box->setNoContentMargin(true);
	box->setCloseByEscape(true);
	box->addTopButton(st::boxTitleClose, [=] { box->closeBox(); });

	const auto content = box->verticalLayout().get();

	Ui::AddSkip(content);
	{
		const auto &size = st::settingsCloudPasswordIconSize;
		auto icon = CreateLottieIcon(
			content,
			{ .name = u"passkeys"_q, .sizeOverride = { size, size } },
			st::settingLocalPasscodeIconPadding);
		const auto animate = std::move(icon.animate);
		box->addRow(std::move(icon.widget), style::al_top);
		box->showFinishes() | rpl::take(1) | rpl::start_with_next([=] {
			animate(anim::repeat::once);
		}, content->lifetime());
	}
	Ui::AddSkip(content);
	Ui::AddSkip(content);
	box->addRow(
		object_ptr<Ui::FlatLabel>(
			content,
			tr::lng_settings_passkeys_none_title(),
			st::boxTitle),
		style::al_top);
	Ui::AddSkip(content);
	box->addRow(
		object_ptr<Ui::FlatLabel>(
			content,
			tr::lng_settings_passkeys_none_about(),
			st::channelEarnLearnDescription),
		style::al_top);
	Ui::AddSkip(content);
	Ui::AddSkip(content);
	Ui::AddSkip(content);
	{
		const auto padding = QMargins(
			st::settingsButton.padding.left(),
			st::boxRowPadding.top(),
			st::boxRowPadding.right(),
			st::boxRowPadding.bottom());
		const auto addEntry = [&](
				rpl::producer<QString> title,
				rpl::producer<QString> about,
				const style::icon &icon) {
			const auto top = content->add(
				object_ptr<Ui::FlatLabel>(
					content,
					std::move(title),
					st::channelEarnSemiboldLabel),
				padding);
			Ui::AddSkip(content, st::channelEarnHistoryThreeSkip);
			content->add(
				object_ptr<Ui::FlatLabel>(
					content,
					std::move(about),
					st::channelEarnHistoryRecipientLabel),
				padding);
			const auto left = Ui::CreateChild<Ui::RpWidget>(
				box->verticalLayout().get());
			left->paintRequest(
			) | rpl::start_with_next([=] {
				auto p = Painter(left);
				icon.paint(p, 0, 0, left->width());
			}, left->lifetime());
			left->resize(icon.size());
			top->geometryValue(
			) | rpl::start_with_next([=](const QRect &g) {
				left->moveToLeft(
					(g.left() - left->width()) / 2,
					g.top() + st::channelEarnHistoryThreeSkip);
			}, left->lifetime());
		};
		addEntry(
			tr::lng_settings_passkeys_none_info1_title(),
			tr::lng_settings_passkeys_none_info1_about(),
			st::settingsIconPasskeysAboutIcon1);
		Ui::AddSkip(content);
		Ui::AddSkip(content);
		addEntry(
			tr::lng_settings_passkeys_none_info2_title(),
			tr::lng_settings_passkeys_none_info2_about(),
			st::settingsIconPasskeysAboutIcon2);
		Ui::AddSkip(content);
		Ui::AddSkip(content);
		addEntry(
			tr::lng_settings_passkeys_none_info3_title(),
			tr::lng_settings_passkeys_none_info3_about(),
			st::settingsIconPasskeysAboutIcon3);
		Ui::AddSkip(content);
		Ui::AddSkip(content);
	}
	Ui::AddSkip(content);
	Ui::AddSkip(content);
	{
		const auto &st = st::premiumPreviewDoubledLimitsBox;
		box->setStyle(st);
		auto button = object_ptr<Ui::RoundButton>(
			box,
			tr::lng_settings_passkeys_none_button(),
			st::defaultActiveButton);
		button->setTextTransform(Ui::RoundButton::TextTransform::NoTransform);
		button->resizeToWidth(box->width()
			- st.buttonPadding.left()
			- st.buttonPadding.left());
		button->setClickedCallback([=] {
			session->passkeys().initRegistration([=](
					const Data::Passkey::RegisterData &data) {
				Platform::WebAuthn::RegisterKey(data, [=](
						Platform::WebAuthn::RegisterResult result) {
					if (!result.success) {
						return;
					}
					session->passkeys().registerPasskey(result, [=] {
						box->closeBox();
					});
				});
			});
		});
		box->addButton(std::move(button));
	}
}

Passkeys::Passkeys(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent) {
	setupContent(controller);
}

rpl::producer<QString> Passkeys::title() {
	return tr::lng_settings_passkeys_title();
}

void Passkeys::setupContent(
		not_null<Window::SessionController*> controller) {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);

	Ui::ResizeFitChild(this, content);
}

Type PasskeysId() {
	return Passkeys::Id();
}

} // namespace Settings
