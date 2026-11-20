#include "ayu/features/templates/message_templates.h"

#include "ayu/ayu_settings.h"
#include "base/unique_qptr.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/popup_menu.h"
#include "styles/style_widgets.h"

#include <QtGui/QTextCursor>
#include <QtWidgets/QTextEdit>

#include <algorithm>

namespace AyuFeatures::Templates {
namespace {

constexpr auto kMenuTrigger = QStringView(u";;");
constexpr auto kMenuTriggerLength = 2;
constexpr auto kPreviewLength = 40;

struct FieldState {
	int length = 0;
	bool applying = false;
	base::unique_qptr<Ui::PopupMenu> menu;
};

[[nodiscard]] const std::vector<Template> &Current() {
	static auto cachedRaw = QString();
	static auto cached = std::vector<Template>();
	static auto ready = false;

	const auto &raw = AyuSettings::getInstance().messageTemplates();
	if (!ready || raw != cachedRaw) {
		ready = true;
		cachedRaw = raw;
		cached = Parse(raw);
		std::stable_sort(begin(cached), end(cached), [](
				const Template &a,
				const Template &b) {
			return a.key.size() > b.key.size();
		});
	}
	return cached;
}

[[nodiscard]] int LongestKey(const std::vector<Template> &list) {
	auto result = 0;
	for (const auto &one : list) {
		result = std::max(result, int(one.key.size()));
	}
	return result;
}

[[nodiscard]] QString TextBeforeCursor(
		not_null<Ui::InputField*> field,
		int count) {
	const auto cursor = field->textCursor();
	if (cursor.hasSelection()) {
		return QString();
	}
	const auto position = cursor.position();
	auto look = QTextCursor(cursor);
	look.setPosition(std::max(position - count, 0));
	look.setPosition(position, QTextCursor::KeepAnchor);
	return look.selectedText();
}

[[nodiscard]] bool IsBoundary(const QString &text, int index) {
	return (index < 0) || text[index].isSpace();
}

void Replace(
		not_null<Ui::InputField*> field,
		not_null<FieldState*> state,
		int length,
		const QString &with) {
	const auto position = field->textCursor().position();
	state->applying = true;
	field->commitInstantReplacement(position - length, position, with, {});
	state->applying = false;
	state->length = int(field->getLastText().size());
}

[[nodiscard]] QString Preview(const QString &text) {
	auto result = text;
	result.replace('\n', ' ');
	return (result.size() > kPreviewLength)
		? (result.left(kPreviewLength - 1) + QChar(0x2026))
		: result;
}

void ShowMenu(
		not_null<Ui::InputField*> field,
		not_null<FieldState*> state,
		const std::vector<Template> &list) {
	state->menu = base::make_unique_q<Ui::PopupMenu>(
		field,
		st::defaultPopupMenu);
	for (const auto &one : list) {
		const auto text = one.text;
		state->menu->addAction(
			one.key + u"  —  "_q + Preview(one.text),
			[=] {
				if (TextBeforeCursor(field, kMenuTriggerLength)
						== kMenuTrigger) {
					Replace(field, state, kMenuTriggerLength, text);
				}
				field->setFocus();
			});
	}
	const auto edit = field->rawTextEdit();
	const auto rect = edit->cursorRect();
	state->menu->popup(edit->mapToGlobal(rect.bottomLeft()));
}

void Check(not_null<Ui::InputField*> field, not_null<FieldState*> state) {
	const auto length = int(field->getLastText().size());
	const auto typedOne = (length == state->length + 1);
	state->length = length;
	if (state->applying || !typedOne) {
		return;
	}
	const auto &list = Current();
	if (list.empty()) {
		return;
	}
	const auto before = TextBeforeCursor(field, LongestKey(list) + 2);
	if (before.isEmpty()) {
		return;
	}

	if (before.endsWith(kMenuTrigger)
		&& IsBoundary(before, int(before.size()) - kMenuTriggerLength - 1)) {
		ShowMenu(field, state, list);
		return;
	}

	if (!before.endsWith(' ')) {
		return;
	}
	const auto typed = before.left(before.size() - 1);
	for (const auto &one : list) {
		if (typed.endsWith(one.key)
			&& IsBoundary(typed, int(typed.size() - one.key.size()) - 1)) {
			Replace(field, state, int(one.key.size()) + 1, one.text);
			return;
		}
	}
}

} // namespace

std::vector<Template> Parse(const QString &raw) {
	auto result = std::vector<Template>();
	for (const auto &line : raw.split('\n')) {
		const auto separator = line.indexOf('=');
		if (separator <= 0) {
			continue;
		}
		const auto key = line.left(separator).trimmed();
		auto text = line.mid(separator + 1).trimmed();
		if (key.isEmpty() || key.contains(' ') || text.isEmpty()) {
			continue;
		}
		text.replace(u"\\n"_q, u"\n"_q);
		result.push_back({ key, text });
	}
	return result;
}

void Attach(not_null<Ui::InputField*> field) {
	const auto state = field->lifetime().make_state<FieldState>();
	state->length = int(field->getLastText().size());
	field->changes(
	) | rpl::on_next([=] {
		Check(field, state);
	}, field->lifetime());
}

} // namespace AyuFeatures::Templates
