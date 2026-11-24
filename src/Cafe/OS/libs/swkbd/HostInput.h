#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>

namespace swkbd::host
{
	// Guest の上限は UTF-16 コード単位。孤立サロゲートと制御文字を返さない。
	inline std::u16string Sanitize(std::u16string_view text, size_t limit)
	{
		std::u16string result;
		limit = std::min<size_t>(limit, 4095);
		for (size_t i = 0; i < text.size() && text[i] && result.size() < limit; ++i)
		{
			const auto c = text[i];
			if (c >= 0xd800 && c <= 0xdbff)
			{
				if (i + 1 < text.size() && text[i + 1] >= 0xdc00 && text[i + 1] <= 0xdfff)
				{
					if (result.size() + 2 > limit) break;
					result += c;
					result += text[++i];
				}
			}
			else if (c >= 32 && !(c >= 0xdc00 && c <= 0xdfff) && c != 127)
				result += c;
		}
		return result;
	}

	enum class Action { Change, Accept, Cancel };
	struct Snapshot
	{
		uint64_t generation = 0, revision = 0;
		bool active = false, pending = false;
		unsigned limit = 0, cursor = 0;
		std::u16string text;
	};

	// UI はコピーだけを扱う。Guest バッファと callback は SwkbdCalc が更新する。
	void Enable(bool enabled);
	bool IsEnabled();
	Snapshot Read();
	bool Submit(uint64_t generation, std::u16string text, Action action);
}
