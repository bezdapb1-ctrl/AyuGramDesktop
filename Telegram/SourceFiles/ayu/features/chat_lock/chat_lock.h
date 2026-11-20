#pragma once

class PeerData;

namespace Ui::Menu {
struct MenuCallback;
} // namespace Ui::Menu

namespace Window {
class SessionController;
} // namespace Window

namespace AyuFeatures::ChatLock {

[[nodiscard]] bool IsLockedChat(not_null<const PeerData*> peer);

[[nodiscard]] bool NeedsUnlock(not_null<const PeerData*> peer);

void RequestUnlock(
	not_null<Window::SessionController*> controller,
	not_null<PeerData*> peer,
	Fn<void()> done);

void AddMenuAction(
	PeerData *peer,
	not_null<Window::SessionController*> controller,
	const Ui::Menu::MenuCallback &addAction);

} // namespace AyuFeatures::ChatLock
