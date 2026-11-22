#pragma once

class PeerData;

namespace Main {
class Session;
} // namespace Main

namespace Window {
class SessionController;
} // namespace Window

namespace Ui {
class PopupMenu;
} // namespace Ui

namespace AyuFeatures::GhostRules {

struct Profile {
	QString id;
	QString name;
	bool hideRead = false;
	bool hideTyping = false;
	bool hideActions = false;
	bool builtin = false;
};

[[nodiscard]] std::vector<Profile> Profiles();
[[nodiscard]] std::optional<Profile> FindProfile(const QString &id);
void SaveCustomProfile(const Profile &profile);
void RemoveCustomProfile(const QString &id);

[[nodiscard]] QString ProfileIdFor(not_null<const PeerData*> peer);
[[nodiscard]] std::optional<Profile> ProfileFor(not_null<const PeerData*> peer);
void SetProfile(not_null<PeerData*> peer, const QString &profileId);
[[nodiscard]] std::vector<std::pair<PeerId, QString>> Rules(
	not_null<Main::Session*> session);
[[nodiscard]] rpl::producer<> Changes();

// Per-chat rule first, then the account's global ghost settings.
[[nodiscard]] bool SendRead(not_null<const PeerData*> peer);
[[nodiscard]] bool SendTyping(not_null<const PeerData*> peer);
[[nodiscard]] bool SendActions(not_null<const PeerData*> peer);
[[nodiscard]] bool ReadAfterReply(not_null<const PeerData*> peer);

void FillMenu(
	not_null<Ui::PopupMenu*> menu,
	not_null<Window::SessionController*> controller,
	not_null<PeerData*> peer);
[[nodiscard]] QString ProfileTitle(const QString &id);

} // namespace AyuFeatures::GhostRules
