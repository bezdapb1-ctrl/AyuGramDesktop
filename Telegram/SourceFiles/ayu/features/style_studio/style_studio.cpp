#include "ayu/features/style_studio/style_studio.h"

#include "ayu/ayu_settings.h"
#include "ayu/ayu_ui_settings.h"
#include "ayu/ui/settings/settings_ayu_utils.h"
#include "base/unixtime.h"
#include "base/zlib_help.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "core/file_utilities.h"
#include "lang/lang_keys.h"
#include "settings.h"
#include "storage/localstorage.h"
#include "ui/boxes/choose_font_box.h"
#include "ui/image/image_prepare.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/style/style_core_font.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/color_editor.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "window/themes/window_theme.h"
#include "window/window_session_controller.h"
#include "styles/style_basic.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"

#include <QtCore/QBuffer>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtGui/QImage>

#include <array>

namespace AyuFeatures::StyleStudio {
namespace {

constexpr auto kFieldsCount = 9;
constexpr auto kMaxBackgroundSide = 2560;
constexpr auto kBlurRadius = 24;
constexpr auto kDimAlpha = 110;
constexpr auto kPreviewHeight = 170;
constexpr auto kSwatchSize = 22;

enum class Shape {
	Keep = 0,
	Sharp = 1,
	Soft = 2,
	Round = 3,
};

struct Colors {
	QColor bg;
	QColor surface;
	QColor elevated;
	QColor text;
	QColor subtext;
	QColor accent;
	QColor accentText;
	QColor in;
	QColor out;
};

struct Field {
	QColor Colors::*member = nullptr;
	const char *key = nullptr;
};

const auto kFields = std::array<Field, kFieldsCount>{ {
	{ &Colors::bg, "bg" },
	{ &Colors::surface, "surface" },
	{ &Colors::elevated, "elevated" },
	{ &Colors::text, "text" },
	{ &Colors::subtext, "subtext" },
	{ &Colors::accent, "accent" },
	{ &Colors::accentText, "accentText" },
	{ &Colors::in, "in" },
	{ &Colors::out, "out" },
} };

struct Preset {
	const char *name = nullptr;
	std::array<const char*, kFieldsCount> colors = {};
	Shape shape = Shape::Keep;
};

const auto kPresets = std::array<Preset, 6>{ {
	{ "Graphite Black", { "#0b0b0c", "#111113", "#18181b", "#ececef",
		"#8b8b93", "#3d6fd6", "#8fb4ff", "#1a1a1d", "#1f2b45" },
		Shape::Soft },
	{ "Void Purple", { "#000000", "#000000", "#0e0b14", "#ececf1",
		"#8a8496", "#5b2fa8", "#b394ff", "#121016", "#1f1033" },
		Shape::Soft },
	{ "Midnight Blue", { "#0f1624", "#131c2e", "#1a2438", "#e6edf7",
		"#8696b0", "#3b82f6", "#7fb2ff", "#1b2539", "#234a8a" },
		Shape::Round },
	{ "Forest", { "#0f1712", "#121c16", "#18241d", "#e5efe7",
		"#86998b", "#2f9e5b", "#73d39a", "#18241d", "#1d4a31" },
		Shape::Round },
	{ "Sakura", { "#1a0f16", "#20121b", "#2a1824", "#f6e8f0",
		"#b08aa0", "#e0569b", "#ff9ccc", "#2a1824", "#5a1f43" },
		Shape::Round },
	{ "Classic Light", { "#ffffff", "#ffffff", "#ffffff", "#000000",
		"#8a8a8a", "#3390ec", "#168acd", "#ffffff", "#effdde" },
		Shape::Round },
} };

struct Studio {
	Colors colors;
	Shape shape = Shape::Keep;
	QImage background;
	bool blur = false;
	bool dim = false;
	QString font;
	bool fontChanged = false;
	rpl::event_stream<> changed;
};

[[nodiscard]] QString FieldTitle(int index) {
	switch (index) {
	case 0: return tr::ayu_StyleColorBg(tr::now);
	case 1: return tr::ayu_StyleColorSurface(tr::now);
	case 2: return tr::ayu_StyleColorElevated(tr::now);
	case 3: return tr::ayu_StyleColorText(tr::now);
	case 4: return tr::ayu_StyleColorSubtext(tr::now);
	case 5: return tr::ayu_StyleColorAccent(tr::now);
	case 6: return tr::ayu_StyleColorAccentText(tr::now);
	case 7: return tr::ayu_StyleColorIn(tr::now);
	case 8: return tr::ayu_StyleColorOut(tr::now);
	}
	return QString();
}

void ApplyPreset(not_null<Studio*> studio, const Preset &preset) {
	for (auto i = 0; i != kFieldsCount; ++i) {
		studio->colors.*(kFields[i].member) = QColor(
			QString::fromLatin1(preset.colors[i]));
	}
	studio->shape = preset.shape;
}

[[nodiscard]] QColor Mix(const QColor &a, const QColor &b, double t) {
	const auto mix = [&](float x, float y) {
		return float(x * (1. - t) + y * t);
	};
	return QColor::fromRgbF(
		mix(a.redF(), b.redF()),
		mix(a.greenF(), b.greenF()),
		mix(a.blueF(), b.blueF()),
		mix(a.alphaF(), b.alphaF()));
}

[[nodiscard]] QColor WithAlpha(QColor color, int alpha) {
	color.setAlpha(alpha);
	return color;
}

[[nodiscard]] QByteArray Hex(const QColor &color) {
	auto result = QString::asprintf(
		"#%02x%02x%02x",
		color.red(),
		color.green(),
		color.blue());
	if (color.alpha() != 255) {
		result += QString::asprintf("%02x", color.alpha());
	}
	return result.toLatin1();
}

[[nodiscard]] QByteArray GeneratePalette(const Colors &c) {
	const auto bg = c.bg;
	const auto surf = c.surface;
	const auto elev = c.elevated;
	const auto fg = c.text;
	const auto sub = c.subtext;
	const auto acc = c.accent;
	const auto accTxt = c.accentText;
	const auto in = c.in;
	const auto out = c.out;

	const auto dark = (bg.lightnessF() < 0.5);
	const auto white = QColor(255, 255, 255);
	const auto black = QColor(0, 0, 0);
	const auto red = QColor(0xef, 0x5b, 0x5b);
	const auto green = QColor(0x4f, 0xc2, 0x7a);
	const auto none = WithAlpha(black, 0);

	const auto hover = Mix(elev, fg, 0.05);
	const auto ripple = Mix(elev, fg, 0.10);
	const auto border = Mix(elev, fg, 0.12);
	const auto fg2 = Mix(fg, sub, 0.35);
	const auto accHi = dark ? acc.lighter(115) : acc.darker(110);
	const auto accRipple = Mix(acc, bg, 0.25);
	const auto sel = Mix(surf, acc, dark ? 0.25 : 0.15);
	const auto selHi = Mix(surf, acc, dark ? 0.35 : 0.25);
	const auto inSel = Mix(in, acc, 0.12);
	const auto outSel = Mix(out, acc, 0.15);
	const auto outSub = Mix(out, fg, 0.55);
	const auto onAcc = (acc.lightnessF() > 0.7) ? black : white;
	const auto onSel = dark ? white : fg;
	const auto selSub = Mix(onSel, sel, 0.3);
	const auto muted = Mix(sub, surf, 0.4);
	const auto disabled = Mix(sub, bg, 0.35);
	const auto placeholder = Mix(sub, bg, 0.2);
	const auto inputBg = Mix(surf, fg, dark ? 0.04 : 0.05);
	const auto sideBar = dark ? Mix(surf, black, 0.4) : Mix(surf, fg, 0.06);
	const auto over = [&](int alpha) {
		return WithAlpha(dark ? white : black, alpha);
	};
	const auto bubbleShadow = WithAlpha(black, dark ? 0 : 0x1e);
	const auto outAccent = Mix(accTxt, fg, 0.3);
	const auto closeRed = QColor(0xe8, 0x11, 0x23);

	auto result = QByteArray("// Generated by the AyuGram style studio\n");
	const auto add = [&](const char *key, const QColor &value) {
		result.append(key).append(": ").append(Hex(value)).append(";\n");
	};

	add("windowBg", bg);
	add("windowFg", fg);
	add("windowBgOver", hover);
	add("windowBgRipple", ripple);
	add("windowFgOver", fg);
	add("windowSubTextFg", sub);
	add("windowSubTextFgOver", Mix(sub, fg, 0.2));
	add("windowBoldFg", fg);
	add("windowBoldFgOver", fg);
	add("windowBgActive", acc);
	add("windowFgActive", onAcc);
	add("windowActiveTextFg", accTxt);
	add("windowShadowFg", black);
	add("windowShadowFgFallback", Mix(bg, black, 0.2));
	add("shadowFg", WithAlpha(black, dark ? 0x40 : 0x18));
	add("slideFadeOutBg", WithAlpha(black, 0x3c));
	add("imageBg", black);
	add("imageBgTransparent", white);

	add("activeButtonBg", acc);
	add("activeButtonBgOver", accHi);
	add("activeButtonBgRipple", accRipple);
	add("activeButtonFg", onAcc);
	add("activeButtonFgOver", onAcc);
	add("activeButtonSecondaryFg", Mix(onAcc, acc, 0.2));
	add("activeButtonSecondaryFgOver", Mix(onAcc, acc, 0.2));
	add("activeLineFg", acc);
	add("activeLineFgError", red);
	add("lightButtonBg", bg);
	add("lightButtonBgOver", hover);
	add("lightButtonBgRipple", ripple);
	add("lightButtonFg", accTxt);
	add("lightButtonFgOver", accTxt);
	add("attentionButtonFg", red);
	add("attentionButtonFgOver", red);
	add("attentionButtonBgOver", Mix(bg, red, 0.12));
	add("attentionButtonBgRipple", Mix(bg, red, 0.2));
	add("outlineButtonBg", bg);
	add("outlineButtonBgOver", hover);
	add("outlineButtonOutlineFg", acc);
	add("outlineButtonBgRipple", ripple);

	add("menuBg", elev);
	add("menuBgOver", hover);
	add("menuBgRipple", ripple);
	add("menuIconFg", sub);
	add("menuIconFgOver", fg2);
	add("menuSubmenuArrowFg", sub);
	add("menuFgDisabled", disabled);
	add("menuSeparatorFg", border);

	add("scrollBarBg", over(0x40));
	add("scrollBarBgOver", over(0x60));
	add("scrollBg", over(0x10));
	add("scrollBgOver", over(0x1c));
	add("smallCloseIconFg", sub);
	add("smallCloseIconFgOver", fg2);
	add("radialFg", acc);
	add("radialBg", Mix(sub, bg, 0.3));
	add("placeholderFg", placeholder);
	add("placeholderFgActive", Mix(placeholder, bg, 0.25));
	add("inputBorderFg", border);
	add("filterInputBorderFg", acc);
	add("filterInputInactiveBg", inputBg);
	add("filterInputActiveBg", inputBg);
	add("checkboxFg", Mix(sub, bg, 0.2));
	add("sliderBgInactive", Mix(sub, bg, 0.5));
	add("sliderBgActive", acc);
	add("tooltipBg", elev);
	add("tooltipFg", fg);
	add("tooltipBorderFg", border);

	add("titleShadow", none);
	add("titleBg", surf);
	add("titleBgActive", surf);
	add("titleButtonBg", surf);
	add("titleButtonFg", sub);
	add("titleButtonBgOver", hover);
	add("titleButtonFgOver", fg2);
	add("titleButtonBgActive", surf);
	add("titleButtonFgActive", sub);
	add("titleButtonBgActiveOver", hover);
	add("titleButtonFgActiveOver", fg2);
	add("titleButtonCloseBg", surf);
	add("titleButtonCloseFg", sub);
	add("titleButtonCloseBgOver", closeRed);
	add("titleButtonCloseFgOver", white);
	add("titleButtonCloseBgActive", surf);
	add("titleButtonCloseFgActive", sub);
	add("titleButtonCloseBgActiveOver", closeRed);
	add("titleButtonCloseFgActiveOver", white);
	add("titleFg", sub);
	add("titleFgActive", fg2);
	add("trayCounterBg", acc);
	add("trayCounterBgMute", muted);
	add("trayCounterFg", onAcc);

	add("layerBg", WithAlpha(black, dark ? 0xa0 : 0x7f));
	add("cancelIconFg", sub);
	add("cancelIconFgOver", fg2);
	add("boxBg", elev);
	add("boxTextFg", fg);
	add("boxTextFgGood", green);
	add("boxTextFgError", red);
	add("boxTitleFg", fg);
	add("boxSearchBg", elev);
	add("boxTitleAdditionalFg", sub);
	add("boxTitleCloseFg", sub);
	add("boxTitleCloseFgOver", fg2);
	add("contactsBg", elev);
	add("contactsBgOver", hover);
	add("contactsNameFg", fg);
	add("contactsStatusFg", sub);
	add("contactsStatusFgOver", sub);
	add("contactsStatusFgOnline", accTxt);
	add("photoCropFadeBg", WithAlpha(black, 0xa0));
	add("photoCropPointFg", WithAlpha(white, 0xa0));

	add("dialogsMenuIconFg", sub);
	add("dialogsMenuIconFgOver", fg2);
	add("dialogsBg", surf);
	add("dialogsNameFg", fg);
	add("dialogsChatIconFg", fg);
	add("dialogsDateFg", sub);
	add("dialogsTextFg", sub);
	add("dialogsTextFgService", accTxt);
	add("dialogsDraftFg", red);
	add("dialogsVerifiedIconBg", acc);
	add("dialogsVerifiedIconFg", onAcc);
	add("dialogsSendingIconFg", sub);
	add("dialogsSentIconFg", accTxt);
	add("dialogsUnreadBg", acc);
	add("dialogsUnreadBgMuted", muted);
	add("dialogsUnreadFg", onAcc);
	add("dialogsBgOver", Mix(surf, fg, 0.04));
	add("dialogsNameFgOver", fg);
	add("dialogsChatIconFgOver", fg);
	add("dialogsDateFgOver", sub);
	add("dialogsTextFgOver", sub);
	add("dialogsTextFgServiceOver", accTxt);
	add("dialogsDraftFgOver", red);
	add("dialogsVerifiedIconBgOver", acc);
	add("dialogsVerifiedIconFgOver", onAcc);
	add("dialogsSendingIconFgOver", sub);
	add("dialogsSentIconFgOver", accTxt);
	add("dialogsUnreadBgOver", acc);
	add("dialogsUnreadBgMutedOver", muted);
	add("dialogsUnreadFgOver", onAcc);
	add("dialogsBgActive", sel);
	add("dialogsNameFgActive", onSel);
	add("dialogsChatIconFgActive", onSel);
	add("dialogsDateFgActive", selSub);
	add("dialogsTextFgActive", selSub);
	add("dialogsTextFgServiceActive", onSel);
	add("dialogsDraftFgActive", Mix(red, white, 0.3));
	add("dialogsVerifiedIconBgActive", onSel);
	add("dialogsVerifiedIconFgActive", sel);
	add("dialogsSendingIconFgActive", selSub);
	add("dialogsSentIconFgActive", onSel);
	add("dialogsUnreadBgActive", onSel);
	add("dialogsUnreadBgMutedActive", selSub);
	add("dialogsUnreadFgActive", sel);
	add("dialogsRippleBg", ripple);
	add("dialogsRippleBgActive", selHi);
	add("dialogsForwardBg", sel);
	add("dialogsForwardFg", onSel);
	add("searchedBarBg", elev);
	add("searchedBarFg", sub);

	add("sideBarBg", sideBar);
	add("sideBarBgActive", Mix(sideBar, acc, 0.2));
	add("sideBarBgRipple", ripple);
	add("sideBarTextFg", sub);
	add("sideBarTextFgActive", fg);
	add("sideBarIconFg", sub);
	add("sideBarIconFgActive", accTxt);
	add("sideBarBadgeBg", acc);
	add("sideBarBadgeBgMuted", muted);
	add("sideBarBadgeFg", onAcc);

	add("topBarBg", surf);
	add("emojiPanBg", elev);
	add("emojiPanCategories", Mix(elev, black, 0.15));
	add("emojiPanHeaderFg", sub);
	add("emojiPanHeaderBg", WithAlpha(elev, 0xf2));
	add("emojiIconFg", sub);
	add("emojiIconFgActive", accTxt);
	add("stickerPanDeleteBg", WithAlpha(black, 0xcc));
	add("stickerPanDeleteFg", white);
	add("stickerPreviewBg", WithAlpha(Mix(bg, black, 0.2), 0xb0));

	add("historyTextInFg", fg);
	add("historyTextOutFg", fg);
	add("historyCaptionInFg", fg);
	add("historyCaptionOutFg", fg);
	add("historyFileNameInFg", fg);
	add("historyFileNameOutFg", fg);
	add("historyOutIconFg", accTxt);
	add("historyOutIconFgSelected", accTxt);
	add("historyIconFgInverted", white);
	add("historySendingOutIconFg", outSub);
	add("historySendingInIconFg", sub);
	add("historySendingInvertedIconFg", WithAlpha(white, 0xc8));
	add("historySystemBg", WithAlpha(Mix(bg, black, 0.3), 0x99));
	add("historySystemBgSelected", WithAlpha(sel, 0xcc));
	add("historyUnreadBarBg", surf);
	add("historyUnreadBarBorder", none);
	add("historyUnreadBarFg", accTxt);
	add("historyForwardChooseBg", WithAlpha(black, 0x4c));
	add("historyForwardChooseFg", white);
	add("historyPeer1NameFg", QColor(0xf2, 0x7a, 0x7a));
	add("historyPeer2NameFg", QColor(0x7e, 0xd3, 0x8a));
	add("historyPeer3NameFg", QColor(0xe0, 0xa3, 0x5c));
	add("historyPeer4NameFg", QColor(0x6f, 0xb4, 0xf2));
	add("historyPeer5NameFg", QColor(0xb0, 0x8c, 0xf0));
	add("historyPeer6NameFg", QColor(0xee, 0x7f, 0xb1));
	add("historyPeer7NameFg", QColor(0x5c, 0xcf, 0xd0));
	add("historyPeer8NameFg", QColor(0xe8, 0xb6, 0x5a));
	add("historyScrollBarBg", over(0x40));
	add("historyScrollBarBgOver", over(0x60));
	add("historyScrollBg", over(0x10));
	add("historyScrollBgOver", over(0x1c));

	add("msgInBg", in);
	add("msgInBgSelected", inSel);
	add("msgOutBg", out);
	add("msgOutBgSelected", outSel);
	add("msgSelectOverlay", WithAlpha(acc, 0x33));
	add("msgStickerOverlay", WithAlpha(acc, 0x7f));
	add("msgInServiceFg", accTxt);
	add("msgInServiceFgSelected", accTxt);
	add("msgOutServiceFg", outAccent);
	add("msgOutServiceFgSelected", outAccent);
	add("msgInShadow", bubbleShadow);
	add("msgInShadowSelected", bubbleShadow);
	add("msgOutShadow", bubbleShadow);
	add("msgOutShadowSelected", bubbleShadow);
	add("msgInDateFg", sub);
	add("msgInDateFgSelected", sub);
	add("msgOutDateFg", outSub);
	add("msgOutDateFgSelected", outSub);
	add("msgServiceFg", white);
	add("msgServiceBg", WithAlpha(Mix(bg, black, 0.4), 0xe6));
	add("msgServiceBgSelected", WithAlpha(Mix(acc, black, 0.3), 0xe6));
	add("msgInReplyBarColor", accTxt);
	add("msgInReplyBarSelColor", accTxt);
	add("msgOutReplyBarColor", outAccent);
	add("msgOutReplyBarSelColor", outAccent);
	add("msgImgReplyBarColor", white);
	add("msgInMonoFg", Mix(accTxt, fg, 0.3));
	add("msgOutMonoFg", Mix(accTxt, fg, 0.5));
	add("msgDateImgFg", white);
	add("msgDateImgBg", WithAlpha(black, 0x77));
	add("msgDateImgBgOver", WithAlpha(black, 0x99));
	add("msgDateImgBgSelected", WithAlpha(Mix(acc, black, 0.4), 0xaa));
	add("msgFileThumbLinkInFg", accTxt);
	add("msgFileThumbLinkInFgSelected", accTxt);
	add("msgFileThumbLinkOutFg", outAccent);
	add("msgFileThumbLinkOutFgSelected", outAccent);
	add("msgFileInBg", acc);
	add("msgFileInBgOver", accHi);
	add("msgFileInBgSelected", accHi);
	add("msgFileOutBg", acc.lighter(110));
	add("msgFileOutBgOver", accHi);
	add("msgFileOutBgSelected", accHi);
	add("msgFile1Bg", QColor(0x7b, 0x4f, 0xd6));
	add("msgFile2Bg", QColor(0x4f, 0xae, 0x6a));
	add("msgFile3Bg", QColor(0xd6, 0x5a, 0x5a));
	add("msgFile4Bg", QColor(0xd9, 0xa4, 0x41));
	add("msgWaveformInActive", accTxt);
	add("msgWaveformInActiveSelected", accTxt);
	add("msgWaveformInInactive", Mix(in, fg, 0.2));
	add("msgWaveformInInactiveSelected", Mix(inSel, fg, 0.25));
	add("msgWaveformOutActive", Mix(accTxt, fg, 0.5));
	add("msgWaveformOutActiveSelected", Mix(accTxt, fg, 0.5));
	add("msgWaveformOutInactive", Mix(out, fg, 0.25));
	add("msgWaveformOutInactiveSelected", Mix(outSel, fg, 0.3));
	add("msgBotKbOverBgAdd", over(0x14));
	add("msgBotKbIconFg", white);
	add("msgBotKbRippleBg", over(0x1f));
	add("mediaInFg", sub);
	add("mediaInFgSelected", sub);
	add("mediaOutFg", outSub);
	add("mediaOutFgSelected", outSub);

	add("toastBg", WithAlpha(Mix(elev, black, 0.3), 0xf0));
	add("toastFg", white);
	add("reportSpamBg", surf);
	add("reportSpamFg", fg);
	add("historyToDownBg", elev);
	add("historyToDownBgOver", hover);
	add("historyToDownBgRipple", ripple);
	add("historyToDownFg", sub);
	add("historyToDownFgOver", fg2);
	add("historyToDownShadow", WithAlpha(black, 0x40));
	add("historyComposeAreaBg", surf);
	add("historyComposeAreaFg", fg);
	add("historyComposeAreaFgService", sub);
	add("historyComposeIconFg", sub);
	add("historyComposeIconFgOver", fg2);
	add("historySendIconFg", accTxt);
	add("historySendIconFgOver", Mix(accTxt, fg, 0.2));
	add("historyPinnedBg", surf);
	add("historyReplyBg", surf);
	add("historyReplyIconFg", accTxt);
	add("historyReplyCancelFg", sub);
	add("historyReplyCancelFgOver", fg2);
	add("historyComposeButtonBg", surf);
	add("historyComposeButtonBgOver", hover);
	add("historyComposeButtonBgRipple", ripple);

	add("overviewCheckBg", WithAlpha(black, 0x66));
	add("overviewCheckFg", white);
	add("overviewCheckFgActive", white);
	add("overviewPhotoSelectOverlay", WithAlpha(acc, 0x33));
	add("profileStatusFgOver", sub);
	add("profileVerifiedCheckBg", acc);
	add("profileVerifiedCheckFg", onAcc);
	add("profileAdminStartFg", accTxt);
	add("notificationsBoxMonitorFg", fg);
	add("notificationsBoxScreenBg", Mix(bg, black, 0.3));
	add("notificationSampleUserpicFg", Mix(sub, bg, 0.5));
	add("notificationSampleCloseFg", Mix(sub, bg, 0.3));
	add("notificationSampleTextFg", Mix(sub, bg, 0.3));
	add("notificationSampleNameFg", Mix(sub, bg, 0.15));
	add("mainMenuBg", elev);
	add("mainMenuCoverBg", Mix(elev, black, 0.2));
	add("mainMenuCoverFg", fg);
	add("mediaPlayerBg", surf);
	add("mediaPlayerActiveFg", accTxt);
	add("mediaPlayerInactiveFg", muted);
	add("mediaPlayerDisabledFg", disabled);
	add("introBg", bg);
	add("introTitleFg", fg);
	add("introDescriptionFg", sub);
	add("introErrorFg", red);
	add("introCoverTopBg", sel);
	add("introCoverBottomBg", sel);
	add("introCoverIconsFg", Mix(sel, acc, 0.4));

	return result;
}

[[nodiscard]] QByteArray Encode(
		const QImage &image,
		const char *format,
		int quality = -1) {
	auto result = QByteArray();
	QBuffer buffer(&result);
	buffer.open(QIODevice::WriteOnly);
	image.save(&buffer, format, quality);
	return result;
}

[[nodiscard]] QImage PreparedBackground(not_null<const Studio*> studio) {
	auto image = studio->background;
	if (image.isNull()) {
		return image;
	}
	if (image.width() > kMaxBackgroundSide
		|| image.height() > kMaxBackgroundSide) {
		image = image.scaled(
			kMaxBackgroundSide,
			kMaxBackgroundSide,
			Qt::KeepAspectRatio,
			Qt::SmoothTransformation);
	}
	image = std::move(image).convertToFormat(
		QImage::Format_ARGB32_Premultiplied);
	if (studio->blur) {
		image = Images::BlurLargeImage(std::move(image), kBlurRadius);
	}
	if (studio->dim) {
		auto p = QPainter(&image);
		p.fillRect(image.rect(), QColor(0, 0, 0, kDimAlpha));
	}
	return image;
}

[[nodiscard]] QByteArray PackTheme(
		const QByteArray &palette,
		const QByteArray &background,
		bool png,
		bool tiled) {
	zlib::FileToWrite zip;

	zip_fileinfo zfi = { { 0, 0, 0, 0, 0, 0 }, 0, 0, 0 };
	const auto back = std::string(tiled ? "tiled" : "background")
		+ (png ? ".png" : ".jpg");
	zip.openNewFile(
		back.c_str(),
		&zfi,
		nullptr,
		0,
		nullptr,
		0,
		nullptr,
		Z_DEFLATED,
		Z_DEFAULT_COMPRESSION);
	zip.writeInFile(background.constData(), background.size());
	zip.closeFile();
	zip.openNewFile(
		"colors.tdesktop-theme",
		&zfi,
		nullptr,
		0,
		nullptr,
		0,
		nullptr,
		Z_DEFLATED,
		Z_DEFAULT_COMPRESSION);
	zip.writeInFile(palette.constData(), palette.size());
	zip.closeFile();
	zip.close();

	return (zip.error() == ZIP_OK) ? zip.result() : QByteArray();
}

[[nodiscard]] QByteArray BuildTheme(not_null<const Studio*> studio) {
	const auto prepared = PreparedBackground(studio);
	if (prepared.isNull()) {
		auto tile = QImage(16, 16, QImage::Format_RGB32);
		tile.fill(studio->colors.bg);
		return PackTheme(
			GeneratePalette(studio->colors),
			Encode(tile, "PNG"),
			true,
			true);
	}
	return PackTheme(
		GeneratePalette(studio->colors),
		Encode(prepared.convertToFormat(QImage::Format_RGB32), "JPG", 88),
		false,
		false);
}

[[nodiscard]] std::pair<int, int> ShapeValues(Shape shape) {
	const auto max = AyuUiSettings::kMaxAvatarCorners;
	switch (shape) {
	case Shape::Sharp: return { 2, 0 };
	case Shape::Soft: return { 8, max / 2 };
	case Shape::Round: return { 16, max };
	case Shape::Keep: break;
	}
	return { -1, -1 };
}

[[nodiscard]] QJsonObject ToJson(not_null<const Studio*> studio) {
	auto colors = QJsonObject();
	for (const auto &field : kFields) {
		colors.insert(
			QString::fromLatin1(field.key),
			QString::fromLatin1(Hex(studio->colors.*(field.member))));
	}
	auto result = QJsonObject();
	result.insert(u"format"_q, 1);
	result.insert(u"colors"_q, colors);
	result.insert(u"shape"_q, int(studio->shape));
	result.insert(u"blur"_q, studio->blur);
	result.insert(u"dim"_q, studio->dim);
	return result;
}

void FromJson(not_null<Studio*> studio, const QJsonObject &object) {
	const auto colors = object.value(u"colors"_q).toObject();
	for (const auto &field : kFields) {
		const auto value = QColor(
			colors.value(QString::fromLatin1(field.key)).toString());
		if (value.isValid()) {
			studio->colors.*(field.member) = value;
		}
	}
	const auto shape = object.value(u"shape"_q).toInt(0);
	studio->shape = (shape >= 0 && shape <= int(Shape::Round))
		? Shape(shape)
		: Shape::Keep;
	studio->blur = object.value(u"blur"_q).toBool(false);
	studio->dim = object.value(u"dim"_q).toBool(false);
}

void SaveState(not_null<const Studio*> studio) {
	AyuSettings::getInstance().setStyleStudio(QString::fromUtf8(
		QJsonDocument(ToJson(studio)).toJson(QJsonDocument::Compact)));
}

void LoadState(not_null<Studio*> studio) {
	ApplyPreset(studio, kPresets[0]);
	studio->shape = Shape::Keep;
	const auto &raw = AyuSettings::getInstance().styleStudio();
	if (!raw.isEmpty()) {
		FromJson(studio, QJsonDocument::fromJson(raw.toUtf8()).object());
	}
}

void Apply(
		not_null<Window::SessionController*> controller,
		not_null<Studio*> studio) {
	const auto theme = BuildTheme(studio);
	const auto folder = cWorkingDir() + u"tdata/ayu/styles/"_q;
	QDir().mkpath(folder);
	const auto path = folder
		+ u"studio_%1.tdesktop-theme"_q.arg(base::unixtime::now());
	QFile file(path);
	if (theme.isEmpty()
		|| !file.open(QIODevice::WriteOnly)
		|| file.write(theme) != theme.size()) {
		controller->showToast(tr::ayu_StyleFailed(tr::now));
		return;
	}
	file.close();
	if (Window::Theme::Apply(path)) {
		Window::Theme::KeepApplied();
	}
	SaveState(studio);

	auto restart = false;
	const auto [radius, corners] = ShapeValues(studio->shape);
	if (radius >= 0) {
		auto &settings = AyuSettings::getInstance();
		if (settings.messageBubbleRadius() != radius
			|| settings.avatarCorners() != corners) {
			settings.setMessageBubbleRadius(radius);
			settings.setAvatarCorners(corners);
			restart = true;
		}
	}
	if (studio->fontChanged) {
		studio->fontChanged = false;
		Core::App().settings().setCustomFontFamily(studio->font);
		Local::writeSettings();
		restart = true;
	}
	if (restart) {
		Settings::ShowRestartPrompt(controller);
	} else {
		controller->showToast(tr::ayu_StyleApplied(tr::now));
	}
}

void ExportStyle(
		not_null<Window::SessionController*> controller,
		not_null<Studio*> studio,
		QPointer<QWidget> parent) {
	FileDialog::GetWritePath(
		parent,
		tr::ayu_StyleExport(tr::now),
		u"AyuGram style (*.ayustyle)"_q,
		u"style.ayustyle"_q,
		[=](QString &&path) {
			auto object = ToJson(studio);
			const auto prepared = PreparedBackground(studio);
			if (!prepared.isNull()) {
				object.insert(
					u"background"_q,
					QString::fromLatin1(Encode(
						prepared.convertToFormat(QImage::Format_RGB32),
						"JPG",
						85).toBase64()));
			}
			QFile file(path);
			if (file.open(QIODevice::WriteOnly)) {
				file.write(QJsonDocument(object).toJson());
				controller->showToast(tr::ayu_StyleExported(tr::now));
			} else {
				controller->showToast(tr::ayu_StyleFailed(tr::now));
			}
		});
}

void ImportStyle(
		not_null<Window::SessionController*> controller,
		not_null<Studio*> studio,
		QPointer<QWidget> parent,
		Fn<void()> imported) {
	FileDialog::GetOpenPath(
		parent,
		tr::ayu_StyleImport(tr::now),
		u"AyuGram style (*.ayustyle)"_q,
		[=](FileDialog::OpenResult &&result) {
			auto content = result.remoteContent;
			if (content.isEmpty() && !result.paths.isEmpty()) {
				QFile file(result.paths.front());
				if (file.open(QIODevice::ReadOnly)) {
					content = file.readAll();
				}
			}
			const auto object = QJsonDocument::fromJson(content).object();
			if (object.value(u"format"_q).toInt() != 1) {
				controller->showToast(tr::ayu_StyleFailed(tr::now));
				return;
			}
			FromJson(studio, object);
			studio->background = QImage::fromData(QByteArray::fromBase64(
				object.value(u"background"_q).toString().toLatin1()));
			studio->blur = false;
			studio->dim = false;
			if (imported) {
				imported();
			}
		});
}

void ChooseBackground(
		not_null<Studio*> studio,
		QPointer<QWidget> parent) {
	FileDialog::GetOpenPath(
		parent,
		tr::ayu_StyleBackgroundChoose(tr::now),
		FileDialog::ImagesFilter(),
		[=](FileDialog::OpenResult &&result) {
			auto image = result.remoteContent.isEmpty()
				? (result.paths.isEmpty()
					? QImage()
					: QImage(result.paths.front()))
				: QImage::fromData(result.remoteContent);
			if (!image.isNull()) {
				studio->background = std::move(image);
				studio->changed.fire({});
			}
		});
}

void EditColor(
		not_null<Window::SessionController*> controller,
		not_null<Studio*> studio,
		int index) {
	const auto member = kFields[index].member;
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(FieldTitle(index));
		const auto editor = box->addRow(object_ptr<ColorEditor>(
			box,
			ColorEditor::Mode::RGBA,
			studio->colors.*member));
		box->setWidth(editor->width());
		const auto save = [=] {
			studio->colors.*member = editor->color();
			studio->changed.fire({});
			box->closeBox();
		};
		editor->submitRequests(
		) | rpl::on_next(save, editor->lifetime());
		box->setFocusCallback([=] { editor->setInnerFocus(); });
		box->addButton(tr::lng_settings_save(), save);
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	}));
}

void ChooseFont(
		not_null<Window::SessionController*> controller,
		not_null<Studio*> studio) {
	const auto current = studio->fontChanged
		? studio->font
		: Core::App().settings().customFontFamily();
	const auto bg = studio->colors.bg;
	const auto generateBg = [=] {
		const auto size = st::boxWidth;
		const auto ratio = style::DevicePixelRatio();
		auto result = QImage(
			QSize(size, size) * ratio,
			QImage::Format_ARGB32_Premultiplied);
		result.fill(bg);
		return result;
	};
	const auto save = [=](QString chosen) {
		studio->font = chosen;
		studio->fontChanged = true;
		studio->changed.fire({});
	};
	controller->show(Box(Ui::ChooseFontBox, generateBg, current, save));
}

void AddPreview(
		not_null<Ui::VerticalLayout*> container,
		not_null<Studio*> studio) {
	const auto widget = container->add(
		object_ptr<Ui::RpWidget>(container),
		st::boxRowPadding);
	widget->resize(st::boxWideWidth, style::ConvertScale(kPreviewHeight));
	const auto cache = widget->lifetime().make_state<QImage>();
	const auto refreshCache = [=] {
		*cache = PreparedBackground(studio);
	};
	refreshCache();

	widget->paintRequest() | rpl::on_next([=] {
		const auto px = [](int value) {
			return style::ConvertScale(value);
		};
		auto p = QPainter(widget);
		auto hq = PainterHighQualityEnabler(p);
		const auto &c = studio->colors;
		const auto [radiusValue, corners] = ShapeValues(studio->shape);
		const auto radius = px((radiusValue >= 0)
			? radiusValue
			: AyuSettings::getInstance().messageBubbleRadius());
		const auto full = widget->rect();
		const auto listWidth = full.width() / 3;
		const auto header = px(28);

		const auto chat = QRect(
			listWidth,
			header,
			full.width() - listWidth,
			full.height() - header);
		if (!cache->isNull()) {
			p.drawImage(chat, *cache, cache->rect());
		} else {
			p.fillRect(chat, c.bg);
		}

		p.fillRect(QRect(0, 0, listWidth, full.height()), c.surface);
		const auto rowHeight = px(34);
		for (auto i = 0; i != 4; ++i) {
			const auto row = QRect(
				0,
				px(6) + i * rowHeight,
				listWidth,
				rowHeight);
			if (i == 1) {
				p.fillRect(row, Mix(c.surface, c.accent, 0.25));
			}
			const auto userpic = QRect(
				row.x() + px(8),
				row.y() + px(5),
				rowHeight - px(10),
				rowHeight - px(10));
			const auto userpicRadius = (corners >= 0)
				? (userpic.width() * corners
					/ (2. * AyuUiSettings::kMaxAvatarCorners))
				: userpic.width() / 2.;
			p.setPen(Qt::NoPen);
			p.setBrush(Mix(c.accent, c.subtext, 0.3 * i));
			p.drawRoundedRect(userpic, userpicRadius, userpicRadius);
			const auto lineLeft = userpic.right() + px(8);
			p.setBrush(c.text);
			p.drawRoundedRect(
				QRect(lineLeft, row.y() + px(9), listWidth / 3, px(5)),
				px(2),
				px(2));
			p.setBrush(c.subtext);
			p.drawRoundedRect(
				QRect(lineLeft, row.y() + px(19), listWidth / 2, px(4)),
				px(2),
				px(2));
		}

		p.fillRect(QRect(listWidth, 0, chat.width(), header), c.surface);
		p.setBrush(c.text);
		p.drawRoundedRect(
			QRect(listWidth + px(12), px(11), px(70), px(6)),
			px(3),
			px(3));

		p.setFont(st::normalFont);
		const auto bubbleWidth = px(150);
		const auto bubbleHeight = px(30);
		const auto bubble = [&](QRect rect, QColor color, const QString &t) {
			p.setPen(Qt::NoPen);
			p.setBrush(color);
			p.drawRoundedRect(rect, radius, radius);
			p.setPen(c.text);
			p.drawText(
				QRectF(rect.marginsRemoved(QMargins(px(10), 0, px(10), 0))),
				t,
				QTextOption(Qt::AlignVCenter | Qt::AlignLeft));
		};
		bubble(
			QRect(
				listWidth + px(12),
				header + px(16),
				bubbleWidth,
				bubbleHeight),
			c.in,
			tr::ayu_StylePreviewIn(tr::now));
		bubble(
			QRect(
				full.width() - px(12) - bubbleWidth,
				header + px(56),
				bubbleWidth,
				bubbleHeight),
			c.out,
			tr::ayu_StylePreviewOut(tr::now));

		const auto send = px(22);
		p.setPen(Qt::NoPen);
		p.setBrush(c.accent);
		p.drawEllipse(QRect(
			full.width() - px(12) - send,
			full.height() - px(8) - send,
			send,
			send));
	}, widget->lifetime());

	studio->changed.events() | rpl::on_next([=] {
		refreshCache();
		widget->update();
	}, widget->lifetime());
}

void AddColorButton(
		not_null<Window::SessionController*> controller,
		not_null<Ui::VerticalLayout*> container,
		not_null<Studio*> studio,
		int index) {
	const auto member = kFields[index].member;
	const auto button = container->add(object_ptr<Ui::SettingsButton>(
		container,
		rpl::single(FieldTitle(index)),
		st::settingsButtonNoIcon));
	const auto swatch = Ui::CreateChild<Ui::RpWidget>(button);
	const auto swatchSize = style::ConvertScale(kSwatchSize);
	swatch->resize(swatchSize, swatchSize);
	swatch->setAttribute(Qt::WA_TransparentForMouseEvents);
	button->sizeValue() | rpl::on_next([=](QSize size) {
		swatch->moveToRight(
			st::settingsButtonNoIcon.padding.right(),
			(size.height() - swatchSize) / 2,
			size.width());
	}, swatch->lifetime());
	swatch->paintRequest() | rpl::on_next([=] {
		auto p = QPainter(swatch);
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(QPen(st::windowSubTextFg->c, 1));
		p.setBrush(studio->colors.*member);
		const auto radius = style::ConvertScale(6);
		p.drawRoundedRect(
			QRectF(swatch->rect()).marginsRemoved({ 1., 1., 1., 1. }),
			radius,
			radius);
	}, swatch->lifetime());
	studio->changed.events() | rpl::on_next([=] {
		swatch->update();
	}, swatch->lifetime());
	button->setClickedCallback([=] {
		EditColor(controller, studio, index);
	});
}

} // namespace

void ShowBox(not_null<Window::SessionController*> controller) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(tr::ayu_StyleStudioTitle());
		box->setWidth(st::boxWideWidth);

		const auto studio = box->lifetime().make_state<Studio>();
		LoadState(studio);

		const auto content = box->verticalLayout();
		AddPreview(content, studio);
		Ui::AddSkip(content);

		Ui::AddSubsectionTitle(content, tr::ayu_StylePresets());
		const auto presets = std::make_shared<Ui::RadiobuttonGroup>(-1);
		for (auto i = 0; i != int(kPresets.size()); ++i) {
			content->add(
				object_ptr<Ui::Radiobutton>(
					content,
					presets,
					i,
					QString::fromLatin1(kPresets[i].name),
					st::defaultBoxCheckbox),
				st::boxRowPadding);
		}

		Ui::AddSkip(content);
		Ui::AddSubsectionTitle(content, tr::ayu_StyleShape());
		const auto shape = std::make_shared<Ui::RadiobuttonGroup>(
			int(studio->shape));
		const auto shapeTitles = std::array<QString, 4>{ {
			tr::ayu_StyleShapeKeep(tr::now),
			tr::ayu_StyleShapeSharp(tr::now),
			tr::ayu_StyleShapeSoft(tr::now),
			tr::ayu_StyleShapeRound(tr::now),
		} };
		for (auto i = 0; i != int(shapeTitles.size()); ++i) {
			content->add(
				object_ptr<Ui::Radiobutton>(
					content,
					shape,
					i,
					shapeTitles[i],
					st::defaultBoxCheckbox),
				st::boxRowPadding);
		}
		shape->setChangedCallback([=](int value) {
			studio->shape = Shape(value);
			studio->changed.fire({});
		});
		presets->setChangedCallback([=](int value) {
			ApplyPreset(studio, kPresets[value]);
			shape->setValue(int(studio->shape));
			studio->changed.fire({});
		});

		Ui::AddSkip(content);
		Ui::AddSubsectionTitle(content, tr::ayu_StyleColors());
		for (auto i = 0; i != kFieldsCount; ++i) {
			AddColorButton(controller, content, studio, i);
		}

		Ui::AddSkip(content);
		Ui::AddSubsectionTitle(content, tr::ayu_StyleBackground());
		const auto choose = content->add(object_ptr<Ui::SettingsButton>(
			content,
			tr::ayu_StyleBackgroundChoose(),
			st::settingsButtonNoIcon));
		choose->setClickedCallback([=] {
			ChooseBackground(studio, box.get());
		});
		const auto clear = content->add(object_ptr<Ui::SettingsButton>(
			content,
			tr::ayu_StyleBackgroundClear(),
			st::settingsButtonNoIcon));
		clear->setClickedCallback([=] {
			studio->background = QImage();
			studio->changed.fire({});
		});
		const auto blur = content->add(
			object_ptr<Ui::Checkbox>(
				content,
				tr::ayu_StyleBackgroundBlur(tr::now),
				studio->blur,
				st::defaultBoxCheckbox),
			st::boxRowPadding);
		blur->checkedChanges() | rpl::on_next([=](bool checked) {
			studio->blur = checked;
			studio->changed.fire({});
		}, blur->lifetime());
		const auto dim = content->add(
			object_ptr<Ui::Checkbox>(
				content,
				tr::ayu_StyleBackgroundDim(tr::now),
				studio->dim,
				st::defaultBoxCheckbox),
			st::boxRowPadding);
		dim->checkedChanges() | rpl::on_next([=](bool checked) {
			studio->dim = checked;
			studio->changed.fire({});
		}, dim->lifetime());

		Ui::AddSkip(content);
		Ui::AddSubsectionTitle(content, tr::ayu_StyleFont());
		const auto fontLabel = [=] {
			const auto family = studio->fontChanged
				? studio->font
				: Core::App().settings().customFontFamily();
			return family.isEmpty()
				? tr::lng_font_default(tr::now)
				: (family == style::SystemFontTag())
				? tr::lng_font_system(tr::now)
				: family;
		};
		const auto font = content->add(object_ptr<Ui::SettingsButton>(
			content,
			studio->changed.events_starting_with({}) | rpl::map(fontLabel),
			st::settingsButtonNoIcon));
		font->setClickedCallback([=] {
			ChooseFont(controller, studio);
		});

		Ui::AddSkip(content);
		Ui::AddDividerText(content, tr::ayu_StyleAbout());

		const auto imported = [=] {
			shape->setValue(int(studio->shape));
			blur->setChecked(studio->blur);
			dim->setChecked(studio->dim);
			studio->changed.fire({});
		};

		box->addButton(tr::ayu_StyleApply(), [=] {
			Apply(controller, studio);
		});
		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
		box->addLeftButton(tr::ayu_StyleExport(), [=] {
			ExportStyle(controller, studio, box.get());
		});
		box->addLeftButton(tr::ayu_StyleImport(), [=] {
			ImportStyle(controller, studio, box.get(), imported);
		});
	}));
}

} // namespace AyuFeatures::StyleStudio
