#include "ayu/features/screencast/screencast_quality.h"

#include "ayu/ayu_settings.h"
#include "rpl/lifetime.h"

#include "tgcalls/desktop_capturer/DesktopCaptureSource.h"

namespace AyuFeatures::Screencast {
namespace {

rpl::lifetime Lifetime;
bool Started = false;

[[nodiscard]] tgcalls::DesktopCaptureOverrides ForQuality(int quality) {
	switch (quality) {
	case 1: return { .fps = 30, .width = 1920, .height = 1080, .maxBitrateKbps = 2500 };
	case 2: return { .fps = 60, .width = 1920, .height = 1080, .maxBitrateKbps = 5000 };
	}
	return {}; // Library defaults: 24 fps, 1280x720.
}

} // namespace

void start() {
	if (Started) {
		return;
	}
	Started = true;
	AyuSettings::getInstance().screencastQualityValue(
	) | rpl::on_next([=](int quality) {
		tgcalls::SetDesktopCaptureOverrides(ForQuality(quality));
	}, Lifetime);
}

} // namespace AyuFeatures::Screencast
