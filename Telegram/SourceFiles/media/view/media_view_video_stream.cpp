/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "media/view/media_view_video_stream.h"

#include "calls/group/calls_group_call.h"
#include "calls/group/calls_group_common.h"
#include "calls/group/calls_group_members.h"
#include "calls/group/calls_group_messages.h"
#include "calls/group/calls_group_messages_ui.h"
#include "calls/group/calls_group_viewport.h"
#include "calls/calls_instance.h"
#include "chat_helpers/compose/compose_show.h"
#include "core/application.h"

namespace Media::View {

class VideoStream::Delegate final : public Calls::GroupCall::Delegate {
public:
	Delegate();

	void groupCallFinished(not_null<Calls::GroupCall*> call) override;
	void groupCallFailed(not_null<Calls::GroupCall*> call) override;
	void groupCallRequestPermissionsOrFail(Fn<void()> onSuccess) override;
	void groupCallPlaySound(GroupCallSound sound) override;
	auto groupCallGetVideoCapture(const QString &deviceId)
		-> std::shared_ptr<tgcalls::VideoCaptureInterface> override;
	FnMut<void()> groupCallAddAsyncWaiter() override;

private:

};

VideoStream::Delegate::Delegate() {
}

void VideoStream::Delegate::groupCallFinished(
		not_null<Calls::GroupCall*> call) {
	// #TODO videostreams
}

void VideoStream::Delegate::groupCallFailed(not_null<Calls::GroupCall*> call) {
	// #TODO videostreams
}

void VideoStream::Delegate::groupCallRequestPermissionsOrFail(
		Fn<void()> onSuccess) {
}

void VideoStream::Delegate::groupCallPlaySound(GroupCallSound sound) {
}

auto VideoStream::Delegate::groupCallGetVideoCapture(const QString &deviceId)
-> std::shared_ptr<tgcalls::VideoCaptureInterface> {
	return nullptr;
}

FnMut<void()> VideoStream::Delegate::groupCallAddAsyncWaiter() {
	// #TODO videostreams
	return [] {};
}

VideoStream::VideoStream(
	not_null<QWidget*> parent,
	Ui::GL::Backend backend,
	std::shared_ptr<ChatHelpers::Show> show,
	std::shared_ptr<Data::GroupCall> call,
	QString callLinkSlug,
	MsgId callJoinMessageId)
: _show(std::move(show))
, _delegate(std::make_unique<Delegate>())
, _call(std::make_unique<Calls::GroupCall>(
	_delegate.get(),
	Calls::StartConferenceInfo{
		.show = _show,
		.call = std::move(call),
		.linkSlug = callLinkSlug,
		.joinMessageId = callJoinMessageId,
	}))
, _members(
	std::make_unique<Calls::Group::Members>(
		parent,
		_call.get(),
		Calls::Group::PanelMode::VideoStream,
		backend))
, _viewport(
	std::make_unique<Calls::Group::Viewport>(
		parent,
		Calls::Group::PanelMode::VideoStream,
		backend))
, _messages(
	std::make_unique<Calls::Group::MessagesUi>(
		parent,
		_show,
		Calls::Group::MessagesMode::VideoStream,
		_call->messages()->listValue(),
		_call->messages()->idUpdates(),
		_call->messagesEnabledValue())) {
	Core::App().calls().registerVideoStream(_call.get());
	setupMembers();
	setupVideo();
	setupMessages();
}

VideoStream::~VideoStream() {
}

not_null<Calls::GroupCall*> VideoStream::call() const {
	return _call.get();
}

void VideoStream::updateGeometry(int x, int y, int width, int height) {
	_viewport->setGeometry(false, { x, y, width, height });
	_messages->move(x, y + height, width, height);
}

void VideoStream::setupMembers() {
}

void VideoStream::setupVideo() {
	const auto setupTile = [=](
			const Calls::VideoEndpoint &endpoint,
			const std::unique_ptr<Calls::GroupCall::VideoTrack> &track) {
		using namespace rpl::mappers;
		const auto row = endpoint.rtmp()
			? _members->rtmpFakeRow(Calls::GroupCall::TrackPeer(track)).get()
			: _members->lookupRow(Calls::GroupCall::TrackPeer(track));
		Assert(row != nullptr);

		auto pinned = rpl::combine(
			_call->videoEndpointLargeValue(),
			_call->videoEndpointPinnedValue()
		) | rpl::map(_1 == endpoint && _2);
		const auto self = (endpoint.peer == _call->joinAs());
		_viewport->add(
			endpoint,
			Calls::Group::VideoTileTrack{
				Calls::GroupCall::TrackPointer(track),
				row,
			},
			Calls::GroupCall::TrackSizeValue(track),
			std::move(pinned),
			self);
	};
	for (const auto &[endpoint, track] : _call->activeVideoTracks()) {
		setupTile(endpoint, track);
	}
	_call->videoStreamActiveUpdates(
	) | rpl::start_with_next([=](const Calls::VideoStateToggle &update) {
		if (update.value) {
			// Add async (=> the participant row is definitely in Members).
			const auto endpoint = update.endpoint;
			crl::on_main(_viewport->widget(), [=] {
				const auto &tracks = _call->activeVideoTracks();
				const auto i = tracks.find(endpoint);
				if (i != end(tracks)) {
					setupTile(endpoint, i->second);
				}
			});
		} else {
			// Remove sync.
			_viewport->remove(update.endpoint);
		}
	}, _viewport->lifetime());

	_viewport->pinToggled(
	) | rpl::start_with_next([=](bool pinned) {
		_call->pinVideoEndpoint(pinned
			? _call->videoEndpointLarge()
			: Calls::VideoEndpoint{});
	}, _viewport->lifetime());

	_viewport->clicks(
	) | rpl::start_with_next([=](Calls::VideoEndpoint &&endpoint) {
		if (_call->videoEndpointLarge() == endpoint) {
			_call->showVideoEndpointLarge({});
		} else if (_call->videoEndpointPinned()) {
			_call->pinVideoEndpoint(std::move(endpoint));
		} else {
			_call->showVideoEndpointLarge(std::move(endpoint));
		}
	}, _viewport->lifetime());

	_viewport->qualityRequests(
	) | rpl::start_with_next([=](const Calls::VideoQualityRequest &request) {
		_call->requestVideoQuality(request.endpoint, request.quality);
	}, _viewport->lifetime());

	_viewport->widget()->lower();
}

void VideoStream::setupMessages() {

}

} // namespace Media::View
