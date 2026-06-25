#pragma once
#include <SQLiteCpp/SQLiteCpp.h>
#include <cstdint>
#include <string>

// Column-to-type conversions used by repositories.
namespace db {

template<typename T> T colAs(const SQLite::Column& c);
template<> inline std::string colAs<std::string>(const SQLite::Column& c) { return c.getString(); }
template<> inline int         colAs<int>        (const SQLite::Column& c) { return c.getInt(); }
template<> inline int64_t     colAs<int64_t>    (const SQLite::Column& c) { return c.getInt64(); }
template<> inline double      colAs<double>     (const SQLite::Column& c) { return c.getDouble(); }
template<> inline bool        colAs<bool>       (const SQLite::Column& c) { return c.getInt() != 0; }

} // namespace db
