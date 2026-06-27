#pragma once
#include "../model/Chapter.h"
#include <cstdint>
#include <string>
#include <vector>

// Returns a validated duration_ms for a media file.
//
// If dur is already in [1 000, 86 400 000] ms (1 s – 24 h) it is returned
// unchanged.  Otherwise ffprobe is invoked on file_path to obtain the real
// container duration.  If ffprobe succeeds and the result is in range, the
// probed value is returned; if not, 0 is returned and a warning is emitted.
//
// Intended to be called before writing duration_ms to the DB so that stale,
// missing, or corrupt source metadata does not silently produce zero-duration
// items that stall the scheduler.
int64_t validateDurationMs(int64_t dur, const std::string& file_path);

// Reads chapter markers embedded in a media file via ffprobe.
// Returns chapters in position order with source="file" and chapter_type="unclassified".
// media_type and media_id are left empty — caller fills them in.
// Returns empty vector if ffprobe fails or the file has no chapters.
std::vector<Chapter> probeChapters(const std::string& file_path);
