// Game mode: while a full screen game is in front, AyuGram turns on the
// built-in power saving (no animations, autoplaying stickers or GIFs) and
// lowers its own process priority so the game gets the CPU first.
#pragma once

#include "rpl/producer.h"

namespace AyuFeatures::GameMode {

void start();

[[nodiscard]] rpl::producer<bool> activeValue();

} // namespace AyuFeatures::GameMode
