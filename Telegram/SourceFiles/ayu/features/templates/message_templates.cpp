#include "ayu/features/templates/message_templates.h"

#include "ayu/ayu_settings.h"
#include "ui/widgets/fields/input_field.h"

namespace AyuFeatures::Templates {

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

const Ui::InstantReplaces &InstantReplaces() {
	static auto cachedRaw = QString();
	static auto cached = Ui::InstantReplaces::Default();
	static auto initialized = false;

	const auto &raw = AyuSettings::getInstance().messageTemplates();
	if (!initialized || raw != cachedRaw) {
		initialized = true;
		cachedRaw = raw;
		cached = Ui::InstantReplaces::Default();
		for (const auto &one : Parse(raw)) {
			// The trailing space is the trigger, so ";cs" does not fire
			// while typing a longer word that starts the same way.
			cached.add(one.key + ' ', one.text);
		}
	}
	return cached;
}

} // namespace AyuFeatures::Templates
