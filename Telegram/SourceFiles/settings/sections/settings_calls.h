/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "settings/settings_common_session.h"
#include "settings/settings_type.h"

namespace style {
struct Checkbox;
struct Radio;
} // namespace style

namespace Webrtc {
class VideoTrack;
} // namespace Webrtc

namespace Ui {
class GenericBox;
class Show;
class VerticalLayout;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace Settings {

class Calls : public Section<Calls> {
public:
	Calls(QWidget *parent, not_null<Window::SessionController*> controller);
	~Calls();

	[[nodiscard]] rpl::producer<QString> title() override;

	void sectionSaveChanges(FnMut<void()> done) override;

	static Webrtc::VideoTrack *AddCameraSubsection(
		std::shared_ptr<Ui::Show> show,
		not_null<Ui::VerticalLayout*> content,
		bool saveToSettings);

private:
	void setupContent();
	void requestPermissionAndStartTestingMicrophone();

	rpl::event_stream<QString> _cameraNameStream;
	rpl::variable<bool> _testingMicrophone;

};

inline constexpr auto kMicTestUpdateInterval = crl::time(100);
inline constexpr auto kMicTestAnimationDuration = crl::time(200);

[[nodiscard]] rpl::producer<QString> PlaybackDeviceNameValue(
	rpl::producer<QString> id);
[[nodiscard]] rpl::producer<QString> CaptureDeviceNameValue(
	rpl::producer<QString> id);
[[nodiscard]] rpl::producer<QString> CameraDeviceNameValue(
	rpl::producer<QString> id);
[[nodiscard]] object_ptr<Ui::GenericBox> ChoosePlaybackDeviceBox(
	rpl::producer<QString> currentId,
	Fn<void(QString id)> chosen,
	const style::Checkbox *st = nullptr,
	const style::Radio *radioSt = nullptr);
[[nodiscard]] object_ptr<Ui::GenericBox> ChooseCaptureDeviceBox(
	rpl::producer<QString> currentId,
	Fn<void(QString id)> chosen,
	const style::Checkbox *st = nullptr,
	const style::Radio *radioSt = nullptr);
[[nodiscard]] object_ptr<Ui::GenericBox> ChooseCameraDeviceBox(
	rpl::producer<QString> currentId,
	Fn<void(QString id)> chosen,
	const style::Checkbox *st = nullptr,
	const style::Radio *radioSt = nullptr);

namespace Builder {

extern SectionBuildMethod CallsSection;

} // namespace Builder
} // namespace Settings
