#include "ayu/features/game_mode/game_mode.h"

#include "ayu/ayu_settings.h"
#include "base/timer.h"
#include "rpl/variable.h"

#ifdef Q_OS_WIN
#include "platform/win/windows_dlls.h"
#endif // Q_OS_WIN

namespace AyuFeatures::GameMode {
namespace {

constexpr auto kCheckInterval = crl::time(2000);

rpl::variable<bool> Active = false;
base::Timer *CheckTimer = nullptr;
bool PriorityLowered = false;

[[nodiscard]] bool FullscreenAppInFront() {
#ifdef Q_OS_WIN
	using namespace Platform;
	if (!Dlls::SHQueryUserNotificationState) {
		return false;
	}
	auto state = QUERY_USER_NOTIFICATION_STATE();
	if (FAILED(Dlls::SHQueryUserNotificationState(&state))) {
		return false;
	}
	if (state != QUNS_RUNNING_D3D_FULL_SCREEN && state != QUNS_BUSY) {
		return false;
	}
	// Our own full screen media viewer is not a game.
	auto pid = DWORD(0);
	GetWindowThreadProcessId(GetForegroundWindow(), &pid);
	return pid && (pid != GetCurrentProcessId());
#else // Q_OS_WIN
	return false;
#endif // Q_OS_WIN
}

void SetLowPriority(bool low) {
#ifdef Q_OS_WIN
	if (PriorityLowered == low) {
		return;
	}
	PriorityLowered = low;
	SetPriorityClass(
		GetCurrentProcess(),
		low ? BELOW_NORMAL_PRIORITY_CLASS : NORMAL_PRIORITY_CLASS);
#endif // Q_OS_WIN
}

void Check() {
	const auto active = AyuSettings::getInstance().gameMode()
		&& FullscreenAppInFront();
	SetLowPriority(active);
	Active = active;
}

} // namespace

void start() {
	if (CheckTimer) {
		return;
	}
	CheckTimer = new base::Timer(Check);
	CheckTimer->callEach(kCheckInterval);
	Check();
}

rpl::producer<bool> activeValue() {
	return Active.value();
}

} // namespace AyuFeatures::GameMode
