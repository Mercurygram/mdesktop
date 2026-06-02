/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "boxes/secret_chat_key_box.h"

#include "data/data_secret_chat.h"
#include "data/data_user.h"
#include "lang/lang_keys.h"
#include "base/openssl_help.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/labels.h"
#include "ui/rp_widget.h"
#include "ui/painter.h"
#include "ui/text/text_utilities.h"
#include "styles/style_layers.h"
#include "styles/style_boxes.h"

#include <QtGui/QImage>

namespace {

constexpr auto kGridSide = 12; // 36-byte key hash -> 144 = 12x12 pixels.
constexpr auto kImageSide = 264; // Logical size of the rendered square.

// Builds the secret-chat key identicon, matching the official mobile clients:
// key_hash = sha1(authKey)[0..16) + sha256(authKey)[0..20) = 36 bytes, split
// into 2-bit groups (LSB first) filled left-to-right, top-to-bottom into a
// 12x12 grid using four fixed colors. See core.telegram.org and TDLib's
// SecretChatActor::calc_key_hash / Android IdenticonDrawable.
[[nodiscard]] std::array<uchar, 36> BuildKeyHash(bytes::const_span key) {
	const auto sha1 = openssl::Sha1(key);
	const auto sha256 = openssl::Sha256(key);
	auto data = std::array<uchar, 36>{};
	const auto s1 = reinterpret_cast<const uchar*>(sha1.data());
	const auto s2 = reinterpret_cast<const uchar*>(sha256.data());
	std::copy(s1, s1 + 16, data.begin());
	std::copy(s2, s2 + 20, data.begin() + 16);
	return data;
}

// The hex of the first 32 key-hash bytes, exactly as the official mobile
// clients show it below the identicon: four rows of eight bytes, single space
// between bytes and a wider gap after the fourth (see Android IdenticonActivity).
[[nodiscard]] std::array<QString, 4> FormatKeyHashLines(bytes::const_span key) {
	const auto data = BuildKeyHash(key);
	auto lines = std::array<QString, 4>();
	for (auto row = 0; row != 4; ++row) {
		auto &line = lines[row];
		for (auto col = 0; col != 8; ++col) {
			if (col != 0) {
				line += (col == 4) ? QString("  ") : QString(" ");
			}
			line += QString::number(data[row * 8 + col], 16)
				.rightJustified(2, '0')
				.toUpper();
		}
	}
	return lines;
}

[[nodiscard]] QImage GenerateKeyImage(bytes::const_span key) {
	const auto data = BuildKeyHash(key);

	static constexpr QRgb kColors[4] = {
		0xffffffffu, // FFFFFF white
		0xffd5e6f3u, // D5E6F3 light blue
		0xff2d5775u, // 2D5775 dark blue
		0xff2f99c9u, // 2F99C9 cyan
	};

	auto image = QImage(
		kGridSide,
		kGridSide,
		QImage::Format_ARGB32_Premultiplied);
	auto bitOffset = 0;
	for (auto y = 0; y != kGridSide; ++y) {
		for (auto x = 0; x != kGridSide; ++x) {
			const auto value = (data[bitOffset / 8] >> (bitOffset % 8)) & 0x3;
			image.setPixel(x, y, kColors[value]);
			bitOffset += 2;
		}
	}

	const auto ratio = style::DevicePixelRatio();
	auto result = image.scaled(
		kImageSide * ratio,
		kImageSide * ratio,
		Qt::IgnoreAspectRatio,
		Qt::FastTransformation);
	result.setDevicePixelRatio(ratio);
	return result;
}

} // namespace

void SecretChatKeyBox(
		not_null<Ui::GenericBox*> box,
		not_null<SecretChatData*> chat) {
	box->setTitle(tr::lng_secret_chat_key_title());

	// The box stretches each row to the full content width, so center the
	// image and the hex text horizontally inside the widget at paint time.
	auto image = GenerateKeyImage(chat->key());
	const auto widget = box->addRow(
		object_ptr<Ui::RpWidget>(box),
		style::al_top);
	widget->resize(kImageSide, kImageSide);
	widget->paintRequest(
	) | rpl::on_next([=] {
		auto p = QPainter(widget);
		const auto left = (widget->width() - kImageSide) / 2;
		p.drawImage(QRect(left, 0, kImageSide, kImageSide), image);
	}, widget->lifetime());

	// The same key hash as hex, centered and greyed, matching the official
	// mobile clients so users can compare the digits as well as the image.
	const auto lines = FormatKeyHashLines(chat->key());
	const auto font = st::normalFont->monospace();
	const auto hex = box->addRow(
		object_ptr<Ui::RpWidget>(box),
		style::margins(
			0,
			st::boxRowPadding.top() + st::defaultVerticalListSkip,
			0,
			st::defaultVerticalListSkip),
		style::al_top);
	hex->resize(kImageSide, font->height * int(lines.size()));
	hex->paintRequest(
	) | rpl::on_next([=] {
		auto p = QPainter(hex);
		p.setFont(font->f);
		p.setPen(st::windowSubTextFg);
		const auto width = hex->width();
		for (auto i = 0; i != int(lines.size()); ++i) {
			const auto left = (width - font->width(lines[i])) / 2;
			p.drawText(left, i * font->height + font->ascent, lines[i]);
		}
	}, hex->lifetime());

	const auto user = chat->user();
	const auto name = user ? user->shortName() : QString();
	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			tr::lng_secret_chat_key_about(
				lt_user,
				rpl::single(Ui::Text::Bold(name)),
				Ui::Text::WithEntities),
			st::secretChatKeyAbout),
		st::boxRowPadding);

	box->addButton(tr::lng_about_done(), [=] { box->closeBox(); });
}
