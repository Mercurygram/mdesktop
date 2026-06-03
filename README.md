<div align="center">

<img src="./docs/assets/mercurygram_logo.png" alt="Mercurygram Desktop logo" title="Mercurygram Desktop" width="96"/>

# Mercurygram Desktop

</div>

[Telegram](https://telegram.org) is a messaging app with a focus on speed and security. It's superfast, simple and free.

**Mercurygram Desktop** is an unofficial, privacy and security focused fork of [Telegram Desktop](https://github.com/telegramdesktop/tdesktop). It is the desktop counterpart of the [Mercurygram](https://github.com/Mercurygram/Mercurygram) Android client, built by rebasing Mercurygram patches on top of upstream Telegram Desktop. It connects to the regular [Telegram API](https://core.telegram.org) over the [MTProto](https://core.telegram.org/mtproto) protocol while adding extra privacy mitigations (such as the secret-chat work in progress on this branch).

The source code is published under GPLv3 with OpenSSL exception, the license is available [here](LICENSE).

## Install

Mercurygram Desktop is distributed through [GitHub Releases](https://github.com/Mercurygram/mdesktop/releases) for Windows, macOS and Linux. There is no association with Telegram FZ-LLC; do not report Mercurygram issues to upstream Telegram.

> **Note:** because this is an unofficial fork, builds are signed with Mercurygram's own update keys and the in-app updater points at the Mercurygram release server, not Telegram's.

## Build instructions

* [Windows (32-bit and 64-bit)](docs/building-win.md)
* [macOS](docs/building-mac.md)
* [GNU/Linux using Docker](docs/building-linux.md)

To build you must supply your own Telegram `api_id` / `api_hash` at configure time
(`-D TDESKTOP_API_ID=… -D TDESKTOP_API_HASH=…`); credentials are never committed.
See <https://core.telegram.org/api/obtaining_api_id>.

## Why the name Mercurygram?

For a couple of reasons:

- Mercury is the Roman (and I'm Italian) god and the "**messenger** of the gods".
- The logo is a stylized 'F' representing his winged shoes, but it also resembles an 'F' in honor of **Freddy Mercury**.

The application icon is the [hermes wing (created by Anthony Ledoux from the Noun Project)](https://thenounproject.com/icon/hermes-wing-3559879/).

## Third-party

* Qt 6 ([LGPL](http://doc.qt.io/qt-6/lgpl.html)) and Qt 5.15 ([LGPL](http://doc.qt.io/qt-5/lgpl.html)) slightly patched
* OpenSSL 3.2.1 ([Apache License 2.0](https://www.openssl.org/source/apache-license-2.0.txt))
* WebRTC ([New BSD License](https://github.com/desktop-app/tg_owt/blob/master/LICENSE))
* zlib ([zlib License](http://www.zlib.net/zlib_license.html))
* LZMA SDK 9.20 ([public domain](http://www.7-zip.org/sdk.html))
* liblzma ([public domain](http://tukaani.org/xz/))
* Google Breakpad ([License](https://chromium.googlesource.com/breakpad/breakpad/+/master/LICENSE))
* Google Crashpad ([Apache License 2.0](https://chromium.googlesource.com/crashpad/crashpad/+/master/LICENSE))
* GYP ([BSD License](https://github.com/bnoordhuis/gyp/blob/master/LICENSE))
* Ninja ([Apache License 2.0](https://github.com/ninja-build/ninja/blob/master/COPYING))
* OpenAL Soft ([LGPL](https://github.com/kcat/openal-soft/blob/master/COPYING))
* Opus codec ([BSD License](http://www.opus-codec.org/license/))
* FFmpeg ([LGPL](https://www.ffmpeg.org/legal.html))
* Guideline Support Library ([MIT License](https://github.com/Microsoft/GSL/blob/master/LICENSE))
* Range-v3 ([Boost License](https://github.com/ericniebler/range-v3/blob/master/LICENSE.txt))
* Open Sans font ([Apache License 2.0](http://www.apache.org/licenses/LICENSE-2.0.html))
* Vazirmatn font ([SIL Open Font License 1.1](https://github.com/rastikerdar/vazirmatn/blob/master/OFL.txt))
* Emoji alpha codes ([MIT License](https://github.com/emojione/emojione/blob/master/extras/alpha-codes/LICENSE.md))
* xxHash ([BSD License](https://github.com/Cyan4973/xxHash/blob/dev/LICENSE))
* QR Code generator ([MIT License](https://github.com/nayuki/QR-Code-generator#license))
* CMake ([New BSD License](https://github.com/Kitware/CMake/blob/master/Copyright.txt))
* Hunspell ([LGPL](https://github.com/hunspell/hunspell/blob/master/COPYING.LESSER))
* Ada ([Apache License 2.0](https://github.com/ada-url/ada/blob/main/LICENSE-APACHE))
