// Local pins: once the Telegram pin limit is reached, more chats can be
// pinned in the main list. Those pins are kept only on this device and are
// never sent to the server.
#pragma once

class History;

namespace Data {
class Session;
} // namespace Data

namespace AyuFeatures::LocalPins {

inline constexpr auto kReserve = 500;

[[nodiscard]] bool CanPinLocally(not_null<History*> history);
[[nodiscard]] bool IsLocal(not_null<History*> history);
void Toggle(not_null<History*> history, bool pinned);

// Puts the local pins back after the pinned list or dialogs were loaded.
void Reapply(not_null<Data::Session*> owner, bool serverListApplied);

[[nodiscard]] int CountPinned(not_null<const Data::Session*> owner);

} // namespace AyuFeatures::LocalPins
