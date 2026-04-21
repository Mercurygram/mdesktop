/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "settings/business/settings_chatbots.h"

#include "apiwrap.h"
#include "boxes/peers/edit_peer_permissions_box.h"
#include "boxes/peers/prepare_short_info_box.h"
#include "boxes/peer_list_box.h"
#include "core/application.h"
#include "data/business/data_business_chatbots.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "settings/business/settings_recipients_helper.h"
#include "ui/boxes/confirm_box.h"
#include "ui/effects/ripple_animation.h"
#include "ui/text/text_utilities.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/buttons.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/painter.h"
#include "ui/vertical_list.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"

namespace Settings {
namespace {

constexpr auto kDebounceTimeout = crl::time(400);

enum class LookupState {
	Empty,
	Loading,
	Unsupported,
	Ready,
};

struct BotState {
	UserData *bot = nullptr;
	LookupState state = LookupState::Empty;
};

enum class PreviewActionKind {
	Remove,
	Add,
};

[[nodiscard]] constexpr Data::ChatbotsPermissions Defaults() {
	return Data::ChatbotsPermission::ViewMessages;
}

class Chatbots final : public Section<Chatbots> {
public:
	Chatbots(
		QWidget *parent,
		not_null<Window::SessionController*> controller);
	~Chatbots();

	[[nodiscard]] bool closeByOutsideClick() const override;
	[[nodiscard]] rpl::producer<QString> title() override;

	const Ui::RoundRect *bottomSkipRounding() const override;

private:
	void setupContent();
	void refreshDetails();
	void save();

	Ui::RoundRect _bottomSkipRounding;

	Ui::VerticalLayout *_detailsWrap = nullptr;
	Ui::VerticalLayout *_permissionsWrap = nullptr;

	rpl::variable<Data::BusinessRecipients> _recipients;
	rpl::variable<UserData*> _committedBot;
	Data::BusinessRecipients _committedRecipients;
	Data::ChatbotsPermissions _committedPermissions = Defaults();
	rpl::variable<QString> _usernameValue;
	rpl::variable<BotState> _botValue;
	rpl::variable<Data::ChatbotsPermissions> _permissions = Defaults();
	Fn<Data::ChatbotsPermissions()> _resolvePermissions;

};

class PreviewController final : public PeerListController {
public:
	PreviewController(
		not_null<PeerData*> peer,
		PreviewActionKind kind,
		Fn<void()> resetBot,
		Fn<void()> addBot);

	void prepare() override;
	void loadMoreRows() override;
	void rowClicked(not_null<PeerListRow*> row) override;
	void rowRightActionClicked(not_null<PeerListRow*> row) override;
	Main::Session &session() const override;

private:
	const not_null<PeerData*> _peer;
	const PreviewActionKind _kind;
	const Fn<void()> _resetBot;
	const Fn<void()> _addBot;
	rpl::lifetime _lifetime;

};

class PreviewRow final : public PeerListRow {
public:
	PreviewRow(not_null<PeerData*> peer, PreviewActionKind kind);

	QSize rightActionSize() const override;
	QMargins rightActionMargins() const override;
	void rightActionPaint(
		Painter &p,
		int x,
		int y,
		int outerWidth,
		bool selected,
		bool actionSelected) override;
	void rightActionAddRipple(
		QPoint point,
		Fn<void()> updateCallback) override;
	void rightActionStopLastRipple() override;

private:
	[[nodiscard]] QSize addPillSize() const;

	PreviewActionKind _kind = PreviewActionKind::Remove;
	QString _addText;
	int _addTextWidth = 0;
	std::unique_ptr<Ui::RippleAnimation> _actionRipple;

};

PreviewRow::PreviewRow(
	not_null<PeerData*> peer,
	PreviewActionKind kind)
: PeerListRow(peer)
, _kind(kind)
, _addText((_kind == PreviewActionKind::Add)
	? tr::lng_chatbots_add(tr::now)
	: QString())
, _addTextWidth((_kind == PreviewActionKind::Add)
	? st::settingsChatbotsAddButton.style.font->width(_addText)
	: 0) {
}

QSize PreviewRow::addPillSize() const {
	const auto &st = st::settingsChatbotsAddButton;
	const auto width = _addTextWidth - st.width;
	return QSize(std::max(width, st.height), st.height);
}

QSize PreviewRow::rightActionSize() const {
	if (_kind == PreviewActionKind::Add) {
		return addPillSize();
	}
	return QSize(
		st::settingsChatbotsDeleteIcon.width(),
		st::settingsChatbotsDeleteIcon.height()) * 2;
}

QMargins PreviewRow::rightActionMargins() const {
	const auto itemHeight = st::peerListSingleRow.item.height;
	const auto size = rightActionSize();
	const auto skipV = (itemHeight - size.height()) / 2;
	const auto skipRight = (_kind == PreviewActionKind::Add)
		? st::settingsChatbotsAddMargin
		: skipV;
	return QMargins(0, skipV, skipRight, 0);
}

void PreviewRow::rightActionPaint(
		Painter &p,
		int x,
		int y,
		int outerWidth,
		bool selected,
		bool actionSelected) {
	const auto size = rightActionSize();
	if (_kind == PreviewActionKind::Add) {
		const auto &st = st::settingsChatbotsAddButton;
		const auto rect = QRect(QPoint(x, y), size);
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(actionSelected ? st.textBgOver : st.textBg);
		const auto radius = size.height() / 2.;
		p.drawRoundedRect(rect, radius, radius);
		if (_actionRipple) {
			_actionRipple->paint(p, x, y, outerWidth);
			if (_actionRipple->empty()) {
				_actionRipple.reset();
			}
		}
		p.setPen(actionSelected ? st.textFgOver : st.textFg);
		p.setFont(st.style.font);
		p.drawText(rect, Qt::AlignCenter, _addText);
		return;
	}
	if (_actionRipple) {
		_actionRipple->paint(p, x, y, outerWidth);
		if (_actionRipple->empty()) {
			_actionRipple.reset();
		}
	}
	const auto rect = QRect(QPoint(x, y), size);
	(actionSelected
		? st::settingsChatbotsDeleteIconOver
		: st::settingsChatbotsDeleteIcon).paintInCenter(p, rect);
}

void PreviewRow::rightActionAddRipple(
		QPoint point,
		Fn<void()> updateCallback) {
	if (!_actionRipple) {
		const auto size = rightActionSize();
		const auto add = (_kind == PreviewActionKind::Add);
		auto mask = add
			? Ui::RippleAnimation::RoundRectMask(size, size.height() / 2)
			: Ui::RippleAnimation::EllipseMask(size);
		const auto &ripple = add
			? st::settingsChatbotsAddButton.ripple
			: st::defaultRippleAnimation;
		_actionRipple = std::make_unique<Ui::RippleAnimation>(
			ripple,
			std::move(mask),
			std::move(updateCallback));
	}
	_actionRipple->add(point);
}

void PreviewRow::rightActionStopLastRipple() {
	if (_actionRipple) {
		_actionRipple->lastStop();
	}
}

PreviewController::PreviewController(
	not_null<PeerData*> peer,
	PreviewActionKind kind,
	Fn<void()> resetBot,
	Fn<void()> addBot)
: _peer(peer)
, _kind(kind)
, _resetBot(std::move(resetBot))
, _addBot(std::move(addBot)) {
}

void PreviewController::prepare() {
	delegate()->peerListAppendRow(
		std::make_unique<PreviewRow>(_peer, _kind));
	delegate()->peerListRefreshRows();
}

void PreviewController::loadMoreRows() {
}

void PreviewController::rowClicked(not_null<PeerListRow*> row) {
}

void PreviewController::rowRightActionClicked(
		not_null<PeerListRow*> row) {
	if (_kind == PreviewActionKind::Add) {
		_addBot();
	} else {
		_resetBot();
	}
}

Main::Session &PreviewController::session() const {
	return _peer->session();
}

[[nodiscard]] rpl::producer<QString> DebouncedValue(
		not_null<Ui::InputField*> field) {
	return [=](auto consumer) {

		auto result = rpl::lifetime();
		struct State {
			base::Timer timer;
			QString lastText;
		};
		const auto state = result.make_state<State>();
		const auto push = [=] {
			state->timer.cancel();
			consumer.put_next_copy(state->lastText);
		};
		state->timer.setCallback(push);
		state->lastText = field->getLastText();
		consumer.put_next_copy(field->getLastText());
		field->changes() | rpl::on_next([=] {
			const auto &text = field->getLastText();
			const auto was = std::exchange(state->lastText, text);
			if (std::abs(int(text.size()) - int(was.size())) == 1) {
				state->timer.callOnce(kDebounceTimeout);
			} else {
				push();
			}
		}, result);
		return result;
	};
}

[[nodiscard]] QString ExtractUsername(QString text) {
	text = text.trimmed();
	if (text.startsWith(QChar('@'))) {
		return text.mid(1);
	}
	static const auto expression = QRegularExpression(
		"^(https://)?([a-zA-Z0-9\\.]+/)?([a-zA-Z0-9_\\.]+)");
	const auto match = expression.match(text);
	return match.hasMatch() ? match.captured(3) : text;
}

[[nodiscard]] rpl::producer<BotState> LookupBot(
		not_null<Main::Session*> session,
		rpl::producer<QString> usernameChanges) {
	using Cache = base::flat_map<QString, UserData*>;
	const auto cache = std::make_shared<Cache>();
	return std::move(
		usernameChanges
	) | rpl::map([=](const QString &username) -> rpl::producer<BotState> {
		const auto extracted = ExtractUsername(username);
		const auto owner = &session->data();
		static const auto expression = QRegularExpression(
			"^[a-zA-Z0-9_\\.]+$");
		if (!expression.match(extracted).hasMatch()) {
			return rpl::single(BotState());
		} else if (const auto peer = owner->peerByUsername(extracted)) {
			if (const auto user = peer->asUser(); user && user->isBot()) {
				if (user->botInfo->supportsBusiness) {
					return rpl::single(BotState{
						.bot = user,
						.state = LookupState::Ready,
					});
				}
				return rpl::single(BotState{
					.state = LookupState::Unsupported,
				});
			}
			return rpl::single(BotState{
				.state = LookupState::Ready,
			});
		} else if (const auto i = cache->find(extracted); i != end(*cache)) {
			return rpl::single(BotState{
				.bot = i->second,
				.state = LookupState::Ready,
			});
		}

		return [=](auto consumer) {
			auto result = rpl::lifetime();

			const auto requestId = result.make_state<mtpRequestId>();
			*requestId = session->api().request(MTPcontacts_ResolveUsername(
				MTP_flags(0),
				MTP_string(extracted),
				MTP_string()
			)).done([=](const MTPcontacts_ResolvedPeer &result) {
				const auto &data = result.data();
				session->data().processUsers(data.vusers());
				session->data().processChats(data.vchats());
				const auto peerId = peerFromMTP(data.vpeer());
				const auto peer = session->data().peer(peerId);
				if (const auto user = peer->asUser()) {
					if (user->isBot()) {
						cache->emplace(extracted, user);
						consumer.put_next(BotState{
							.bot = user,
							.state = LookupState::Ready,
						});
						return;
					}
				}
				cache->emplace(extracted, nullptr);
				consumer.put_next(BotState{ .state = LookupState::Ready });
			}).fail([=] {
				cache->emplace(extracted, nullptr);
				consumer.put_next(BotState{ .state = LookupState::Ready });
			}).send();

			result.add([=] {
				session->api().request(*requestId).cancel();
			});
			return result;
		};
	}) | rpl::flatten_latest();
}

[[nodiscard]] object_ptr<Ui::RpWidget> MakeBotPreview(
		not_null<Ui::RpWidget*> parent,
		rpl::producer<std::pair<BotState, PreviewActionKind>> stateAndKind,
		Fn<void()> resetBot,
		Fn<void()> addBot) {
	auto result = object_ptr<Ui::SlideWrap<>>(
		parent.get(),
		object_ptr<Ui::RpWidget>(parent.get()));
	const auto raw = result.data();
	const auto inner = raw->entity();
	raw->hide(anim::type::instant);

	const auto child = inner->lifetime().make_state<Ui::RpWidget*>(nullptr);
	std::move(stateAndKind) | rpl::filter([=](const auto &pair) {
		return pair.first.state != LookupState::Loading;
	}) | rpl::on_next([=](std::pair<BotState, PreviewActionKind> pair) {
		const auto &state = pair.first;
		const auto kind = pair.second;
		raw->toggle(
			(state.state == LookupState::Ready
				|| state.state == LookupState::Unsupported),
			anim::type::normal);
		if (state.bot) {
			const auto delegate = parent->lifetime().make_state<
				PeerListContentDelegateSimple
			>();
			const auto controller = parent->lifetime().make_state<
				PreviewController
			>(state.bot, kind, resetBot, addBot);
			controller->setStyleOverrides(&st::peerListSingleRow);
			const auto content = Ui::CreateChild<PeerListContent>(
				inner,
				controller);
			delegate->setContent(content);
			controller->setDelegate(delegate);
			delete base::take(*child);
			*child = content;
		} else if (state.state == LookupState::Ready
			|| state.state == LookupState::Unsupported) {
			const auto content = Ui::CreateChild<Ui::RpWidget>(inner);
			const auto label = Ui::CreateChild<Ui::FlatLabel>(
				content,
				(state.state == LookupState::Unsupported
					? tr::lng_chatbots_not_supported()
					: tr::lng_chatbots_not_found()),
				st::settingsChatbotsNotFound);
			content->resize(
				inner->width(),
				st::peerListSingleRow.item.height);
			rpl::combine(
				content->sizeValue(),
				label->sizeValue()
			) | rpl::on_next([=](QSize size, QSize inner) {
				label->move(
					(size.width() - inner.width()) / 2,
					(size.height() - inner.height()) / 2);
			}, label->lifetime());
			delete base::take(*child);
			*child = content;
		} else {
			return;
		}
		(*child)->show();

		inner->widthValue() | rpl::on_next([=](int width) {
			(*child)->resizeToWidth(width);
		}, (*child)->lifetime());

		(*child)->heightValue() | rpl::on_next([=](int height) {
			inner->resize(inner->width(), height + st::contactSkip);
		}, inner->lifetime());
	}, inner->lifetime());

	raw->finishAnimating();
	return result;
}

Chatbots::Chatbots(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent, controller)
, _bottomSkipRounding(st::boxRadius, st::boxDividerBg) {
	setupContent();
}

Chatbots::~Chatbots() {
	if (!Core::Quitting()) {
		save();
	}
}

bool Chatbots::closeByOutsideClick() const {
	return false;
}

rpl::producer<QString> Chatbots::title() {
	return tr::lng_chat_automation_title();
}

const Ui::RoundRect *Chatbots::bottomSkipRounding() const {
	return _permissionsWrap->count() ? nullptr : &_bottomSkipRounding;
}

void Chatbots::setupContent() {
	using namespace rpl::mappers;

	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);
	const auto current = controller()->session().data().chatbots().current();

	_recipients = Data::BusinessRecipients::MakeValid(current.recipients);
	_permissions = current.permissions;
	_committedBot = current.bot;
	_committedRecipients = _recipients.current();
	_committedPermissions = _permissions.current();

	AddDividerTextWithLottie(content, {
		.lottie = u"settings/chat_automation"_q,
		.lottieSize = st::settingsCloudPasswordIconSize,
		.lottieMargins = st::peerAppearanceIconPadding,
		.showFinished = showFinishes(),
		.about = tr::lng_chat_automation_about(tr::marked),
		.aboutMargins = st::peerAppearanceCoverLabelMargin,
	});

	const auto username = content->add(
		object_ptr<Ui::InputField>(
			content,
			st::settingsChatbotsUsername,
			tr::lng_chatbots_placeholder(),
			(current.bot
				? current.bot->session().createInternalLink(
					current.bot->username())
				: QString())),
		st::settingsChatbotsUsernameMargins);

	_usernameValue = DebouncedValue(username);
	_botValue = rpl::single(BotState{
		current.bot,
		current.bot ? LookupState::Ready : LookupState::Empty
	}) | rpl::then(
		LookupBot(&controller()->session(), _usernameValue.changes())
	);

	const auto resetBot = [=] {
		username->setText(QString());
		username->setFocus();
		_committedBot = nullptr;
		_recipients = Data::BusinessRecipients::MakeValid({});
		_permissions = Defaults();
		_committedRecipients = _recipients.current();
		_committedPermissions = _permissions.current();
	};
	const auto addBot = [=] {
		const auto resolved = _botValue.current().bot;
		if (!resolved) {
			return;
		}
		_committedBot = resolved;
		_committedRecipients = _recipients.current();
		_committedPermissions = _resolvePermissions();
	};
	auto stateAndKind = rpl::combine(
		_botValue.value(),
		_committedBot.value()
	) | rpl::map([](BotState state, UserData *committed) {
		const auto kind = (state.bot && state.bot == committed)
			? PreviewActionKind::Remove
			: PreviewActionKind::Add;
		return std::make_pair(state, kind);
	});

	content->add(object_ptr<Ui::SlideWrap<Ui::RpWidget>>(
		content,
		MakeBotPreview(
			content,
			std::move(stateAndKind),
			resetBot,
			addBot)));

	Ui::AddDividerText(
		content,
		tr::lng_chat_automation_add_about(),
		st::peerAppearanceDividerTextMargin);

	_detailsWrap = content->add(object_ptr<Ui::VerticalLayout>(content));

	AddBusinessRecipientsSelector(_detailsWrap, {
		.controller = controller(),
		.title = tr::lng_chatbots_access_title(),
		.data = &_recipients,
		.type = Data::BusinessRecipientsType::Bots,
	});

	Ui::AddSkip(_detailsWrap, st::settingsChatbotsAccessSkip);
	Ui::AddDividerText(
		_detailsWrap,
		tr::lng_chatbots_exclude_about(),
		st::peerAppearanceDividerTextMargin,
		st::defaultDividerLabel,
		RectPart::Top);

	_permissionsWrap = _detailsWrap->add(
		object_ptr<Ui::VerticalLayout>(_detailsWrap));

	refreshDetails();
	rpl::merge(
		_committedBot.changes() | rpl::to_empty,
		_botValue.changes() | rpl::to_empty
	) | rpl::on_next([=] {
		refreshDetails();
	}, lifetime());

	Ui::ResizeFitChild(this, content);
}

void Chatbots::refreshDetails() {
	_resolvePermissions = [=] {
		return Data::ChatbotsPermissions();
	};
	while (_permissionsWrap->count()) {
		delete _permissionsWrap->widgetAt(0);
	}

	const auto bot = _botValue.current().bot
		? _botValue.current().bot
		: _committedBot.current();
	if (!bot) {
		_permissionsWrap->resizeToWidth(width());
		return;
	}

	const auto content = _permissionsWrap;
	Ui::AddSkip(content);
	Ui::AddSubsectionTitle(content, tr::lng_chatbots_permissions_title());

	auto permissions = CreateEditChatbotPermissions(
		content,
		_permissions.current());
	content->add(std::move(permissions.widget));
	_resolvePermissions = permissions.value;

	std::move(
		permissions.changes
	) | rpl::on_next([=](Data::ChatbotsPermissions now) {
		const auto warn = [&](tr::phrase<lngtag_bot> text) {
			controller()->show(Ui::MakeInformBox({
				.text = text(
					tr::now,
					lt_bot,
					tr::bold(bot->name()),
					tr::rich),
				.title = tr::lng_chatbots_warning_title(),
			}));
		};

		const auto was = _permissions.current();
		const auto diff = now ^ was;
		const auto enabled = diff & now;
		using Flag = Data::ChatbotsPermission;
		if (enabled & (Flag::TransferGifts | Flag::SellGifts)) {
			if (enabled & Flag::TransferStars) {
				warn(tr::lng_chatbots_warning_both_text);
			} else {
				warn(tr::lng_chatbots_warning_gifts_text);
			}
		} else if (enabled & Flag::TransferStars) {
			warn(tr::lng_chatbots_warning_stars_text);
		} else if (enabled & Flag::EditUsername) {
			warn(tr::lng_chatbots_warning_username_text);
		}
		_permissions = now;
	}, lifetime());

	Ui::AddSkip(content);

	_permissionsWrap->resizeToWidth(width());
}

void Chatbots::save() {
	const auto show = controller()->uiShow();
	const auto fail = [=](QString error) {
		if (error == u"BUSINESS_RECIPIENTS_EMPTY"_q) {
			show->showToast(tr::lng_greeting_recipients_empty(tr::now));
		} else if (error == u"BOT_BUSINESS_MISSING"_q) {
			show->showToast(tr::lng_chatbots_not_supported(tr::now));
		}
	};
	controller()->session().data().chatbots().save({
		.bot = _committedBot.current(),
		.recipients = _committedRecipients,
		.permissions = _committedPermissions,
	}, [=] {
	}, fail);
}

} // namespace

Type ChatbotsId() {
	return Chatbots::Id();
}

} // namespace Settings
