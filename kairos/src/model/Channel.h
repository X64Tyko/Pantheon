#pragma once
#include <optional>
#include <string>

struct Channel
{
	std::string channel_id;
	std::string name;
	int number = 0;
	// NULL = admin-owned (today's default, unchanged). Set to the creating
	// guest/viewer's user_id for a self-service channel (see ChannelAuth.h).
	std::optional<std::string> owner_user_id;
	// Only meaningful when owner_user_id is set: true for a guest's
	// throwaway demo channel (excluded from the real lineup), false for a
	// real viewer's persistent one (a full lineup citizen).
	bool is_demo                         = false;
	std::string timezone                 = "UTC";
	std::string advance_mode             = "scheduled";
	std::string default_filler_selection = "round_robin";
	int rerun_min_time_mins              = 0;
	int seed                             = 12345;
	std::string offline_video_path;
	std::string offline_image_path;
	std::string offline_audio_id;
	std::string offline_audio_type;
	std::string offline_audio_title;
	std::string logo_path;
	std::string anchor_hashes; // JSON blob, may be empty
	std::string audio_lang;    // preferred audio language, e.g. "eng"; empty = default
	std::string subtitle_lang; // preferred subtitle language; empty = none
	// Per-channel transcode quality settings (Hephaestus)
	std::string stream_resolution = "source"; // "source"|"1080p"|"720p"|"480p"
	int stream_video_bitrate      = 0;        // kbps; 0 = CRF/CQ auto
	int stream_audio_bitrate      = 192;      // kbps
	// Disables Hephaestus's native/direct-stream bucket for this channel
	// entirely — every viewer gets the transcode bucket, which can smoothly
	// drift-correct via speed (direct-stream can only skip/seek, coarser) and
	// always had loudnorm. Trades away direct-stream's CPU/GPU savings for
	// those guarantees. Default false: existing channels unaffected.
	bool force_transcode = false;
	// Parental controls — admin-assigned rating ceiling for this channel
	// (TV-scale, e.g. "TV-MA"), evaluated against a restricted account's
	// max_channel_rating the same way a show's content_rating is. Empty =
	// unrated, which fails closed for a restricted account (see RatingSeverity.h).
	std::string content_tag;
};