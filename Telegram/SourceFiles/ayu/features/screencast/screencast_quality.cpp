#include "ayu/features/screencast/screencast_quality.h"

#include "ayu/ayu_settings.h"
#include "base/timer.h"
#include "logs.h"
#include "rpl/lifetime.h"

#include "tgcalls/desktop_capturer/DesktopCaptureSource.h"

namespace AyuFeatures::Screencast {
namespace {

constexpr auto kStatsInterval = crl::time(5000);

rpl::lifetime Lifetime;
bool Started = false;
base::Timer *StatsTimer = nullptr;
long long LastFrames = 0;
crl::time LastTime = 0;

[[nodiscard]] tgcalls::DesktopCaptureOverrides ForQuality(int quality) {
	switch (quality) {
	case 1: return { // Smooth: frame rate first.
		.fps = 60,
		.width = 1280,
		.height = 720,
		.maxBitrateKbps = 4000,
		.startBitrateKbps = 2000,
		.degradation = 1,
	};
	case 2: return { // Maximum: 1080p60, balanced under load.
		.fps = 60,
		.width = 1920,
		.height = 1080,
		.maxBitrateKbps = 6000,
		.startBitrateKbps = 3000,
		.degradation = 2,
	};
	}
	return {}; // Library defaults: 24 fps, 1280x720, ~1 Mbps.
}

// Writes the real capture rate to log.txt while a screen is shared.
void LogStats() {
	const auto stats = tgcalls::GetDesktopCaptureStats();
	const auto now = crl::now();
	const auto frames = stats.frames - LastFrames;
	const auto elapsed = now - LastTime;
	LastFrames = stats.frames;
	LastTime = now;
	if (frames <= 0 || elapsed <= 0) {
		return;
	}
	LOG(("Ayu Screencast: %1 fps captured, %2x%3, send estimate %4 kbps, "
		"quality %5"
		).arg(QString::number(frames * 1000. / elapsed, 'f', 1)
		).arg(stats.width
		).arg(stats.height
		).arg(stats.sendBitrateKbps
		).arg(AyuSettings::getInstance().screencastQuality()));
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

	LastTime = crl::now();
	StatsTimer = new base::Timer(LogStats);
	StatsTimer->callEach(kStatsInterval);
}

} // namespace AyuFeatures::Screencast
