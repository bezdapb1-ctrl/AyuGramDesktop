#pragma once

#include "ui/widgets/fields/input_field.h"

class History;
class HistoryItem;
class QPainter;

namespace HistoryView {
class Element;
} // namespace HistoryView

namespace AyuFeatures::UndoSend {

struct Restore {
	not_null<History*> history;
	TextWithTags text;
};

// Holds an outgoing message for a few seconds so it can still be taken back.
[[nodiscard]] bool Intercept(
	not_null<History*> history,
	uint64 randomId,
	Fn<void()> send);

[[nodiscard]] bool IsPending(not_null<const HistoryItem*> item);
void PaintBadge(
	QPainter &p,
	not_null<const HistoryView::Element*> view,
	int top);
[[nodiscard]] bool HandleClick(not_null<History*> history, QPoint point);
bool CancelLast(not_null<History*> history);
[[nodiscard]] rpl::producer<Restore> Restores();

} // namespace AyuFeatures::UndoSend
