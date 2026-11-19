// Local names: rename any user, group or channel for yourself only.
// The name is kept on this device and replaces the real one everywhere
// in AyuGram; the real name stays visible in the rename box.
#pragma once

class PeerData;

namespace Ui::Menu {
struct MenuCallback;
} // namespace Ui::Menu

namespace Window {
class SessionController;
} // namespace Window

namespace AyuFeatures::LocalNames {

// Called with every name that comes from the server.
[[nodiscard]] QString Apply(not_null<PeerData*> peer, const QString &name);

void AddMenuAction(
	PeerData *peer,
	not_null<Window::SessionController*> controller,
	const Ui::Menu::MenuCallback &addAction);

} // namespace AyuFeatures::LocalNames
