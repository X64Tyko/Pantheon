#pragma once
#include <array>
#include <cstdint>
#include <ctime>
#include <ostream>
#include <string>
#include <vector>

// Minimal dependency-free ZIP writer — STORED (uncompressed) entries only, no
// external library. Log files are already bounded (LogBuffer::kMaxFileSize,
// 10MB) and text, so skipping deflate costs little size for the sake of not
// pulling in zlib/miniz/libzip as a new project dependency just for an
// admin-only diagnostics export. Writes a spec-valid ZIP (local file headers
// + central directory + end-of-central-directory record) any standard tool
// can open. Shared by kairos, hermes and hephaestus — see shared/README.md.
class ZipWriter
{
public:
	// name: the path shown inside the archive (e.g. "kairos.log"). content:
	// raw bytes for that entry.
	void addFile(const std::string& name, const std::string& content)
	{
		entries_.push_back({name, content});
	}

	// Writes the complete archive to out. Safe to call once (entries_ isn't
	// consumed, but there's no reason to call it twice).
	void write(std::ostream& out) const
	{
		uint16_t dos_time, dos_date;
		dosNow(dos_time, dos_date);

		std::vector<uint32_t> offsets;
		std::vector<uint32_t> crcs;
		offsets.reserve(entries_.size());
		crcs.reserve(entries_.size());

		uint32_t offset = 0;
		for (const auto& [name, content] : entries_)
		{
			offsets.push_back(offset);
			uint32_t crc = crc32(content);
			crcs.push_back(crc);

			offset += writeLocalHeader(out, name, content, crc, dos_time, dos_date);
			out.write(content.data(), static_cast<std::streamsize>(content.size()));
			offset += static_cast<uint32_t>(content.size());
		}

		uint32_t central_start = offset;
		for (size_t i = 0; i < entries_.size(); ++i)
		{
			offset += writeCentralHeader(out, entries_[i].first, entries_[i].second,
										 crcs[i], offsets[i], dos_time, dos_date);
		}
		uint32_t central_size = offset - central_start;

		writeEndOfCentralDirectory(out, static_cast<uint16_t>(entries_.size()), central_size, central_start);
	}

private:
	std::vector<std::pair<std::string, std::string>> entries_;

	static void put16(std::ostream& out, uint16_t v)
	{
		char b[2] = {static_cast<char>(v & 0xFF), static_cast<char>((v >> 8) & 0xFF)};
		out.write(b, 2);
	}

	static void put32(std::ostream& out, uint32_t v)
	{
		char b[4] = {
			static_cast<char>(v & 0xFF), static_cast<char>((v >> 8) & 0xFF),
			static_cast<char>((v >> 16) & 0xFF), static_cast<char>((v >> 24) & 0xFF)
		};
		out.write(b, 4);
	}

	static void dosNow(uint16_t& dos_time, uint16_t& dos_date)
	{
		std::time_t t = std::time(nullptr);
		std::tm tm{};
		localtime_r(&t, &tm);
		dos_time = static_cast<uint16_t>((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2));
		dos_date = static_cast<uint16_t>(((tm.tm_year + 1900 - 1980) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday);
	}

	// Standard CRC-32 (polynomial 0xEDB88320), table computed once per call —
	// log files are small enough (bounded at 10MB) that this isn't worth
	// caching a static table for.
	static uint32_t crc32(const std::string& data)
	{
		std::array<uint32_t, 256> table{};
		for (uint32_t i = 0; i < 256; ++i)
		{
			uint32_t c = i;
			for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
			table[i] = c;
		}
		uint32_t crc = 0xFFFFFFFFu;
		for (unsigned char ch : data) crc = table[(crc ^ ch) & 0xFF] ^ (crc >> 8);
		return crc ^ 0xFFFFFFFFu;
	}

	// Returns bytes written (header + filename), for offset bookkeeping.
	static uint32_t writeLocalHeader(std::ostream& out, const std::string& name, const std::string& content,
									 uint32_t crc, uint16_t dos_time, uint16_t dos_date)
	{
		put32(out, 0x04034b50);
		put16(out, 20); // version needed
		put16(out, 0);  // flags
		put16(out, 0);  // compression: stored
		put16(out, dos_time);
		put16(out, dos_date);
		put32(out, crc);
		put32(out, static_cast<uint32_t>(content.size())); // compressed size == uncompressed for stored
		put32(out, static_cast<uint32_t>(content.size()));
		put16(out, static_cast<uint16_t>(name.size()));
		put16(out, 0); // extra field length
		out.write(name.data(), static_cast<std::streamsize>(name.size()));
		return 30 + static_cast<uint32_t>(name.size());
	}

	static uint32_t writeCentralHeader(std::ostream& out, const std::string& name, const std::string& content,
									   uint32_t crc, uint32_t local_offset, uint16_t dos_time, uint16_t dos_date)
	{
		put32(out, 0x02014b50);
		put16(out, 20); // version made by
		put16(out, 20); // version needed
		put16(out, 0);  // flags
		put16(out, 0);  // compression: stored
		put16(out, dos_time);
		put16(out, dos_date);
		put32(out, crc);
		put32(out, static_cast<uint32_t>(content.size()));
		put32(out, static_cast<uint32_t>(content.size()));
		put16(out, static_cast<uint16_t>(name.size()));
		put16(out, 0); // extra field length
		put16(out, 0); // comment length
		put16(out, 0); // disk number start
		put16(out, 0); // internal attributes
		put32(out, 0); // external attributes
		put32(out, local_offset);
		out.write(name.data(), static_cast<std::streamsize>(name.size()));
		return 46 + static_cast<uint32_t>(name.size());
	}

	static void writeEndOfCentralDirectory(std::ostream& out, uint16_t count, uint32_t central_size, uint32_t central_offset)
	{
		put32(out, 0x06054b50);
		put16(out, 0); // disk number
		put16(out, 0); // disk with central dir start
		put16(out, count);
		put16(out, count);
		put32(out, central_size);
		put32(out, central_offset);
		put16(out, 0); // comment length
	}
};