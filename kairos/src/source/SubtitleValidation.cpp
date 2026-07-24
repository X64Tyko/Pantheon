#include "SubtitleValidation.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

namespace {

std::string lowerExt(const std::string& path) {
	auto ext = std::filesystem::path(path).extension().string();
	std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
	return ext;
}

// Minimum real cue/dialogue lines to consider a file a genuine subtitle
// track rather than a stub/placeholder — real episodes routinely have
// hundreds of cues; a legitimately terse forced-only track still clears
// this easily, so the bar is set low enough to only catch the "basically
// empty" case, not flag real-but-short files.
constexpr int kMinCues = 2;

// Reads up to this many bytes — plenty to establish "this file has real
// content" without pulling a multi-megabyte ASS file entirely into memory
// just to count Dialogue: lines.
constexpr size_t kMaxReadBytes = 2 * 1024 * 1024;

int countOccurrences(const std::string& haystack, const std::string& needle) {
	int count = 0;
	size_t pos = 0;
	while ((pos = haystack.find(needle, pos)) != std::string::npos) {
		++count;
		pos += needle.size();
	}
	return count;
}

// .ass/.ssa "Dialogue:" lines only count at the actual start of a line
// (case-sensitive, matches the format spec) — a loose substring search
// would also match the word turning up inside some other line's own text.
int countLineStartMatches(const std::string& content, const std::string& prefix) {
	int count = 0;
	size_t pos = 0;
	while (pos < content.size()) {
		size_t line_end = content.find('\n', pos);
		size_t line_len = line_end == std::string::npos ? std::string::npos : line_end - pos;
		if (content.compare(pos, std::min(line_len, prefix.size()), prefix) == 0) ++count;
		if (line_end == std::string::npos) break;
		pos = line_end + 1;
	}
	return count;
}

} // namespace

SubtitleValidationResult validateSubtitleFile(const std::string& path) {
	std::ifstream f(path, std::ios::binary);
	if (!f) return { false, "file could not be opened" };

	std::string content(kMaxReadBytes, '\0');
	f.read(content.data(), static_cast<std::streamsize>(content.size()));
	content.resize(static_cast<size_t>(f.gcount()));

	if (content.empty()) return { false, "file is empty" };

	const std::string ext = lowerExt(path);
	int cues;
	if (ext == ".ass" || ext == ".ssa") {
		cues = countLineStartMatches(content, "Dialogue:");
	} else {
		// .srt/.vtt (the only other extensions scanDirectoryForSubtitles
		// recognizes) both delimit every cue with "-->" between its start
		// and end timestamps.
		cues = countOccurrences(content, "-->");
	}

	if (cues < kMinCues)
		return { false, "found only " + std::to_string(cues) + " cue(s) in the file - likely empty/corrupt" };
	return { true, "" };
}
