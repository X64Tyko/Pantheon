#include <gtest/gtest.h>
#include "stream/MediaProbe.h"

TEST(MediaProbeTest, PickAudioTrack) {
    MediaInfo info;
    
    AudioTrack a1;
    a1.relative_index = 0;
    a1.language = "eng";
    info.audio.push_back(a1);
    
    AudioTrack a2;
    a2.relative_index = 1;
    a2.language = "jpn";
    info.audio.push_back(a2);

    // Preferred language match
    EXPECT_EQ(pickAudioTrack(info, "jpn"), 1);
    EXPECT_EQ(pickAudioTrack(info, "eng"), 0);
    
    // Partial match (ISO 639-1 vs 639-2)
    // The current implementation uses .substr(0, 3) on both, which means
    // "ja" vs "jpn" will NOT match because "ja".substr(0,3) is "ja" and "jpn".substr(0,3) is "jpn".
    // This test confirms the current (strict) behavior.
    EXPECT_EQ(pickAudioTrack(info, "ja"), 0); 
}

TEST(MediaProbeTest, PickAudioTrackFallback) {
    MediaInfo info;
    AudioTrack a1;
    a1.relative_index = 0;
    a1.language = "eng";
    info.audio.push_back(a1);

    // No match -> fallback to 0
    EXPECT_EQ(pickAudioTrack(info, "fra"), 0);
    
    // Empty preferred -> fallback to 0
    EXPECT_EQ(pickAudioTrack(info, ""), 0);
    
    // No tracks at all
    MediaInfo emptyInfo;
    EXPECT_EQ(pickAudioTrack(emptyInfo, "eng"), 0);
}

TEST(MediaProbeTest, PickSubtitleTrack) {
    MediaInfo info;
    
    SubtitleTrack s1;
    s1.relative_index = 0;
    s1.language = "eng";
    info.subtitles.push_back(s1);
    
    SubtitleTrack s2;
    s2.relative_index = 1;
    s2.language = "spa";
    info.subtitles.push_back(s2);

    EXPECT_EQ(pickSubtitleTrack(info, "spa"), 1);
    EXPECT_EQ(pickSubtitleTrack(info, "eng"), 0);
    
    // No match -> -1
    EXPECT_EQ(pickSubtitleTrack(info, "fra"), -1);
    
    // Empty preferred -> -1
    EXPECT_EQ(pickSubtitleTrack(info, ""), -1);
}
