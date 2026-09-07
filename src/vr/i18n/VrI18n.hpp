#pragma once

#include <string>
#include <vector>

namespace GakumasVrI18n {
	const char* ts(const std::string& key);

	// Every string any menu language can display. The VR menu builds its
	// font atlas from the union so a mid-session language switch cannot
	// fall back to '?' in headset.
	std::vector<std::string> ActiveStrings();
}
