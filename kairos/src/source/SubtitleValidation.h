#pragma once
#include <string>

// Cheap, format-aware sanity check for a subtitle sidecar file discovered by
// SubtitleSidecar.h's scanDirectoryForSubtitles — that scan only matches on
// filename convention, so it has no way to notice a file that LOOKS like a
// subtitle track (right name, right extension) but ISN'T one: found in the
// wild, a ".es.srt" containing a single line, which ffmpeg's srt demuxer
// happily "extracts" from — exiting 0 with ~0 real cues — rather than
// erroring, so nothing downstream (charset sniffing, the extraction
// pipeline itself) ever notices either. This is not a real subtitle parser:
// it counts the one structural marker every supported format uses for a
// cue/dialogue line and flags anything implausibly low, rather than
// validating full syntax.
struct SubtitleValidationResult {
	bool        valid = true;
	std::string reason; // empty when valid; human-readable when not (surfaced verbatim in the admin's Review > Subtitles tab)
};

SubtitleValidationResult validateSubtitleFile(const std::string& path);
