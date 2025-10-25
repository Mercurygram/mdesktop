/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "ui/widgets/buttons.h"
#include "ui/text/text.h"
#include "ui/effects/animations.h"
#include "data/data_peer_colors.h"

namespace Ui {
namespace Text {
struct MarkedContext;
} // namespace Text
class ChatStyle;
struct ColorCollectible;

class ColorSample final : public AbstractButton {
public:
	ColorSample(
		not_null<QWidget*> parent,
		Fn<Ui::Text::MarkedContext()> contextProvider,
		Fn<TextWithEntities(uint64)> emojiProvider,
		std::shared_ptr<ChatStyle> style,
		rpl::producer<uint8> colorIndex,
		rpl::producer<std::shared_ptr<ColorCollectible>> collectible,
		const QString &name);
	ColorSample(
		not_null<QWidget*> parent,
		std::shared_ptr<ChatStyle> style,
		uint8 colorIndex,
		bool selected);
	ColorSample(
		not_null<QWidget*> parent,
		Fn<Data::ColorProfileSet(uint8)> profileProvider,
		uint8 colorIndex,
		bool selected);

	[[nodiscard]] uint8 index() const;

	void setSelected(bool selected);

private:
	void paintEvent(QPaintEvent *e) override;

	std::shared_ptr<ChatStyle> _style;
	Text::String _name;
	uint8 _index = 0;
	std::shared_ptr<ColorCollectible> _collectible;
	Animations::Simple _selectAnimation;
	bool _selected = false;
	bool _simple = false;
	Fn<Data::ColorProfileSet(uint8)> _profileProvider;

};

} // namespace Ui