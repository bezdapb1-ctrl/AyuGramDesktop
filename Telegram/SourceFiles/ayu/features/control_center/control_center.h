#pragma once

namespace Window {
class SessionController;
} // namespace Window

namespace AyuFeatures::ControlCenter {

enum class Tab {
	Ghost,
	Search,
	Links,
	Sending,
};

void Show(
	not_null<Window::SessionController*> controller,
	Tab tab = Tab::Ghost);

} // namespace AyuFeatures::ControlCenter
