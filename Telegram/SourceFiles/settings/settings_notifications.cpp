/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "settings/settings_notifications.h"

#include "settings/builder/settings_notifications_builder.h"
#include "settings/settings_notifications_common.h"
#include "settings/settings_notifications_type.h"
#include "ui/boxes/confirm_box.h"
#include "ui/chat/chat_theme.h"
#include "ui/controls/chat_service_checkbox.h"
#include "ui/effects/animations.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "core/application.h"
#include "data/data_session.h"
#include "data/notify/data_notify_settings.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "mainwindow.h"
#include "window/notifications_manager.h"
#include "window/section_widget.h"
#include "window/themes/window_theme.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_chat.h"
#include "styles/style_dialogs.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "styles/style_window.h"

#include <QSvgRenderer>

namespace Settings {

using ChangeType = Window::Notifications::ChangeType;

int CurrentNotificationsCount() {
	return std::clamp(
		Core::App().settings().notificationsCount(),
		1,
		kMaxNotificationsCount);
}

class NotificationsCount::SampleWidget : public QWidget {
public:
	SampleWidget(NotificationsCount *owner, const QPixmap &cache);

	void detach();
	void showFast();
	void hideFast();

protected:
	void paintEvent(QPaintEvent *e) override;

private:
	void startAnimation();
	void animationCallback();

	NotificationsCount *_owner;
	QPixmap _cache;
	Ui::Animations::Simple _opacity;
	bool _hiding = false;

};

void AddTypeButton(
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*> controller,
		Data::DefaultNotify type,
		Fn<void(Type)> showOther) {
	using Type = Data::DefaultNotify;
	auto label = [&] {
		switch (type) {
		case Type::User: return tr::lng_notification_private_chats();
		case Type::Group: return tr::lng_notification_groups();
		case Type::Broadcast: return tr::lng_notification_channels();
		}
		Unexpected("Type value in AddTypeButton.");
	}();
	const auto icon = [&] {
		switch (type) {
		case Type::User: return &st::menuIconProfile;
		case Type::Group: return &st::menuIconGroups;
		case Type::Broadcast: return &st::menuIconChannel;
		}
		Unexpected("Type value in AddTypeButton.");
	}();
	const auto button = AddButtonWithIcon(
		container,
		std::move(label),
		st::settingsNotificationType,
		{ icon });
	button->setClickedCallback([=] {
		showOther(NotificationsType::Id(type));
	});

	const auto session = &controller->session();
	const auto settings = &session->data().notifySettings();
	const auto &st = st::settingsNotificationType;
	auto status = rpl::combine(
		NotificationsEnabledForTypeValue(session, type),
		rpl::single(
			type
		) | rpl::then(settings->exceptionsUpdates(
		) | rpl::filter(rpl::mappers::_1 == type))
	) | rpl::map([=](bool enabled, const auto &) {
		const auto count = int(settings->exceptions(type).size());
		return !count
			? tr::lng_notification_click_to_change()
			: (enabled
				? tr::lng_notification_on
				: tr::lng_notification_off)(
					lt_exceptions,
					tr::lng_notification_exceptions(
						lt_count,
						rpl::single(float64(count))));
	}) | rpl::flatten_latest();
	const auto details = Ui::CreateChild<Ui::FlatLabel>(
		button.get(),
		std::move(status),
		st::settingsNotificationTypeDetails);
	details->show();
	details->moveToLeft(
		st.padding.left(),
		st.padding.top() + st.height - details->height());
	details->setAttribute(Qt::WA_TransparentForMouseEvents);

	const auto toggleButton = Ui::CreateChild<Ui::SettingsButton>(
		container.get(),
		nullptr,
		st);
	const auto checkView = button->lifetime().make_state<Ui::ToggleView>(
		st.toggle,
		NotificationsEnabledForType(session, type),
		[=] { toggleButton->update(); });

	const auto separator = Ui::CreateChild<Ui::RpWidget>(container.get());
	separator->paintRequest(
	) | rpl::on_next([=, bg = st.textBgOver] {
		auto p = QPainter(separator);
		p.fillRect(separator->rect(), bg);
	}, separator->lifetime());
	const auto separatorHeight = st.height - 2 * st.toggle.border;
	button->geometryValue(
	) | rpl::on_next([=](const QRect &r) {
		const auto w = st::rightsButtonToggleWidth;
		toggleButton->setGeometry(
			r.x() + r.width() - w,
			r.y(),
			w,
			r.height());
		separator->setGeometry(
			toggleButton->x() - st::lineWidth,
			r.y() + (r.height() - separatorHeight) / 2,
			st::lineWidth,
			separatorHeight);
	}, toggleButton->lifetime());

	const auto checkWidget = Ui::CreateChild<Ui::RpWidget>(toggleButton);
	checkWidget->resize(checkView->getSize());
	checkWidget->paintRequest(
	) | rpl::on_next([=] {
		auto p = QPainter(checkWidget);
		checkView->paint(p, 0, 0, checkWidget->width());
	}, checkWidget->lifetime());
	toggleButton->sizeValue(
	) | rpl::on_next([=](const QSize &s) {
		checkWidget->moveToRight(
			st.toggleSkip,
			(s.height() - checkWidget->height()) / 2);
	}, toggleButton->lifetime());

	const auto toggle = crl::guard(toggleButton, [=] {
		const auto enabled = !checkView->checked();
		checkView->setChecked(enabled, anim::type::normal);
		settings->defaultUpdate(type, Data::MuteValue{
			.unmute = enabled,
			.forever = !enabled,
		});
	});
	toggleButton->clicks(
	) | rpl::on_next([=] {
		const auto count = int(settings->exceptions(type).size());
		if (!count) {
			toggle();
		} else {
			controller->show(Box([=](not_null<Ui::GenericBox*> box) {
				const auto phrase = [&] {
					switch (type) {
					case Type::User:
						return tr::lng_notification_about_private_chats;
					case Type::Group:
						return tr::lng_notification_about_groups;
					case Type::Broadcast:
						return tr::lng_notification_about_channels;
					}
					Unexpected("Type in AddTypeButton.");
				}();
				Ui::ConfirmBox(box, {
					.text = phrase(
						lt_count,
						rpl::single(float64(count)),
						tr::rich),
					.confirmed = [=](auto close) { toggle(); close(); },
					.confirmText = tr::lng_box_ok(),
					.title = tr::lng_notification_exceptions_title(),
					.inform = true,
				});
				box->addLeftButton(
					tr::lng_notification_exceptions_view(),
					[=] {
						box->closeBox();
						showOther(NotificationsType::Id(type));
					});
			}));
		}
	}, toggleButton->lifetime());
}

NotificationsCount::NotificationsCount(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: _controller(controller)
, _chosenCorner(Core::App().settings().notificationsCorner())
, _oldCount(CurrentNotificationsCount()) {
	setMouseTracking(true);

	_sampleOpacities.resize(kMaxNotificationsCount);

	prepareNotificationSampleSmall();
	prepareNotificationSampleLarge();
}

void NotificationsCount::paintEvent(QPaintEvent *e) {
	Painter p(this);

	auto contentLeft = getContentLeft();

	auto screenRect = getScreenRect();
	p.fillRect(
		screenRect.x(),
		screenRect.y(),
		st::notificationsBoxScreenSize.width(),
		st::notificationsBoxScreenSize.height(),
		st::notificationsBoxScreenBg);

	auto monitorTop = 0;
	st::notificationsBoxMonitor.paint(p, contentLeft, monitorTop, width());

	for (int corner = 0; corner != 4; ++corner) {
		auto screenCorner = static_cast<ScreenCorner>(corner);
		auto isLeft = Core::Settings::IsLeftCorner(screenCorner);
		auto isTop = Core::Settings::IsTopCorner(screenCorner);
		auto sampleLeft = isLeft ? (screenRect.x() + st::notificationsSampleSkip) : (screenRect.x() + screenRect.width() - st::notificationsSampleSkip - st::notificationSampleSize.width());
		auto sampleTop = isTop ? (screenRect.y() + st::notificationsSampleTopSkip) : (screenRect.y() + screenRect.height() - st::notificationsSampleBottomSkip - st::notificationSampleSize.height());
		if (corner == static_cast<int>(_chosenCorner)) {
			auto count = _oldCount;
			for (int i = 0; i != kMaxNotificationsCount; ++i) {
				auto opacity = _sampleOpacities[i].value((i < count) ? 1. : 0.);
				p.setOpacity(opacity);
				p.drawPixmapLeft(sampleLeft, sampleTop, width(), _notificationSampleSmall);
				sampleTop += (isTop ? 1 : -1) * (st::notificationSampleSize.height() + st::notificationsSampleMargin);
			}
			p.setOpacity(1.);
		} else {
			p.setOpacity(st::notificationSampleOpacity);
			p.drawPixmapLeft(sampleLeft, sampleTop, width(), _notificationSampleSmall);
			p.setOpacity(1.);
		}
	}
}

void NotificationsCount::setCount(int count) {
	auto moreSamples = (count > _oldCount);
	auto from = moreSamples ? 0. : 1.;
	auto to = moreSamples ? 1. : 0.;
	auto indexDelta = moreSamples ? 1 : -1;
	auto animatedDelta = moreSamples ? 0 : -1;
	for (; _oldCount != count; _oldCount += indexDelta) {
		_sampleOpacities[_oldCount + animatedDelta].start([this] { update(); }, from, to, st::notifyFastAnim);
	}

	if (count != Core::App().settings().notificationsCount()) {
		Core::App().settings().setNotificationsCount(count);
		Core::App().saveSettingsDelayed();
		Core::App().notifications().notifySettingsChanged(
			ChangeType::MaxCount);
	}
}

int NotificationsCount::getContentLeft() const {
	return (width() - st::notificationsBoxMonitor.width()) / 2;
}

QRect NotificationsCount::getScreenRect() const {
	return getScreenRect(width());
}

QRect NotificationsCount::getScreenRect(int width) const {
	auto screenLeft = (width - st::notificationsBoxScreenSize.width()) / 2;
	auto screenTop = st::notificationsBoxScreenTop;
	return QRect(screenLeft, screenTop, st::notificationsBoxScreenSize.width(), st::notificationsBoxScreenSize.height());
}

int NotificationsCount::resizeGetHeight(int newWidth) {
	update();
	return st::notificationsBoxMonitor.height();
}

void NotificationsCount::prepareNotificationSampleSmall() {
	auto width = st::notificationSampleSize.width();
	auto height = st::notificationSampleSize.height();
	auto sampleImage = QImage(
		QSize(width, height) * style::DevicePixelRatio(),
		QImage::Format_ARGB32_Premultiplied);
	sampleImage.setDevicePixelRatio(style::DevicePixelRatio());
	sampleImage.fill(st::notificationBg->c);
	{
		Painter p(&sampleImage);
		PainterHighQualityEnabler hq(p);

		p.setPen(Qt::NoPen);

		auto padding = height / 8;
		auto userpicSize = height - 2 * padding;
		p.setBrush(st::notificationSampleUserpicFg);
		p.drawEllipse(style::rtlrect(padding, padding, userpicSize, userpicSize, width));

		auto rowLeft = height;
		auto rowHeight = padding;
		auto nameTop = (height - 5 * padding) / 2;
		auto nameWidth = height;
		p.setBrush(st::notificationSampleNameFg);
		p.drawRoundedRect(style::rtlrect(rowLeft, nameTop, nameWidth, rowHeight, width), rowHeight / 2, rowHeight / 2);

		auto rowWidth = (width - rowLeft - 3 * padding);
		auto rowTop = nameTop + rowHeight + padding;
		p.setBrush(st::notificationSampleTextFg);
		p.drawRoundedRect(style::rtlrect(rowLeft, rowTop, rowWidth, rowHeight, width), rowHeight / 2, rowHeight / 2);
		rowTop += rowHeight + padding;
		p.drawRoundedRect(style::rtlrect(rowLeft, rowTop, rowWidth, rowHeight, width), rowHeight / 2, rowHeight / 2);

		auto closeLeft = width - 2 * padding;
		p.fillRect(style::rtlrect(closeLeft, padding, padding, padding, width), st::notificationSampleCloseFg);
	}
	_notificationSampleSmall = Ui::PixmapFromImage(std::move(sampleImage));
	_notificationSampleSmall.setDevicePixelRatio(style::DevicePixelRatio());
}

void NotificationsCount::prepareNotificationSampleUserpic() {
	if (_notificationSampleUserpic.isNull()) {
		_notificationSampleUserpic = Ui::PixmapFromImage(
			Window::LogoNoMargin().scaled(
				st::notifyPhotoSize * style::DevicePixelRatio(),
				st::notifyPhotoSize * style::DevicePixelRatio(),
				Qt::IgnoreAspectRatio,
				Qt::SmoothTransformation));
		_notificationSampleUserpic.setDevicePixelRatio(
			style::DevicePixelRatio());
	}
}

void NotificationsCount::prepareNotificationSampleLarge() {
	int w = st::notifyWidth, h = st::notifyMinHeight;
	auto sampleImage = QImage(
		w * style::DevicePixelRatio(),
		h * style::DevicePixelRatio(),
		QImage::Format_ARGB32_Premultiplied);
	sampleImage.setDevicePixelRatio(style::DevicePixelRatio());
	sampleImage.fill(st::notificationBg->c);
	{
		Painter p(&sampleImage);
		p.fillRect(0, 0, w - st::notifyBorderWidth, st::notifyBorderWidth, st::notifyBorder->b);
		p.fillRect(w - st::notifyBorderWidth, 0, st::notifyBorderWidth, h - st::notifyBorderWidth, st::notifyBorder->b);
		p.fillRect(st::notifyBorderWidth, h - st::notifyBorderWidth, w - st::notifyBorderWidth, st::notifyBorderWidth, st::notifyBorder->b);
		p.fillRect(0, st::notifyBorderWidth, st::notifyBorderWidth, h - st::notifyBorderWidth, st::notifyBorder->b);

		prepareNotificationSampleUserpic();
		p.drawPixmap(st::notifyPhotoPos.x(), st::notifyPhotoPos.y(), _notificationSampleUserpic);

		int itemWidth = w - st::notifyPhotoPos.x() - st::notifyPhotoSize - st::notifyTextLeft - st::notifyClosePos.x() - st::notifyClose.width;

		auto rectForName = style::rtlrect(st::notifyPhotoPos.x() + st::notifyPhotoSize + st::notifyTextLeft, st::notifyTextTop, itemWidth, st::msgNameFont->height, w);

		auto notifyText = st::dialogsTextFont->elided(tr::lng_notification_sample(tr::now), itemWidth);
		p.setFont(st::dialogsTextFont);
		p.setPen(st::dialogsTextFgService);
		p.drawText(st::notifyPhotoPos.x() + st::notifyPhotoSize + st::notifyTextLeft, st::notifyItemTop + st::msgNameFont->height + st::dialogsTextFont->ascent, notifyText);

		p.setPen(st::dialogsNameFg);
		p.setFont(st::msgNameFont);

		auto notifyTitle = st::msgNameFont->elided(u"Telegram Desktop"_q, rectForName.width());
		p.drawText(rectForName.left(), rectForName.top() + st::msgNameFont->ascent, notifyTitle);

		st::notifyClose.icon.paint(p, w - st::notifyClosePos.x() - st::notifyClose.width + st::notifyClose.iconPosition.x(), st::notifyClosePos.y() + st::notifyClose.iconPosition.y(), w);
	}

	_notificationSampleLarge = Ui::PixmapFromImage(std::move(sampleImage));
}

void NotificationsCount::removeSample(SampleWidget *widget) {
	for (auto &samples : _cornerSamples) {
		for (int i = 0, size = samples.size(); i != size; ++i) {
			if (samples[i] == widget) {
				for (int j = i + 1; j != size; ++j) {
					samples[j]->detach();
				}
				samples.resize(i);
				break;
			}
		}
	}
}

void NotificationsCount::mouseMoveEvent(QMouseEvent *e) {
	auto screenRect = getScreenRect();
	auto cornerWidth = screenRect.width() / 3;
	auto cornerHeight = screenRect.height() / 3;
	auto topLeft = style::rtlrect(screenRect.x(), screenRect.y(), cornerWidth, cornerHeight, width());
	auto topRight = style::rtlrect(screenRect.x() + screenRect.width() - cornerWidth, screenRect.y(), cornerWidth, cornerHeight, width());
	auto bottomRight = style::rtlrect(screenRect.x() + screenRect.width() - cornerWidth, screenRect.y() + screenRect.height() - cornerHeight, cornerWidth, cornerHeight, width());
	auto bottomLeft = style::rtlrect(screenRect.x(), screenRect.y() + screenRect.height() - cornerHeight, cornerWidth, cornerHeight, width());
	if (topLeft.contains(e->pos())) {
		setOverCorner(ScreenCorner::TopLeft);
	} else if (topRight.contains(e->pos())) {
		setOverCorner(ScreenCorner::TopRight);
	} else if (bottomRight.contains(e->pos())) {
		setOverCorner(ScreenCorner::BottomRight);
	} else if (bottomLeft.contains(e->pos())) {
		setOverCorner(ScreenCorner::BottomLeft);
	} else {
		clearOverCorner();
	}
}

void NotificationsCount::leaveEventHook(QEvent *e) {
	clearOverCorner();
}

void NotificationsCount::setOverCorner(ScreenCorner corner) {
	if (_isOverCorner) {
		if (corner == _overCorner) {
			return;
		}
		const auto index = static_cast<int>(_overCorner);
		for (const auto widget : _cornerSamples[index]) {
			widget->hideFast();
		}
	} else {
		_isOverCorner = true;
		setCursor(style::cur_pointer);
		Core::App().notifications().notifySettingsChanged(
			ChangeType::DemoIsShown);
	}
	_overCorner = corner;

	auto &samples = _cornerSamples[static_cast<int>(_overCorner)];
	auto samplesAlready = int(samples.size());
	auto samplesNeeded = _oldCount;
	auto samplesLeave = qMin(samplesAlready, samplesNeeded);
	for (int i = 0; i != samplesLeave; ++i) {
		samples[i]->showFast();
	}
	if (samplesNeeded > samplesLeave) {
		const auto r = Window::Notifications::NotificationDisplayRect(
			&_controller->window());
		auto isLeft = Core::Settings::IsLeftCorner(_overCorner);
		auto isTop = Core::Settings::IsTopCorner(_overCorner);
		auto sampleLeft = (isLeft == rtl()) ? (r.x() + r.width() - st::notifyWidth - st::notifyDeltaX) : (r.x() + st::notifyDeltaX);
		auto sampleTop = isTop ? (r.y() + st::notifyDeltaY) : (r.y() + r.height() - st::notifyDeltaY - st::notifyMinHeight);
		for (int i = samplesLeave; i != samplesNeeded; ++i) {
			auto widget = std::make_unique<SampleWidget>(this, _notificationSampleLarge);
			widget->move(sampleLeft, sampleTop + (isTop ? 1 : -1) * i * (st::notifyMinHeight + st::notifyDeltaY));
			widget->showFast();
			samples.push_back(widget.release());
		}
	} else {
		for (int i = samplesLeave; i != samplesAlready; ++i) {
			samples[i]->hideFast();
		}
	}
}

void NotificationsCount::clearOverCorner() {
	if (_isOverCorner) {
		_isOverCorner = false;
		setCursor(style::cur_default);
		Core::App().notifications().notifySettingsChanged(
			ChangeType::DemoIsHidden);

		for (const auto &samples : _cornerSamples) {
			for (const auto widget : samples) {
				widget->hideFast();
			}
		}
	}
}

void NotificationsCount::mousePressEvent(QMouseEvent *e) {
	_isDownCorner = _isOverCorner;
	_downCorner = _overCorner;
}

void NotificationsCount::mouseReleaseEvent(QMouseEvent *e) {
	auto isDownCorner = base::take(_isDownCorner);
	if (isDownCorner
		&& _isOverCorner
		&& _downCorner == _overCorner
		&& _downCorner != _chosenCorner) {
		_chosenCorner = _downCorner;
		update();

		if (_chosenCorner != Core::App().settings().notificationsCorner()) {
			Core::App().settings().setNotificationsCorner(_chosenCorner);
			Core::App().saveSettingsDelayed();
			Core::App().notifications().notifySettingsChanged(
				ChangeType::Corner);
		}
	}
}

NotificationsCount::~NotificationsCount() {
	for (const auto &samples : _cornerSamples) {
		for (const auto widget : samples) {
			widget->detach();
		}
	}
	clearOverCorner();
}

NotificationsCount::SampleWidget::SampleWidget(
	NotificationsCount *owner,
	const QPixmap &cache)
: _owner(owner)
, _cache(cache) {
	setFixedSize(
		cache.width() / cache.devicePixelRatio(),
		cache.height() / cache.devicePixelRatio());

	setWindowFlags(Qt::WindowFlags(Qt::FramelessWindowHint)
		| Qt::WindowStaysOnTopHint
		| Qt::BypassWindowManagerHint
		| Qt::NoDropShadowWindowHint
		| Qt::Tool);
	setAttribute(Qt::WA_MacAlwaysShowToolWindow);
	setAttribute(Qt::WA_TransparentForMouseEvents);
	setAttribute(Qt::WA_OpaquePaintEvent);

	setWindowOpacity(0.);
	show();
}

void NotificationsCount::SampleWidget::detach() {
	_owner = nullptr;
	hideFast();
}

void NotificationsCount::SampleWidget::showFast() {
	_hiding = false;
	startAnimation();
}

void NotificationsCount::SampleWidget::hideFast() {
	_hiding = true;
	startAnimation();
}

void NotificationsCount::SampleWidget::paintEvent(QPaintEvent *e) {
	Painter p(this);
	p.drawPixmap(0, 0, _cache);
}

void NotificationsCount::SampleWidget::startAnimation() {
	_opacity.start(
		[=] { animationCallback(); },
		_hiding ? 1. : 0.,
		_hiding ? 0. : 1.,
		st::notifyFastAnim);
}

void NotificationsCount::SampleWidget::animationCallback() {
	setWindowOpacity(_opacity.value(_hiding ? 0. : 1.));
	if (!_opacity.animating() && _hiding) {
		if (_owner) {
			_owner->removeSample(this);
		}
		hide();
		deleteLater();
	}
}

class NotifyPreview final {
public:
	NotifyPreview(bool nameShown, bool previewShown);

	void setNameShown(bool shown);
	void setPreviewShown(bool shown);

	int resizeGetHeight(int newWidth);
	void paint(Painter &p, int x, int y);

private:
	int _width = 0;
	int _height = 0;
	bool _nameShown = false;
	bool _previewShown = false;
	Ui::RoundRect _roundRect;
	Ui::Text::String _name, _title;
	Ui::Text::String _text, _preview;
	QSvgRenderer _userpic;
	QImage _logo;

};

NotifyPreview::NotifyPreview(bool nameShown, bool previewShown)
: _nameShown(nameShown)
, _previewShown(previewShown)
, _roundRect(st::boxRadius, st::msgInBg)
, _userpic(u":/gui/icons/settings/dino.svg"_q)
, _logo(Window::LogoNoMargin()) {
	const auto ratio = style::DevicePixelRatio();
	_logo = _logo.scaledToWidth(
		st::notifyPreviewUserpicSize * ratio,
		Qt::SmoothTransformation);
	_logo.setDevicePixelRatio(ratio);

	_name.setText(
		st::defaultSubsectionTitle.style,
		tr::lng_notification_preview_title(tr::now));
	_title.setText(st::defaultSubsectionTitle.style, AppName.utf16());

	_text.setText(
		st::boxTextStyle,
		tr::lng_notification_preview_text(tr::now));
	_preview.setText(
		st::boxTextStyle,
		tr::lng_notification_preview(tr::now));
}

void NotifyPreview::setNameShown(bool shown) {
	_nameShown = shown;
}

void NotifyPreview::setPreviewShown(bool shown) {
	_previewShown = shown;
}

int NotifyPreview::resizeGetHeight(int newWidth) {
	_width = newWidth;
	_height = st::notifyPreviewUserpicPosition.y()
		+ st::notifyPreviewUserpicSize
		+ st::notifyPreviewUserpicPosition.y();
	const auto available = _width
		- st::notifyPreviewTextPosition.x()
		- st::notifyPreviewUserpicPosition.x();
	if (std::max(_text.maxWidth(), _preview.maxWidth()) >= available) {
		_height += st::defaultTextStyle.font->height;
	}
	return _height;
}

void NotifyPreview::paint(Painter &p, int x, int y) {
	if (!_width || !_height) {
		return;
	}
	p.translate(x, y);
	const auto guard = gsl::finally([&] { p.translate(-x, -y); });

	_roundRect.paint(p, { 0, 0, _width, _height });
	const auto userpic = QRect(
		st::notifyPreviewUserpicPosition,
		QSize{ st::notifyPreviewUserpicSize, st::notifyPreviewUserpicSize });

	if (_nameShown) {
		_userpic.render(&p, QRectF(userpic));
	} else {
		p.drawImage(userpic.topLeft(), _logo);
	}

	p.setPen(st::historyTextInFg);

	const auto &title = _nameShown ? _name : _title;
	title.drawElided(
		p,
		st::notifyPreviewTitlePosition.x(),
		st::notifyPreviewTitlePosition.y(),
		_width - st::notifyPreviewTitlePosition.x() - userpic.x());

	const auto &text = _previewShown ? _text : _preview;
	text.drawElided(
		p,
		st::notifyPreviewTextPosition.x(),
		st::notifyPreviewTextPosition.y(),
		_width - st::notifyPreviewTextPosition.x() - userpic.x(),
		2);
}

NotifyViewCheckboxes SetupNotifyViewOptions(
		not_null<Window::SessionController*> controller,
		not_null<Ui::VerticalLayout*> container,
		bool nameShown,
		bool previewShown) {
	using namespace rpl::mappers;

	auto wrap = container->add(object_ptr<Ui::SlideWrap<>>(
		container,
		object_ptr<Ui::RpWidget>(container)));
	const auto widget = wrap->entity();

	const auto makeCheckbox = [&](const QString &text, bool checked) {
		return Ui::MakeChatServiceCheckbox(
			widget,
			text,
			st::backgroundCheckbox,
			st::backgroundCheck,
			checked).release();
	};
	const auto name = makeCheckbox(
		tr::lng_notification_show_name(tr::now),
		nameShown);
	const auto preview = makeCheckbox(
		tr::lng_notification_show_text(tr::now),
		previewShown);

	const auto view = widget->lifetime().make_state<NotifyPreview>(
		nameShown,
		previewShown);
	using ThemePtr = std::unique_ptr<Ui::ChatTheme>;
	const auto theme = widget->lifetime().make_state<ThemePtr>(
		Window::Theme::DefaultChatThemeOn(widget->lifetime()));
	widget->widthValue(
	) | rpl::filter(
		_1 >= (st::historyMinimalWidth / 2)
	) | rpl::on_next([=](int width) {
		const auto margins = st::notifyPreviewMargins;
		const auto bubblew = width - margins.left() - margins.right();
		const auto bubbleh = view->resizeGetHeight(bubblew);
		const auto height = bubbleh + margins.top() + margins.bottom();
		widget->resize(width, height);

		const auto skip = st::notifyPreviewChecksSkip;
		const auto checksWidth = name->width() + skip + preview->width();
		const auto checksLeft = (width - checksWidth) / 2;
		const auto checksTop = height
			- (margins.bottom() + name->height()) / 2;
		name->move(checksLeft, checksTop);
		preview->move(checksLeft + name->width() + skip, checksTop);
	}, widget->lifetime());

	widget->paintRequest(
	) | rpl::on_next([=](QRect rect) {
		Painter p(widget);
		p.setClipRect(rect);
		Window::SectionWidget::PaintBackground(
			p,
			theme->get(),
			QSize(widget->width(), widget->window()->height()),
			rect);

		view->paint(
			p,
			st::notifyPreviewMargins.left(),
			st::notifyPreviewMargins.top());
	}, widget->lifetime());

	name->checkedChanges(
	) | rpl::on_next([=](bool checked) {
		view->setNameShown(checked);
		widget->update();
	}, name->lifetime());

	preview->checkedChanges(
	) | rpl::on_next([=](bool checked) {
		view->setPreviewShown(checked);
		widget->update();
	}, preview->lifetime());

	return {
		.wrap = wrap,
		.name = name,
		.preview = preview,
	};
}

Notifications::Notifications(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent) {
	setController(controller);
	setupContent();
}

rpl::producer<QString> Notifications::title() {
	return tr::lng_settings_section_notify();
}

void Notifications::setupContent() {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);
	build(content, Builder::NotificationsSection);
	Ui::ResizeFitChild(this, content);
}

} // namespace Settings
