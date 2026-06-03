/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "core/version.h"
#include "settings.h"

enum {
	MaxSelectedItems = 100,

	LocalEncryptIterCount = 4000, // key derivation iteration count
	LocalEncryptNoPwdIterCount = 4, // key derivation iteration count without pwd (not secure anyway)
	LocalEncryptSaltSize = 32, // 256 bit

	AutoSearchTimeout = 900, // 0.9 secs

	PreloadHeightsCount = 3, // when 3 screens to scroll left make a preload request

	SearchPeopleLimit = 5,

	MaxMessageSize = 4096,

	WebPageUserId = 701000,

	UpdateDelayConstPart = 8 * 3600, // 8 hour min time between update check requests
	UpdateDelayRandPart = 8 * 3600, // 8 hour max - min time between update check requests

	WrongPasscodeTimeout = 1500,

	ChoosePeerByDragTimeout = 1000, // 1 second mouse not moved to choose dialog when dragging a file
};

inline const char *cGUIDStr() {
#ifndef OS_MAC_STORE
	static const char *gGuidStr = "{87A94AB0-E370-4cde-98D3-ACC110C5967D}";
#else // OS_MAC_STORE
	static const char *gGuidStr = "{E51FB841-8C0B-4EF9-9E9E-5A0078567627}";
#endif // OS_MAC_STORE

	return gGuidStr;
}

// Mercurygram: update-signing keys. Sign release maps with the matching
// private keys (kept outside the repo, never committed).
static const char *UpdatesPublicKey = "\
-----BEGIN RSA PUBLIC KEY-----\n\
MIIBCgKCAQEAt+T349wW8F41pbXFZjElgkqOAkAqlFcSJvDc93hj0Ets/T4kp+lM\n\
jphespAxymNQErsCZgbISsgXt8kvLSOtwFHAOmTfm7Bg8jszZRrV2VVxN8GDfa1Q\n\
JWoPDyYvyT2TCBQ75Odj3FrA+dctlL9FVJNlGrlD1CwV2xesvgxpycZaylyI5j/K\n\
l+tFcVeFiaRZRfM0KCuD2WdOk80rk2wBgSqosIXkFW0bM0D+POR4tdrX5mZ0yL51\n\
VlSkLEwqed6kMgVaZgpPDb79iLcuHTtcDMxRNISh/EigFGxmrEKy9CP5h7RoI6Oi\n\
vuSgT/cQdSYk7rjiwdl48jV6luzOIjcRQQIDAQAB\n\
-----END RSA PUBLIC KEY-----\
";

static const char *UpdatesPublicBetaKey = "\
-----BEGIN RSA PUBLIC KEY-----\n\
MIIBCgKCAQEA1xJmuCBgM1hUNAWJRz/lVXVx9NKvABJpcaMCL/Ns0eTlhM95IYfl\n\
68LXCuuo8cZw1x8F4SjWDRA6Rq+jZV7qropekZ64lrI0n4ME+NRsTFh1QXUl23Z0\n\
zd7BLIceysI+ZiUyHIIIns/QR/EDRgDqN8615yEU/mQdz2BegpaXNGikuj/BvPzh\n\
GAfthpCi+JXGyv8N60lWMPd5Tz4JNjgAPTVCMng6qmKa2fZdZaQsWDzcLE8fWVfV\n\
0K9EtPuApAGI5bSxwDVD/u4Zlmanh9OJ3o3qI1IGnyu3rrQNs/7jgD99u5P9/KfN\n\
zj8uNEWadnc9IrjrcJjbH+wWE8QsVph4iwIDAQAB\n\
-----END RSA PUBLIC KEY-----\
";

#if defined TDESKTOP_API_ID && defined TDESKTOP_API_HASH

constexpr auto ApiId = TDESKTOP_API_ID;
constexpr auto ApiHash = QT_STRINGIFY(TDESKTOP_API_HASH);

#else // TDESKTOP_API_ID && TDESKTOP_API_HASH

// To build your version of Mercurygram Desktop you're required to provide
// your own 'api_id' and 'api_hash' for the Telegram API access.
//
// How to obtain your 'api_id' and 'api_hash' is described here:
// https://core.telegram.org/api/obtaining_api_id
//
// If you're building the application not for deployment,
// but only for test purposes you can comment out the error below.
//
// This will allow you to use TEST ONLY 'api_id' and 'api_hash' which are
// very limited by the Telegram API server.
//
// Your users will start getting internal server errors on login
// if you deploy an app using those 'api_id' and 'api_hash'.

#error You are required to provide API_ID and API_HASH.

constexpr auto ApiId = 17349;
constexpr auto ApiHash = "344583e45741c457fe1862106095a5eb";

#endif // TDESKTOP_API_ID && TDESKTOP_API_HASH

#if Q_BYTE_ORDER == Q_BIG_ENDIAN
#error "Only little endian is supported!"
#endif // Q_BYTE_ORDER == Q_BIG_ENDIAN

#if (TDESKTOP_ALPHA_VERSION != 0)

// Private key for downloading closed alphas.
#include "../../../DesktopPrivate/alpha_private.h"

#else
static const char *AlphaPrivateKey = "";
#endif

extern QString gKeyFile;
inline const QString &cDataFile() {
	if (!gKeyFile.isEmpty()) return gKeyFile;
	static const QString res(u"data"_q);
	return res;
}

inline const QRegularExpression &cRussianLetters() {
	static QRegularExpression regexp(QString::fromUtf8("[а-яА-ЯёЁ]"));
	return regexp;
}
