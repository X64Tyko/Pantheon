#include "SourceRepository.h"
#include "Database.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <iomanip>
#include <random>
#include <sstream>

namespace {

std::string generateId() {
    thread_local std::mt19937_64 rng(std::random_device{}());
    std::ostringstream ss;
    ss << std::hex << std::setfill('0') << std::setw(16) << rng();
    return ss.str();
}

} // namespace

SourceRepository::SourceRepository(Database& db) : db_(db) {}

std::vector<MediaSourceConfig> SourceRepository::listSources() {
    SQLite::Statement q(db_.get(),
        "SELECT source_id, source_type, display_name, COALESCE(base_url,''), enabled, "
        "       COALESCE(synced_user_id,'') "
        "FROM media_source ORDER BY display_name");
    std::vector<MediaSourceConfig> result;
    while (q.executeStep()) {
        MediaSourceConfig s;
        s.source_id       = q.getColumn(0).getString();
        s.source_type     = q.getColumn(1).getString();
        s.display_name    = q.getColumn(2).getString();
        s.base_url        = q.getColumn(3).getString();
        s.enabled         = q.getColumn(4).getInt() != 0;
        s.synced_user_id  = q.getColumn(5).getString();
        result.push_back(std::move(s));
    }
    return result;
}

void SourceRepository::createSource(const std::string& source_id,
                                     const std::string& source_type,
                                     const std::string& display_name,
                                     const std::string& base_url) {
    SQLite::Statement s(db_.get(),
        "INSERT INTO media_source (source_id, source_type, display_name, base_url) "
        "VALUES (?,?,?,?)");
    s.bind(1, source_id); s.bind(2, source_type);
    s.bind(3, display_name); s.bind(4, base_url);
    s.exec();
}

void SourceRepository::removeSource(const std::string& source_id) {
    SQLite::Transaction txn(db_.get());
    { SQLite::Statement s(db_.get(), "DELETE FROM source_mapping WHERE source_id = ?");
      s.bind(1, source_id); s.exec(); }
    { SQLite::Statement s(db_.get(), "DELETE FROM media_source WHERE source_id = ?");
      s.bind(1, source_id); s.exec(); }
    txn.commit();
}

std::vector<MediaLibraryConfig> SourceRepository::listLibraries(const std::string& source_id) {
    SQLite::Statement q(db_.get(),
        "SELECT library_id, external_lib_id, display_name, library_type, enabled, "
        "       preferred_scraper, preferred_language, include_anidb, show_on_home, skip_scraping "
        "FROM media_library WHERE source_id = ? ORDER BY display_name");
    q.bind(1, source_id);
    std::vector<MediaLibraryConfig> result;
    while (q.executeStep()) {
        MediaLibraryConfig lib;
        lib.library_id          = q.getColumn(0).getString();
        lib.source_id           = source_id;
        lib.external_lib_id     = q.getColumn(1).getString();
        lib.display_name        = q.getColumn(2).getString();
        lib.library_type        = q.getColumn(3).getString();
        lib.enabled             = q.getColumn(4).getInt() != 0;
        lib.preferred_scraper   = q.getColumn(5).getString();
        lib.preferred_language  = q.getColumn(6).getString();
        lib.include_anidb       = q.getColumn(7).getInt() != 0;
        lib.show_on_home        = q.getColumn(8).getInt() != 0;
        lib.skip_scraping       = q.getColumn(9).getInt() != 0;
        result.push_back(std::move(lib));
    }
    return result;
}

std::string SourceRepository::createLibrary(const std::string& source_id,
                                             const std::string& external_lib_id,
                                             const std::string& display_name,
                                             const std::string& library_type,
                                             const std::string& preferred_scraper,
                                             const std::string& preferred_language,
                                             bool include_anidb) {
    std::string library_id = generateId();
    SQLite::Statement s(db_.get(),
        "INSERT INTO media_library "
        "(library_id, source_id, external_lib_id, display_name, library_type, "
        " preferred_scraper, preferred_language, include_anidb) "
        "VALUES (?,?,?,?,?,?,?,?)");
    s.bind(1, library_id); s.bind(2, source_id);
    s.bind(3, external_lib_id); s.bind(4, display_name);
    s.bind(5, library_type);   s.bind(6, preferred_scraper);
    s.bind(7, preferred_language);
    s.bind(8, include_anidb ? 1 : 0);
    s.exec();
    return library_id;
}

void SourceRepository::updateLibrary(const std::string& library_id,
                                      const std::string& display_name,
                                      const std::string& library_type,
                                      const std::string& preferred_scraper,
                                      const std::string& preferred_language,
                                      bool include_anidb,
                                      bool skip_scraping) {
    SQLite::Statement s(db_.get(),
        "UPDATE media_library "
        "SET display_name = ?, library_type = ?, preferred_scraper = ?, preferred_language = ?, "
        "    include_anidb = ?, skip_scraping = ? "
        "WHERE library_id = ?");
    s.bind(1, display_name);
    s.bind(2, library_type);
    s.bind(3, preferred_scraper);
    s.bind(4, preferred_language);
    s.bind(5, include_anidb ? 1 : 0);
    s.bind(6, skip_scraping ? 1 : 0);
    s.bind(7, library_id);
    s.exec();
}

void SourceRepository::setLibraryShowOnHome(const std::string& library_id, bool show_on_home) {
    SQLite::Statement s(db_.get(), "UPDATE media_library SET show_on_home = ? WHERE library_id = ?");
    s.bind(1, show_on_home ? 1 : 0);
    s.bind(2, library_id);
    s.exec();
}

std::vector<std::string> SourceRepository::getScraperPriority(const std::string& library_id,
                                                                const std::string& item_type) {
    std::vector<std::string> out;
    SQLite::Statement q(db_.get(),
        "SELECT source FROM library_scraper_priority WHERE library_id = ? AND item_type = ? ORDER BY priority ASC");
    q.bind(1, library_id);
    q.bind(2, item_type);
    while (q.executeStep()) out.push_back(q.getColumn(0).getString());
    return out;
}

void SourceRepository::setScraperPriority(const std::string& library_id, const std::string& item_type,
                                           const std::vector<std::string>& order) {
    SQLite::Transaction txn(db_.get());
    {
        SQLite::Statement d(db_.get(),
            "DELETE FROM library_scraper_priority WHERE library_id = ? AND item_type = ?");
        d.bind(1, library_id);
        d.bind(2, item_type);
        d.exec();
    }
    SQLite::Statement ins(db_.get(),
        "INSERT INTO library_scraper_priority (library_id, item_type, source, priority) VALUES (?, ?, ?, ?)");
    int priority = 1;
    for (const auto& source : order) {
        if (source.empty()) continue;
        ins.bind(1, library_id);
        ins.bind(2, item_type);
        ins.bind(3, source);
        ins.bind(4, priority++);
        ins.exec();
        ins.reset();
    }
    txn.commit();
}

void SourceRepository::removeLibrary(const std::string& library_id) {
    // No transaction: avoids "cannot start a transaction within a transaction" when sync is mid-write on the same connection.
    { SQLite::Statement s(db_.get(), "DELETE FROM source_mapping WHERE library_id = ?");
      s.bind(1, library_id); s.exec(); }
    { SQLite::Statement s(db_.get(), "DELETE FROM media_library WHERE library_id = ?");
      s.bind(1, library_id); s.exec(); }
}

std::optional<MediaLibraryConfig> SourceRepository::getLibrary(const std::string& library_id) {
    SQLite::Statement q(db_.get(),
        "SELECT library_id, source_id, external_lib_id, display_name, library_type, enabled, "
        "       preferred_scraper, preferred_language, include_anidb, show_on_home, skip_scraping "
        "FROM media_library WHERE library_id = ?");
    q.bind(1, library_id);
    if (q.executeStep()) {
        MediaLibraryConfig lib;
        lib.library_id          = q.getColumn(0).getString();
        lib.source_id           = q.getColumn(1).getString();
        lib.external_lib_id     = q.getColumn(2).getString();
        lib.display_name        = q.getColumn(3).getString();
        lib.library_type        = q.getColumn(4).getString();
        lib.enabled             = q.getColumn(5).getInt() != 0;
        lib.preferred_scraper   = q.getColumn(6).getString();
        lib.preferred_language  = q.getColumn(7).getString();
        lib.include_anidb       = q.getColumn(8).getInt() != 0;
        lib.show_on_home        = q.getColumn(9).getInt() != 0;
        lib.skip_scraping       = q.getColumn(10).getInt() != 0;
        return lib;
    }
    return std::nullopt;
}

std::string SourceRepository::resolveKairosId(const std::string& source_id,
                                               const std::string& external_id,
                                               const std::string& item_type) {
    SQLite::Statement q(db_.get(),
        "SELECT kairos_id FROM source_mapping "
        "WHERE source_id = ? AND external_id = ? AND item_type = ?");
    q.bind(1, source_id); q.bind(2, external_id); q.bind(3, item_type);
    return q.executeStep() ? q.getColumn(0).getString() : "";
}

std::optional<std::string> SourceRepository::samplePath(const std::string& source_id) {
    SQLite::Statement q(db_.get(), R"(
        SELECT e.file_path FROM episode e
        JOIN source_mapping sm ON sm.kairos_id = e.episode_id AND sm.item_type = 'episode'
        JOIN media_library ml ON ml.library_id = sm.library_id
        WHERE ml.source_id = ? AND e.file_path != ''
        LIMIT 1
    )");
    q.bind(1, source_id);
    if (q.executeStep()) return q.getColumn(0).getString();

    SQLite::Statement mq(db_.get(), R"(
        SELECT m.file_path FROM movie m
        JOIN source_mapping sm ON sm.kairos_id = m.movie_id AND sm.item_type = 'movie'
        JOIN media_library ml ON ml.library_id = sm.library_id
        WHERE ml.source_id = ? AND m.file_path != ''
        LIMIT 1
    )");
    mq.bind(1, source_id);
    if (mq.executeStep()) return mq.getColumn(0).getString();

    return std::nullopt;
}

std::vector<SourceRepository::SourceBasicRow> SourceRepository::listSourcesBasic() {
    SQLite::Statement q(db_.get(),
        "SELECT source_id, source_type, display_name FROM media_source ORDER BY display_name");
    std::vector<SourceBasicRow> rows;
    while (q.executeStep())
        rows.push_back({q.getColumn(0).getString(), q.getColumn(1).getString(),
                        q.getColumn(2).getString()});
    return rows;
}

std::optional<SourceRepository::SourceMappingRow>
SourceRepository::getSourceMapping(const std::string& kairos_id) {
    SQLite::Statement q(db_.get(),
        "SELECT source_id, external_id FROM source_mapping WHERE kairos_id = ? LIMIT 1");
    q.bind(1, kairos_id);
    if (!q.executeStep()) return std::nullopt;
    return SourceMappingRow{q.getColumn(0).getString(), q.getColumn(1).getString()};
}

std::string SourceRepository::getExternalLibId(const std::string& library_id,
                                                const std::string& source_id) {
    SQLite::Statement q(db_.get(),
        "SELECT external_lib_id FROM media_library WHERE library_id = ? AND source_id = ?");
    q.bind(1, library_id); q.bind(2, source_id);
    return q.executeStep() ? q.getColumn(0).getString() : "";
}

std::optional<SourceRepository::ResolvedSource>
SourceRepository::resolveItemSource(const std::string& item_type,
                                     const std::string& kairos_id) {
    std::string sql;
    if (item_type == "episode") {
        sql = R"(
            SELECT sm.source_id, sm.external_id, e.file_path
            FROM source_mapping sm
            JOIN episode e ON e.episode_id = sm.kairos_id
            WHERE sm.item_type='episode' AND sm.kairos_id=?
            LIMIT 1
        )";
    } else {
        sql = R"(
            SELECT sm.source_id, sm.external_id, m.file_path
            FROM source_mapping sm
            JOIN movie m ON m.movie_id = sm.kairos_id
            WHERE sm.item_type='movie' AND sm.kairos_id=?
            LIMIT 1
        )";
    }
    SQLite::Statement q(db_.get(), sql);
    q.bind(1, kairos_id);
    if (!q.executeStep()) return std::nullopt;
    return ResolvedSource{
        q.getColumn(0).getString(),
        q.getColumn(1).getString(),
        q.getColumn(2).getString(),
    };
}

std::string SourceRepository::getSourceBaseUrl(const std::string& source_id) {
    SQLite::Statement q(db_.get(),
        "SELECT base_url FROM media_source WHERE source_id = ?");
    q.bind(1, source_id);
    return q.executeStep() ? q.getColumn(0).getString() : "";
}

std::optional<MediaSourceConfig> SourceRepository::getSource(const std::string& source_id) {
    SQLite::Statement q(db_.get(),
        "SELECT source_id, source_type, display_name, COALESCE(base_url,''), enabled, "
        "       COALESCE(synced_user_id,'') "
        "FROM media_source WHERE source_id = ?");
    q.bind(1, source_id);
    if (!q.executeStep()) return std::nullopt;
    MediaSourceConfig s;
    s.source_id      = q.getColumn(0).getString();
    s.source_type    = q.getColumn(1).getString();
    s.display_name   = q.getColumn(2).getString();
    s.base_url       = q.getColumn(3).getString();
    s.enabled        = q.getColumn(4).getInt() != 0;
    s.synced_user_id = q.getColumn(5).getString();
    return s;
}

std::string SourceRepository::getExternalId(const std::string& source_id,
                                             const std::string& kairos_id,
                                             const std::string& item_type) {
    SQLite::Statement q(db_.get(),
        "SELECT external_id FROM source_mapping "
        "WHERE source_id = ? AND kairos_id = ? AND item_type = ?");
    q.bind(1, source_id); q.bind(2, kairos_id); q.bind(3, item_type);
    return q.executeStep() ? q.getColumn(0).getString() : "";
}

std::vector<SourceRepository::WritebackTarget> SourceRepository::getWritebackTargets(
    const std::string& item_type, const std::string& kairos_id) {
    SQLite::Statement q(db_.get(), R"(
        SELECT ms.source_id, ms.source_type, ms.base_url,
               sm.external_id, COALESCE(ml.external_lib_id, ''), ms.display_name
        FROM source_mapping sm
        JOIN media_source ms ON ms.source_id = sm.source_id
        LEFT JOIN media_library ml ON ml.library_id = sm.library_id
        WHERE sm.item_type = ? AND sm.kairos_id = ?
    )");
    q.bind(1, item_type);
    q.bind(2, kairos_id);

    std::vector<WritebackTarget> out;
    while (q.executeStep()) {
        out.push_back({
            q.getColumn(0).getString(),
            q.getColumn(1).getString(),
            q.getColumn(2).getString(),
            q.getColumn(3).getString(),
            q.getColumn(4).getString(),
            q.getColumn(5).getString(),
        });
    }
    return out;
}

void SourceRepository::upsertSourceUsers(const std::string& source_id,
                                          const std::vector<SourceUserInfo>& users,
                                          int64_t now) {
    SQLite::Statement s(db_.get(), R"(
        INSERT INTO source_user (source_id, external_user_id, display_name, email, last_seen_at)
        VALUES (?, ?, ?, ?, ?)
        ON CONFLICT(source_id, external_user_id) DO UPDATE SET
            display_name = excluded.display_name,
            email        = excluded.email,
            last_seen_at = excluded.last_seen_at
    )");
    for (const auto& u : users) {
        s.bind(1, source_id);
        s.bind(2, u.external_user_id);
        s.bind(3, u.display_name);
        s.bind(4, u.email);
        s.bind(5, now);
        s.exec();
        s.reset();
    }
}

std::optional<SourceRepository::SourceUserRow>
SourceRepository::getSourceUser(const std::string& source_id, const std::string& external_user_id) {
    SQLite::Statement q(db_.get(),
        "SELECT display_name, email, COALESCE(imported_user_id,'') "
        "FROM source_user WHERE source_id = ? AND external_user_id = ?");
    q.bind(1, source_id);
    q.bind(2, external_user_id);
    if (!q.executeStep()) return std::nullopt;
    return SourceUserRow{q.getColumn(0).getString(), q.getColumn(1).getString(), q.getColumn(2).getString()};
}

std::vector<SourceRepository::UnmappedSourceUserRow> SourceRepository::listUnmappedSourceUsers() {
    SQLite::Statement q(db_.get(), R"(
        SELECT su.source_id, ms.display_name, su.external_user_id, su.display_name, su.email
        FROM source_user su
        JOIN media_source ms ON ms.source_id = su.source_id
        WHERE su.imported_user_id IS NULL
        ORDER BY ms.display_name, su.display_name
    )");
    std::vector<UnmappedSourceUserRow> out;
    while (q.executeStep()) {
        out.push_back({
            q.getColumn(0).getString(),
            q.getColumn(1).getString(),
            q.getColumn(2).getString(),
            q.getColumn(3).getString(),
            q.getColumn(4).getString(),
        });
    }
    return out;
}

void SourceRepository::setImportedUserId(const std::string& source_id,
                                          const std::string& external_user_id,
                                          const std::string& user_id) {
    SQLite::Statement s(db_.get(),
        "UPDATE source_user SET imported_user_id = ? WHERE source_id = ? AND external_user_id = ?");
    s.bind(1, user_id);
    s.bind(2, source_id);
    s.bind(3, external_user_id);
    s.exec();
}

void SourceRepository::setSyncedUserId(const std::string& source_id, const std::string& user_id) {
    SQLite::Statement s(db_.get(), "UPDATE media_source SET synced_user_id = ? WHERE source_id = ?");
    if (user_id.empty()) s.bind(1); else s.bind(1, user_id);
    s.bind(2, source_id);
    s.exec();
}

std::string SourceRepository::getSyncedUserId(const std::string& source_id) {
    SQLite::Statement q(db_.get(), "SELECT synced_user_id FROM media_source WHERE source_id = ?");
    q.bind(1, source_id);
    if (!q.executeStep() || q.getColumn(0).isNull()) return "";
    return q.getColumn(0).getString();
}
