// Message templates: typing a key followed by a space in the message field
// replaces it with the saved text. One template per line: "key = text",
// "\n" inside the text becomes a line break.
#pragma once

#include <QtCore/QString>

#include <vector>

namespace Ui {
struct InstantReplaces;
} // namespace Ui

namespace AyuFeatures::Templates {

struct Template {
	QString key;
	QString text;
};

[[nodiscard]] std::vector<Template> Parse(const QString &raw);

// Default emoji/text replaces plus the user's templates.
[[nodiscard]] const Ui::InstantReplaces &InstantReplaces();

} // namespace AyuFeatures::Templates
