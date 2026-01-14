// This file is part of Telegram Desktop,
// the official desktop application for the Telegram messaging service.
//
// For license and copyright information please follow this link:
// https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
//
#include "history/view/history_view_top_peers_selector.h"

#include "apiwrap.h"
#include "main/session/session_show.h"
#include "chat_helpers/share_message_phrase_factory.h"
#include "ui/controls/dynamic_images_strip.h"
#include "ui/controls/popup_selector.h"
#include "ui/dynamic_image.h"
#include "ui/dynamic_thumbnails.h"
#include "ui/effects/animations.h"
#include "ui/rect.h"
#include "data/components/top_peers.h"
#include "history/history.h"
#include "data/data_peer.h"
#include "data/data_user.h"
#include "data/data_session.h"
#include "main/main_session.h"
#include "styles/style_chat_helpers.h"

namespace HistoryView {
namespace {

constexpr auto kMaxPeers = 5;

[[nodiscard]] std::vector<not_null<PeerData*>> CollectPeers(
		not_null<Main::Session*> session) {
	const auto user = session->user();
	auto topPeers = session->topPeers().list();
	const auto it = ranges::find(topPeers, user);
	if (it != topPeers.end()) {
		topPeers.erase(it);
	}
	auto result = std::vector<not_null<PeerData*>>();
	result.push_back(user);
	for (const auto &peer : topPeers | ranges::views::take(kMaxPeers - 1)) {
		result.push_back(peer);
	}
	return result;
}

} // namespace

void ShowTopPeersSelector(
		not_null<Ui::RpWidget*> parent,
		std::shared_ptr<Main::SessionShow> show,
		FullMsgId fullId,
		QPoint globalPos) {
	const auto session = &show->session();
	const auto peers = CollectPeers(session);
	auto thumbnails = std::vector<std::shared_ptr<Ui::DynamicImage>>();
	thumbnails.reserve(peers.size());
	for (const auto &peer : peers) {
		thumbnails.push_back(peer->isSelf()
			? Ui::MakeSavedMessagesThumbnail()
			: Ui::MakeUserpicThumbnail(peer));
	}

	const auto send = [=](not_null<PeerData*> peer) {
		if (const auto item = session->data().message(fullId)) {
			session->api().forwardMessages(
				Data::ResolvedForwardDraft{ .items = { item } },
				Api::SendAction(session->data().history(peer)),
				[=] {
					using namespace ChatHelpers;
					auto text = rpl::variable<TextWithEntities>(
						ForwardedMessagePhrase({
							.toCount = 1,
							.singleMessage = 1,
							.to1 = peer,
						})).current();
					show->showToast(std::move(text));
				});
		}
	};

	const auto contentWidth = peers.size() * st::topPeersSelectorUserpicSize
		+ (peers.size() - 1) * st::topPeersSelectorUserpicGap;
	const auto contentHeight = int(
		st::topPeersSelectorUserpicSize
			* (1. + st::topPeersSelectorUserpicExpand));
	const auto selectorWidth = contentWidth
		+ 2 * st::topPeersSelectorPadding;
	const auto selectorHeight = contentHeight
		+ 2 * st::topPeersSelectorPadding;
	const auto selector = Ui::CreateChild<Ui::PopupSelector>(
		parent,
		QSize(selectorWidth, selectorHeight));
	const auto userpicsWidget = Ui::CreateChild<Ui::DynamicImagesStrip>(
		selector,
		std::move(thumbnails),
		st::topPeersSelectorUserpicSize,
		st::topPeersSelectorUserpicGap);
	const auto margins = selector->marginsForShadow();
	const auto x = (selectorWidth - contentWidth) / 2 + margins.left();
	const auto y = (selectorHeight - contentHeight) / 2 + margins.top();
	userpicsWidget->setGeometry(
		QRect(x, y, contentWidth, contentHeight)
			+ Margins(int(st::topPeersSelectorUserpicSize
				* st::topPeersSelectorUserpicExpand)));
	userpicsWidget->setClickCallback([=](int index) {
		send(peers[index]);
		selector->hideAnimated();
	});
	userpicsWidget->setCursor(style::cur_pointer);
	selector->updateShowState(0, 0, true);
	selector->popup((!globalPos.isNull() ? globalPos : QCursor::pos())
		- QPoint(selector->width() / 2, selector->height())
		+ st::topPeersSelectorSkip);

	auto animation
		= selector->lifetime().make_state<Ui::Animations::Simple>();
	constexpr auto kShift = 0.15;
	animation->start([=](float64 value) {
		const auto userpicsProgress = std::clamp((value - kShift), 0., 1.);
		userpicsWidget->setProgress(anim::easeInQuint(1, userpicsProgress));
		value = std::clamp(value, 0., 1.);
		selector->updateShowState(value, value, true);
	}, 0., 1. + kShift, st::fadeWrapDuration * 3, anim::easeOutQuint);
}

} // namespace HistoryView
