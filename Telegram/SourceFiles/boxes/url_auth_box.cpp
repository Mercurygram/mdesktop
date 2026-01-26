/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "boxes/url_auth_box.h"

#include "apiwrap.h"
#include "base/qthelp_url.h"
#include "core/application.h"
#include "core/click_handler_types.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history_item_components.h"
#include "history/history_item.h"
#include "history/history.h"
#include "info/profile/info_profile_values.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "send_credits_box.h"
#include "ui/boxes/confirm_box.h"
#include "ui/controls/userpic_button.h"
#include "ui/layers/generic_box.h"
#include "ui/text/text_utilities.h"
#include "ui/userpic_view.h"
#include "ui/vertical_list.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/menu/menu_action.h"
#include "ui/widgets/popup_menu.h"
#include "ui/wrap/vertical_layout.h"
#include "styles/style_layers.h"
#include "styles/style_boxes.h"
#include "styles/style_settings.h"
#include "styles/style_premium.h"
#include "styles/style_window.h"
#include "styles/style_menu_icons.h"

namespace UrlAuthBox {
namespace {

class SwitchableUserpicButton final : public Ui::RippleButton {
public:
	SwitchableUserpicButton(
		not_null<Ui::RpWidget*> parent,
		not_null<UserData*> peer,
		int size);

	void setExpanded(bool expanded);
	[[nodiscard]] bool isExpanded() const {
		return _expanded;
	}

	void setUser(not_null<UserData*> user);
	[[nodiscard]] not_null<UserData*> user() const {
		return _user;
	}

private:
	void paintEvent(QPaintEvent *e) override;
	QImage prepareRippleMask() const override;
	QPoint prepareRippleStartPosition() const override;

	const int _size;
	const int _userpicSize;
	const int _skip;
	bool _expanded = false;
	not_null<UserData*> _user;
	base::unique_qptr<Ui::UserpicButton> _userpic;

};

SwitchableUserpicButton::SwitchableUserpicButton(
	not_null<Ui::RpWidget*> parent,
	not_null<UserData*> peer,
	int size)
: RippleButton(parent, st::defaultRippleAnimation)
, _size(size)
, _userpicSize(st::restoreUserpicIcon.photoSize)
, _skip((_size - _userpicSize) / 2)
, _user(peer)
, _userpic(base::make_unique_q<Ui::UserpicButton>(
	this,
	peer,
	st::restoreUserpicIcon)) {
	resize(_size, _size);
	_userpic->move(_skip, _skip);
	_userpic->setAttribute(Qt::WA_TransparentForMouseEvents);
}

void SwitchableUserpicButton::setUser(not_null<UserData*> user) {
	if (_user == user) {
		return;
	}
	_user = user;
	_userpic = base::make_unique_q<Ui::UserpicButton>(
		this,
		user,
		st::restoreUserpicIcon);
	_userpic->moveToRight(_skip, _skip);
	_userpic->setAttribute(Qt::WA_TransparentForMouseEvents);
	_userpic->show();
	update();
}

void SwitchableUserpicButton::setExpanded(bool expanded) {
	if (_expanded == expanded) {
		return;
	}
	_expanded = expanded;
	const auto w = _expanded
		? (_size * 2.5 - _userpicSize)
		: _size;
	resize(w, _size);
	_userpic->moveToRight(_skip, _skip);
	update();
}

void SwitchableUserpicButton::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	paintRipple(p, 0, 0);

	if (!_expanded) {
		return;
	}

	const auto arrowSize = st::lineWidth * 12;
	const auto center = QPoint(_size / 2, height() / 2 + st::lineWidth * 4);

	auto pen = QPen(st::windowSubTextFg);
	pen.setWidthF(st::lineWidth * 1.5);
	p.setPen(pen);
	p.setRenderHint(QPainter::Antialiasing);

	p.drawLine(center, center + QPoint(-arrowSize / 2, -arrowSize / 2));
	p.drawLine(center, center + QPoint(arrowSize / 2, -arrowSize / 2));
}

QImage SwitchableUserpicButton::prepareRippleMask() const {
	return _expanded
		? Ui::RippleAnimation::RoundRectMask(size(), height() / 2)
		: Ui::RippleAnimation::EllipseMask(size());
}

QPoint SwitchableUserpicButton::prepareRippleStartPosition() const {
	return mapFromGlobal(QCursor::pos());
}

using AnotherSessionFactory = Fn<not_null<Main::Session*>()>;

struct SwitchAccountResult {
	not_null<Ui::RpWidget*> widget;
	AnotherSessionFactory anotherSession;
};

[[nodiscard]] SwitchAccountResult AddAccountsMenu(
		not_null<Ui::RpWidget*> parent) {
	const auto session = &Core::App().domain().active().session();
	const auto widget = Ui::CreateChild<SwitchableUserpicButton>(
		parent,
		session->user(),
		st::restoreUserpicIcon.photoSize + st::lineWidth * 8);
	const auto isCurrentTest = session->isTestMode();
	const auto filtered = [=] {
		auto result = std::vector<not_null<Main::Session*>>();
		for (const auto &account : Core::App().domain().orderedAccounts()) {
			if (!account->sessionExists()
				|| (account->session().user() == widget->user())
				|| (account->session().isTestMode() != isCurrentTest)) {
				continue;
			}
			result.push_back(&account->session());
		}
		return result;
	};
	const auto isSingle = filtered().empty();
	widget->setExpanded(!isSingle);
	widget->setAttribute(Qt::WA_TransparentForMouseEvents, isSingle);
	struct State {
		base::unique_qptr<Ui::PopupMenu> menu;
	};
	const auto state = widget->lifetime().make_state<State>();
	widget->setClickedCallback([=] {
		const auto &st = st::popupMenuWithIcons;
		state->menu = base::make_unique_q<Ui::PopupMenu>(widget, st);
		for (const auto &anotherSession : filtered()) {
			const auto user = anotherSession->user();
			const auto action = new QAction(user->name(), state->menu);
			QObject::connect(action, &QAction::triggered, [=] {
				widget->setUser(user);
			});
			auto owned = base::make_unique_q<Ui::Menu::Action>(
				state->menu->menu(),
				state->menu->menu()->st(),
				action,
				nullptr,
				nullptr);
			const auto userpic = Ui::CreateChild<Ui::UserpicButton>(
				owned.get(),
				user,
				st::lockSetupEmailUserpicSmall);
			userpic->setAttribute(Qt::WA_TransparentForMouseEvents);
			userpic->move(st.menu.itemIconPosition);
			state->menu->addAction(std::move(owned));
		}

		state->menu->setForcedOrigin(Ui::PanelAnimation::Origin::TopRight);
		state->menu->popup(
			widget->mapToGlobal(
				QPoint(
					widget->width() + st.shadow.extend.right(),
					widget->height())));
	});
	return {
		widget,
		[=] { return &widget->user()->session(); },
	};
}

void AddAuthInfoRow(
		not_null<Ui::VerticalLayout*> container,
		const QString &topText,
		const QString &bottomText,
		const QString &leftText,
		const style::icon &icon) {
	const auto row = container->add(
		object_ptr<Ui::RpWidget>(container),
		st::boxRowPadding);

	const auto topLabel = Ui::CreateChild<Ui::FlatLabel>(
		row,
		topText,
		st::boxLabel);
	const auto bottomLabel = Ui::CreateChild<Ui::FlatLabel>(
		row,
		bottomText,
		st::sessionValueLabel);
	const auto leftLabel = Ui::CreateChild<Ui::FlatLabel>(
		row,
		leftText,
		st::boxLabel);

	rpl::combine(
		row->widthValue(),
		topLabel->sizeValue(),
		bottomLabel->sizeValue()
	) | rpl::on_next([=](int rowWidth, QSize topSize, QSize bottomSize) {
		const auto totalHeight = topSize.height() + bottomSize.height();
		row->resize(rowWidth, totalHeight);

		topLabel->moveToRight(0, 0);
		bottomLabel->moveToRight(0, topSize.height());

		const auto left = st::sessionValuePadding.left();
		leftLabel->moveToLeft(left, (totalHeight - leftLabel->height()) / 2);
	}, row->lifetime());

	{
		const auto widget = Ui::CreateChild<Ui::RpWidget>(row);
		widget->resize(icon.size());

		rpl::combine(
			row->widthValue(),
			topLabel->sizeValue(),
			bottomLabel->sizeValue()
		) | rpl::on_next([=](int rowWidth, QSize topSize, QSize bottomSize) {
			const auto totalHeight = topSize.height() + bottomSize.height();
			widget->moveToLeft(0, (totalHeight - leftLabel->height()) / 2);
		}, row->lifetime());

		widget->paintRequest() | rpl::on_next([=, &icon] {
			auto p = QPainter(widget);
			icon.paintInCenter(p, widget->rect());
		}, widget->lifetime());
	}
}

} // namespace

void RequestButton(
	std::shared_ptr<Ui::Show> show,
	const MTPDurlAuthResultRequest &request,
	not_null<const HistoryItem*> message,
	int row,
	int column);
void RequestUrl(
	std::shared_ptr<Ui::Show> show,
	const MTPDurlAuthResultRequest &request,
	not_null<Main::Session*> session,
	const QString &url,
	QVariant context);

void Show(
	not_null<Ui::GenericBox*> box,
	not_null<Main::Session*> session,
	const QString &url,
	const QString &domain,
	UserData *bot,
	Fn<void(Result)> callback);
void ShowDetails(
	not_null<Ui::GenericBox*> box,
	not_null<Main::Session*> session,
	const QString &url,
	const QString &domain,
	Fn<void(Result)> callback,
	UserData *bot,
	const QString &browser,
	const QString &platform,
	const QString &ip,
	const QString &region);

void ActivateButton(
		std::shared_ptr<Ui::Show> show,
		not_null<const HistoryItem*> message,
		int row,
		int column) {
	const auto itemId = message->fullId();
	const auto button = HistoryMessageMarkupButton::Get(
		&message->history()->owner(),
		itemId,
		row,
		column);
	if (button->requestId || !message->isRegular()) {
		return;
	}
	const auto session = &message->history()->session();
	const auto inputPeer = message->history()->peer->input();
	const auto buttonId = button->buttonId;
	const auto url = QString::fromUtf8(button->data);

	using Flag = MTPmessages_RequestUrlAuth::Flag;
	button->requestId = session->api().request(MTPmessages_RequestUrlAuth(
		MTP_flags(Flag::f_peer | Flag::f_msg_id | Flag::f_button_id),
		inputPeer,
		MTP_int(itemId.msg),
		MTP_int(buttonId),
		MTPstring() // #TODO auth url
	)).done([=](const MTPUrlAuthResult &result) {
		const auto button = HistoryMessageMarkupButton::Get(
			&session->data(),
			itemId,
			row,
			column);
		if (!button) {
			return;
		}

		button->requestId = 0;
		result.match([&](const MTPDurlAuthResultAccepted &data) {
			if (const auto url = data.vurl()) {
				UrlClickHandler::Open(qs(url->v));
			}
		}, [&](const MTPDurlAuthResultDefault &data) {
			HiddenUrlClickHandler::Open(url);
		}, [&](const MTPDurlAuthResultRequest &data) {
			if (const auto item = session->data().message(itemId)) {
				RequestButton(show, data, item, row, column);
			}
		});
	}).fail([=] {
		const auto button = HistoryMessageMarkupButton::Get(
			&session->data(),
			itemId,
			row,
			column);
		if (!button) {
			return;
		}

		button->requestId = 0;
		HiddenUrlClickHandler::Open(url);
	}).send();
}

void ActivateUrl(
		std::shared_ptr<Ui::Show> show,
		not_null<Main::Session*> session,
		const QString &url,
		QVariant context) {
	context = QVariant::fromValue([&] {
		auto result = context.value<ClickHandlerContext>();
		result.skipBotAutoLogin = true;
		return result;
	}());

	using Flag = MTPmessages_RequestUrlAuth::Flag;
	session->api().request(MTPmessages_RequestUrlAuth(
		MTP_flags(Flag::f_url),
		MTPInputPeer(),
		MTPint(), // msg_id
		MTPint(), // button_id
		MTP_string(url)
	)).done([=](const MTPUrlAuthResult &result) {
		result.match([&](const MTPDurlAuthResultAccepted &data) {
			UrlClickHandler::Open(qs(data.vurl().value_or_empty()), context);
		}, [&](const MTPDurlAuthResultDefault &data) {
			HiddenUrlClickHandler::Open(url, context);
		}, [&](const MTPDurlAuthResultRequest &data) {
			RequestUrl(show, data, session, url, context);
		});
	}).fail([=] {
		HiddenUrlClickHandler::Open(url, context);
	}).send();
}

void RequestButton(
		std::shared_ptr<Ui::Show> show,
		const MTPDurlAuthResultRequest &request,
		not_null<const HistoryItem*> message,
		int row,
		int column) {
	const auto itemId = message->fullId();
	const auto button = HistoryMessageMarkupButton::Get(
		&message->history()->owner(),
		itemId,
		row,
		column);
	if (!button || button->requestId || !message->isRegular()) {
		return;
	}
	const auto session = &message->history()->session();
	const auto inputPeer = message->history()->peer->input();
	const auto buttonId = button->buttonId;
	const auto url = QString::fromUtf8(button->data);

	const auto bot = request.is_request_write_access()
		? session->data().processUser(request.vbot()).get()
		: nullptr;
	const auto box = std::make_shared<base::weak_qptr<Ui::BoxContent>>();
	const auto finishWithUrl = [=](const QString &url) {
		if (*box) {
			(*box)->closeBox();
		}
		UrlClickHandler::Open(url);
	};
	const auto callback = [=](Result result) {
		if (!result.auth) {
			finishWithUrl(url);
		} else if (session->data().message(itemId)) {
			using Flag = MTPmessages_AcceptUrlAuth::Flag;
			const auto flags = Flag(0)
				| (result.allowWrite ? Flag::f_write_allowed : Flag(0))
				| (result.sharePhone ? Flag::f_share_phone_number : Flag(0))
				| (Flag::f_peer | Flag::f_msg_id | Flag::f_button_id);
			session->api().request(MTPmessages_AcceptUrlAuth(
				MTP_flags(flags),
				inputPeer,
				MTP_int(itemId.msg),
				MTP_int(buttonId),
				MTPstring() // #TODO auth url
			)).done([=](const MTPUrlAuthResult &result) {
				const auto to = result.match(
				[&](const MTPDurlAuthResultAccepted &data) {
					return qs(data.vurl().value_or_empty());
				}, [&](const MTPDurlAuthResultDefault &data) {
					return url;
				}, [&](const MTPDurlAuthResultRequest &data) {
					LOG(("API Error: "
						"got urlAuthResultRequest after acceptUrlAuth."));
					return url;
				});
				finishWithUrl(to);
			}).fail([=] {
				finishWithUrl(url);
			}).send();
		}
	};
	*box = show->show(
		Box(Show, session, url, qs(request.vdomain()), bot, callback),
		Ui::LayerOption::KeepOther);
}

void RequestUrl(
		std::shared_ptr<Ui::Show> show,
		const MTPDurlAuthResultRequest &request,
		not_null<Main::Session*> session,
		const QString &url,
		QVariant context) {
	const auto bot = request.is_request_write_access()
		? session->data().processUser(request.vbot()).get()
		: nullptr;
	const auto box = std::make_shared<base::weak_qptr<Ui::BoxContent>>();
	const auto finishWithUrl = [=](const QString &url) {
		if (*box) {
			(*box)->closeBox();
		}
		UrlClickHandler::Open(url, context);
	};
	const auto anotherSessionFactory
		= std::make_shared<AnotherSessionFactory>(nullptr);
	const auto sendRequest = [=](Result result) {
		if (!result.auth) {
			finishWithUrl(url);
		} else {
			using Flag = MTPmessages_AcceptUrlAuth::Flag;
			const auto flags = Flag::f_url
				| (result.allowWrite ? Flag::f_write_allowed : Flag(0))
				| (result.sharePhone ? Flag::f_share_phone_number : Flag(0));
			const auto currentSession = anotherSessionFactory
				? (*anotherSessionFactory)()
				: session;
			currentSession->api().request(MTPmessages_AcceptUrlAuth(
				MTP_flags(flags),
				MTPInputPeer(),
				MTPint(), // msg_id
				MTPint(), // button_id
				MTP_string(url)
			)).done([=](const MTPUrlAuthResult &result) {
				const auto to = result.match(
				[&](const MTPDurlAuthResultAccepted &data) {
					return qs(data.vurl().value_or_empty());
				}, [&](const MTPDurlAuthResultDefault &data) {
					return url;
				}, [&](const MTPDurlAuthResultRequest &data) {
					LOG(("API Error: "
						"got urlAuthResultRequest after acceptUrlAuth."));
					return url;
				});
				finishWithUrl(to);
			}).fail([=] {
				finishWithUrl(url);
			}).send();
		}
	};
	const auto requestPhone = request.is_request_phone_number();
	const auto domain = qs(request.vdomain());
	const auto browser = qs(request.vbrowser().value_or("Unknown browser"));
	const auto device = qs(request.vplatform().value_or("Unknown platform"));
	const auto ip = qs(request.vip().value_or("Unknown IP"));
	const auto region = qs(request.vregion().value_or("Unknown region"));
	*box = show->show(Box([=](not_null<Ui::GenericBox*> box) {
		const auto callback = [=](Result result) {
			if (!requestPhone) {
				return sendRequest(result);
			}
			box->uiShow()->show(Box([=](not_null<Ui::GenericBox*> box) {
				box->setTitle(tr::lng_url_auth_phone_sure_title());
				const auto confirm = [=](bool confirmed) {
					return [=](Fn<void()> close) {
						auto copy = result;
						copy.sharePhone = confirmed;
						sendRequest(copy);
						close();
					};
				};
				const auto currentSession = anotherSessionFactory
					? (*anotherSessionFactory)()
					: session;
				const auto capitalized = [=](const QString &value) {
					return value.left(1).toUpper() + value.mid(1).toLower();
				};
				using namespace Info::Profile;
				Ui::ConfirmBox(
					box,
					Ui::ConfirmBoxArgs{
						.text = tr::lng_url_auth_phone_sure_text(
							lt_domain,
							rpl::single(tr::bold(capitalized(domain))),
							lt_phone,
							PhoneValue(currentSession->user()),
							tr::rich),
						.confirmed = confirm(true),
						.cancelled = confirm(false),
						.confirmText = tr::lng_allow_bot(),
						.cancelText = tr::lng_url_auth_phone_sure_deny(),
					});
			}));
		};
		ShowDetails(
			box,
			session,
			url,
			domain,
			callback,
			bot,
			browser,
			device,
			ip,
			region);

		const auto content = box->verticalLayout();
		const auto accountResult = AddAccountsMenu(content);
		content->widthValue() | rpl::on_next([=, w = accountResult.widget] {
			w->moveToRight(st::lineWidth * 4, 0);
		}, accountResult.widget->lifetime());
		*anotherSessionFactory = accountResult.anotherSession;
	}));
}

void Show(
		not_null<Ui::GenericBox*> box,
		not_null<Main::Session*> session,
		const QString &url,
		const QString &domain,
		UserData *bot,
		Fn<void(Result)> callback) {
	box->setWidth(st::boxWidth);

	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			tr::lng_url_auth_open_confirm(tr::now, lt_link, url),
			st::boxLabel),
		st::boxPadding);

	const auto addCheckbox = [&](const TextWithEntities &text) {
		const auto checkbox = box->addRow(
			object_ptr<Ui::Checkbox>(
				box,
				text,
				true,
				st::urlAuthCheckbox),
			style::margins(
				st::boxPadding.left(),
				st::boxPadding.bottom(),
				st::boxPadding.right(),
				st::boxPadding.bottom()));
		checkbox->setAllowTextLines();
		return checkbox;
	};

	const auto auth = addCheckbox(
		tr::lng_url_auth_login_option(
			tr::now,
			lt_domain,
			tr::bold(domain),
			lt_user,
			tr::bold(session->user()->name()),
			tr::marked));

	const auto allow = bot
		? addCheckbox(tr::lng_url_auth_allow_messages(
			tr::now,
			lt_bot,
			tr::bold(bot->firstName),
			tr::marked))
		: nullptr;

	if (allow) {
		rpl::single(
			auth->checked()
		) | rpl::then(
			auth->checkedChanges()
		) | rpl::on_next([=](bool checked) {
			if (!checked) {
				allow->setChecked(false);
			}
			allow->setDisabled(!checked);
		}, auth->lifetime());
	}

	box->addButton(tr::lng_open_link(), [=] {
		const auto authed = auth->checked();
		const auto allowed = (authed && allow && allow->checked());
		callback({
			.auth = authed,
			.allowWrite = allowed,
		});
	});
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

void ShowDetails(
		not_null<Ui::GenericBox*> box,
		not_null<Main::Session*> session,
		const QString &url,
		const QString &domain,
		Fn<void(Result)> callback,
		UserData *bot,
		const QString &browser,
		const QString &platform,
		const QString &ip,
		const QString &region) {
	box->setWidth(st::boxWidth);

	const auto content = box->verticalLayout();

	Ui::AddSkip(content);
	Ui::AddSkip(content);
	if (bot) {
		const auto userpic = content->add(
			object_ptr<Ui::UserpicButton>(
				content,
				bot,
				st::defaultUserpicButton,
				Ui::PeerUserpicShape::Forum),
			st::boxRowPadding,
			style::al_top);
		userpic->setAttribute(Qt::WA_TransparentForMouseEvents);
		Ui::AddSkip(content);
		Ui::AddSkip(content);
	}

	const auto domainUrl = qthelp::validate_url(domain);
	content->add(
		object_ptr<Ui::FlatLabel>(
			content,
			domainUrl.isEmpty()
				? tr::lng_url_auth_login_button(tr::marked)
				: tr::lng_url_auth_login_title(
					lt_domain,
					rpl::single(Ui::Text::Link(domain, domainUrl)),
					tr::marked),
			st::boxTitle),
		st::boxRowPadding,
		style::al_top);
	Ui::AddSkip(content);

	content->add(
		object_ptr<Ui::FlatLabel>(
			content,
			tr::lng_url_auth_site_access(tr::rich),
			st::urlAuthCheckboxAbout),
		st::boxRowPadding);

	Ui::AddSkip(content);
	Ui::AddSkip(content);
	if (!platform.isEmpty() || !browser.isEmpty()) {
		AddAuthInfoRow(
			content,
			platform,
			browser,
			tr::lng_url_auth_device_label(tr::now),
			st::menuIconDevices);
	}
	Ui::AddSkip(content);
	Ui::AddSkip(content);

	if (!ip.isEmpty() || !region.isEmpty()) {
		AddAuthInfoRow(
			content,
			ip,
			region,
			tr::lng_url_auth_ip_label(tr::now),
			st::menuIconAddress);
	}
	Ui::AddSkip(content);
	Ui::AddSkip(content);

	Ui::AddDividerText(
		content,
		rpl::single(tr::lng_url_auth_login_attempt(tr::now)));
	Ui::AddSkip(content);

	auto allowMessages = (Ui::SettingsButton*)(nullptr);
	if (bot) {
		allowMessages = content->add(
			object_ptr<Ui::SettingsButton>(
				content,
				tr::lng_url_auth_allow_messages_label()));
		allowMessages->toggleOn(rpl::single(false));
		Ui::AddSkip(content);
		Ui::AddDividerText(
			content,
			tr::lng_url_auth_allow_messages_about(
				lt_bot,
				Info::Profile::NameValue(bot)));
		Ui::AddSkip(content);
	}

	box->addButton(tr::lng_url_auth_login_button(), [=] {
		callback({
			.auth = true,
			.allowWrite = (allowMessages && allowMessages->toggled()),
		});
	});
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

} // namespace UrlAuthBox
