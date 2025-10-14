/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Calls {
class GroupCall;
} // namespace Calls

namespace Calls::Group {
class Members;
class Viewport;
class MessagesUi;
} // namespace Calls::Group

namespace ChatHelpers {
class Show;
} // namespace ChatHelpers

namespace Data {
class GroupCall;
} // namespace Data

namespace Ui::GL {
enum class Backend;
} // namespace Ui::GL

namespace Media::View {

class VideoStream final {
public:
	VideoStream(
		not_null<QWidget*> parent,
		Ui::GL::Backend backend,
		std::shared_ptr<ChatHelpers::Show> show,
		std::shared_ptr<Data::GroupCall> call,
		QString callLinkSlug,
		MsgId callJoinMessageId);
	~VideoStream();

	[[nodiscard]] not_null<Calls::GroupCall*> call() const;

	void updateGeometry(int x, int y, int width, int height);

private:
	class Delegate;

	void setupVideo();
	void setupMembers();
	void setupMessages();

	std::shared_ptr<ChatHelpers::Show> _show;
	std::unique_ptr<Delegate> _delegate;
	std::unique_ptr<Calls::GroupCall> _call;
	std::unique_ptr<Calls::Group::Members> _members;
	std::unique_ptr<Calls::Group::Viewport> _viewport;
	std::unique_ptr<Calls::Group::MessagesUi> _messages;

};

} // namespace Media::View
