/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/mg_settings.h"

#include "core/application.h"
#include "core/core_settings.h"

#include <rpl/event_stream.h>

namespace MG {

// Each setting is a scalar persisted through the upstream typed pref store
// (Core::Settings::readPref / writePref), with a dedicated event_stream so the
// UI can react to changes live. Toggles default to false (opt-in fork
// features).
#define MG_SETTING(Type, Name, Key, Default) \
namespace { \
[[nodiscard]] rpl::event_stream<Type> &Name##Stream() { \
	static auto result = rpl::event_stream<Type>(); \
	return result; \
} \
} /* namespace */ \
Type Name() { \
	return Core::App().settings().readPref<Type>(Key, Default); \
} \
void Set##Name(Type value) { \
	Core::App().settings().writePref<Type>(Key, value); \
	Core::App().saveSettingsDelayed(); \
	Name##Stream().fire_copy(value); \
} \
rpl::producer<Type> Name##Value() { \
	return Name##Stream().events_starting_with(Name()); \
}

#define MG_BOOL_SETTING(Name, Key) MG_SETTING(bool, Name, Key, false)

MG_BOOL_SETTING(ShowPeerId, "mg-show-peer-id")
MG_BOOL_SETTING(HideStories, "mg-hide-stories")
MG_BOOL_SETTING(DeleteForAllDefault, "mg-delete-for-all-default")
MG_BOOL_SETTING(MessageDetails, "mg-message-details")
MG_BOOL_SETTING(HideAllChats, "mg-hide-all-chats")
MG_BOOL_SETTING(DisableGlobalSearch, "mg-disable-global-search")
MG_BOOL_SETTING(DisableLinkPreviews, "mg-disable-link-previews")
MG_BOOL_SETTING(DisableAiEditor, "mg-disable-ai-editor")
MG_BOOL_SETTING(DisableAiSummaries, "mg-disable-ai-summaries")
MG_BOOL_SETTING(OpenLinksInBrowser, "mg-open-links-in-browser")
MG_BOOL_SETTING(KeepDraftsLocal, "mg-keep-drafts-local")
MG_BOOL_SETTING(ReduceTracking, "mg-reduce-tracking")
MG_BOOL_SETTING(StripTracking, "mg-strip-tracking")
MG_BOOL_SETTING(ConfirmInternalLinks, "mg-confirm-internal-links")
MG_BOOL_SETTING(HidePremiumPromo, "mg-hide-premium-promo")
MG_BOOL_SETTING(LockOnHide, "mg-lock-on-hide")
MG_BOOL_SETTING(AllRecentStickers, "mg-all-recent-stickers")
MG_BOOL_SETTING(ShowCharCounter, "mg-show-char-counter")

// A FilterId; 0 means "open the account's default folder" (upstream).
MG_SETTING(int, LaunchFolder, "mg-launch-folder", 0)

// How far up kTemporaryKeyLadder the server has pushed us; see below.
MG_SETTING(int, TemporaryKeyStep, "mg-temporary-key-step", 0)

#undef MG_BOOL_SETTING
#undef MG_SETTING

namespace {

// The lifetimes to try, shortest first. The last one is the upstream default:
// reaching it means the reduced lifetime is not what the server dislikes, so
// nothing is left to fall back to. A probe against DC2 in 2026-05 accepted an
// expires_in as low as 60 seconds, so one hour keeps a wide margin while
// asking for 24 handshakes a day instead of one.
constexpr TimeId kTemporaryKeyLadder[] = { 3600, 6 * 3600, 86400 };
constexpr auto kTemporaryKeyLadderTop = int(std::size(kTemporaryKeyLadder)) - 1;

} // namespace

TimeId TemporaryKeyExpiresIn() {
	if (!ReduceTracking()) {
		return kTemporaryKeyLadder[kTemporaryKeyLadderTop];
	}
	const auto step = std::clamp(
		TemporaryKeyStep(),
		0,
		kTemporaryKeyLadderTop);
	return kTemporaryKeyLadder[step];
}

rpl::producer<TimeId> TemporaryKeyExpiresInValue() {
	return rpl::combine(
		ReduceTrackingValue(),
		TemporaryKeyStepValue()
	) | rpl::map([](bool, int) { return TemporaryKeyExpiresIn(); });
}

bool StepUpTemporaryKeyExpiresIn() {
	if (!ReduceTracking() || TemporaryKeyStep() >= kTemporaryKeyLadderTop) {
		return false;
	}
	SetTemporaryKeyStep(TemporaryKeyStep() + 1);
	return true;
}

void ResetTemporaryKeyLadder() {
	SetTemporaryKeyStep(0);
}

} // namespace MG
