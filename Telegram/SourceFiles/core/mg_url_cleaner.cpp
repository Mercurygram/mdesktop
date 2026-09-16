/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/mg_url_cleaner.h"

#include "base/assertion.h"
#include "ui/text/text_entity.h"

#include <QtCore/QSet>
#include <QtCore/QStringList>
#include <QtCore/QUrl>

#include <vector>

namespace MG {
namespace {

// A parameter name set. An entry ending in '_' matches by prefix ("utm_"
// covers "utm_source"), anything else matches exactly.
class Rules final {
public:
	Rules(std::initializer_list<QStringView> names) {
		for (const auto &name : names) {
			if (name.endsWith(u'_')) {
				_prefixes.push_back(name.toString());
			} else {
				_exact.insert(name.toString());
			}
		}
	}

	[[nodiscard]] bool matches(const QString &name) const {
		if (_exact.contains(name)) {
			return true;
		}
		for (const auto &prefix : _prefixes) {
			if (name.startsWith(prefix)) {
				return true;
			}
		}
		return false;
	}

private:
	QSet<QString> _exact;
	std::vector<QString> _prefixes;

};

// The rules are hand-written rather than downloaded or vendored on purpose: a
// privacy option that fetches its rule list defeats itself, and the published
// lists (ClearURLs, AdGuard) are mostly domain-scoped, so flattening them into
// one global set would import their false positives too. Kept in step with the
// Android client's MgUrlCleaner.
[[nodiscard]] const Rules &GlobalRules() {
	static const auto result = Rules{
		// campaign tagging schemes
		u"utm_", u"pk_", u"mtm_", u"piwik_", u"matomo_", u"hsa_", u"vero_",
		u"oly_",
		// Google Ads
		u"gclid", u"gclsrc", u"dclid", u"gbraid", u"wbraid", u"gad_source",
		u"gad_campaignid", u"srsltid",
		// Meta. Instagram has shipped three different share ids over time.
		u"fbclid", u"fb_action_ids", u"fb_action_types", u"fb_source",
		u"fb_ref", u"igshid", u"igsh", u"igsi",
		// other ad networks
		u"msclkid", u"twclid", u"ttclid", u"yclid", u"ysclid", u"_openstat",
		u"epik", u"rdt_cid", u"li_fat_id", u"sc_cid", u"ScCid", u"erid",
		u"spm", u"scm",
		// affiliate networks
		u"irclickid", u"irgwc", u"cjevent", u"awc", u"sscid", u"clickid",
		u"affid", u"ranMID", u"ranEAID", u"ranSiteID",
		// email and marketing automation
		u"mc_cid", u"mc_eid", u"mkt_tok", u"_hsenc", u"_hsmi", u"__hssc",
		u"__hstc", u"__hsfp", u"hsCtaTracking", u"__s", u"_kx", u"wickedid",
		u"s_kwcid", u"s_cid", u"ef_id",
		// analytics leftovers
		u"_ga", u"_gl", u"_gid", u"_branch_match_id", u"_branch_referrer",
		u"adjust_tracker", u"af_siteid",
		// publisher-internal campaign ids
		u"ncid", u"cmpid", u"icid", u"intcid", u"int_source", u"ito",
		u"ftag", u"CMP",
	};
	return result;
}

// Parameters that are tracking only on their own site. "si" is the reason this
// list exists: YouTube and Spotify use it as a share id, plenty of unrelated
// sites use it as a real parameter, so stripping it everywhere silently
// corrupted links.
//
// A key containing a dot matches that host and its subdomains. A key with no
// dot matches the second-level label, for sites spread over many TLDs
// (amazon.co.uk, google.de). The latter also matches a third-party host
// carrying that label, which is acceptable for a rule that only drops query
// parameters.
struct HostRules {
	std::vector<QStringView> hosts;
	Rules rules;
};

[[nodiscard]] const std::vector<HostRules> &HostRulesList() {
	static const auto result = std::vector<HostRules>{
		{ { u"youtube.com", u"youtu.be" }, { u"si", u"pp", u"feature" } },
		{ { u"spotify.com", u"spotify.link" }, { u"si", u"nd", u"context" } },
		{ { u"twitter.com", u"x.com" },
			{ u"s", u"t", u"cxt", u"ref_src", u"ref_url" } },
		{ { u"tiktok.com" },
			{ u"_r", u"_t", u"is_from_webapp", u"sender_device",
				u"sender_web_id", u"share_app_id", u"share_link_id",
				u"share_item_id", u"tt_from", u"u_code", u"refer",
				u"is_copy_url", u"checksum" } },
		{ { u"facebook.com", u"fb.watch" },
			{ u"__tn__", u"__cft__", u"mibextid", u"rdid", u"refsrc",
				u"hc_ref", u"_rdr" } },
		// img_index picks which image of a carousel opens, so it stays.
		{ { u"instagram.com" }, { u"stkn" } },
		{ { u"reddit.com", u"redd.it" },
			{ u"share_id", u"correlation_id", u"ref", u"ref_source", u"rdt",
				u"chainedPosts" } },
		// th and psc select a product variant, so they stay.
		{ { u"amazon" },
			{ u"tag", u"ref", u"ref_", u"pd_rd_", u"pf_rd_", u"qid", u"sr",
				u"_encoding", u"linkCode", u"creative", u"creativeASIN",
				u"ascsubtag", u"dib", u"dib_tag", u"content-id",
				u"sp_csd" } },
		{ { u"aliexpress.com" },
			{ u"aff_", u"algo_pvid", u"algo_expid", u"btsid", u"ws_ab_test",
				u"pdp_npi", u"curPageLogUid", u"gatewayAdapt",
				u"terminal_id" } },
		{ { u"linkedin.com" },
			{ u"trk", u"trkInfo", u"midToken", u"midSig", u"eBP", u"refId",
				u"originalSubdomain" } },
		{ { u"google" },
			{ u"ved", u"ei", u"oq", u"gs_lcp", u"gs_lcrp", u"gs_lp",
				u"sca_esv", u"sclient", u"sourceid", u"usg", u"uact", u"cad",
				u"aqs" } },
		{ { u"yahoo.com" },
			{ u"guccounter", u"guce_referrer", u"guce_referrer_sig" } },
		{ { u"bilibili.com", u"b23.tv" },
			{ u"spm_id_from", u"vd_source", u"from_source", u"share_source",
				u"share_medium", u"share_plat", u"share_session_id",
				u"unique_k", u"buvid", u"from_spmid" } },
		{ { u"ebay" },
			{ u"_trkparms", u"_trksid", u"_from", u"mkcid", u"mkrid",
				u"campid", u"toolid", u"customid", u"mkevt" } },
		{ { u"etsy.com" },
			{ u"click_key", u"click_sum", u"ref", u"frs",
				u"organic_search_click" } },
		{ { u"medium.com" }, { u"source", u"sk" } },
		{ { u"bbc.com", u"bbc.co.uk" }, { u"at_" } },
		{ { u"nytimes.com" }, { u"smid", u"smtyp" } },
		{ { u"steampowered.com", u"steamcommunity.com" }, { u"snr" } },
		{ { u"twitch.tv" }, { u"tt_content", u"tt_medium" } },
	};
	return result;
}

[[nodiscard]] const Rules *RulesForHost(const QString &host) {
	if (host.isEmpty()) {
		return nullptr;
	}
	const auto dot = QString(QChar('.'));
	const auto dotted = dot + host.toLower();
	for (const auto &entry : HostRulesList()) {
		for (const auto &key : entry.hosts) {
			const auto hit = !key.contains(QChar('.'))
				? dotted.contains(dot + key + dot)
				: dotted.endsWith(dot + key);
			if (hit) {
				return &entry.rules;
			}
		}
	}
	return nullptr;
}

[[nodiscard]] bool IsTracking(
		const QString &name,
		const Rules *hostRules) {
	return GlobalRules().matches(name)
		|| (hostRules && hostRules->matches(name));
}

} // namespace

QString StripTrackingFromUrl(const QString &url) {
	auto parsed = QUrl::fromUserInput(url);
	const auto scheme = parsed.scheme();
	if (!parsed.isValid()
		|| (scheme != QStringLiteral("http")
			&& scheme != QStringLiteral("https"))) {
		return url;
	}
	const auto query = parsed.query(QUrl::FullyEncoded);
	if (query.isEmpty()) {
		return url;
	}
	const auto hostRules = RulesForHost(parsed.host());
	// The raw encoded query is filtered as it stands: decoding the values and
	// re-appending them would rewrite '+' as %2B, turning "?q=hello+world"
	// into a literal plus for the server.
	auto kept = QStringList();
	auto anyTracking = false;
	for (const auto &pair : query.split(u'&')) {
		const auto equals = pair.indexOf(u'=');
		const auto encoded = (equals < 0) ? pair : pair.left(equals);
		const auto name = QUrl::fromPercentEncoding(encoded.toUtf8());
		if (IsTracking(name, hostRules)) {
			anyTracking = true;
		} else {
			kept.push_back(pair);
		}
	}
	if (!anyTracking) {
		return url;
	}
	if (kept.isEmpty()) {
		parsed.setQuery(QString());
	} else {
		parsed.setQuery(kept.join(u'&'), QUrl::StrictMode);
	}
	return parsed.toString(QUrl::FullyEncoded);
}

QString StripTrackingInText(const QString &text) {
	if (text.isEmpty()) {
		return text;
	}
	auto parsed = TextWithEntities{ text };
	TextUtilities::ParseEntities(parsed, TextParseLinks);
	auto result = text;
	// Back to front, so the offsets parsing produced stay valid.
	for (auto i = parsed.entities.size(); i != 0;) {
		const auto &entity = parsed.entities[--i];
		if (entity.type() != EntityType::Url) {
			continue;
		}
		const auto original = text.mid(entity.offset(), entity.length());
		const auto cleaned = StripTrackingFromUrl(original);
		if (cleaned != original) {
			result.replace(entity.offset(), entity.length(), cleaned);
		}
	}
	return result;
}

} // namespace MG
