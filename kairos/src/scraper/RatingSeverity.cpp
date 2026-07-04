#include "RatingSeverity.h"
#include <algorithm>
#include <cctype>
#include <vector>

namespace {

std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    return s;
}

// Case-insensitive lookup; index in `scale` defines the severity value.
// Not found (unrecognized/blank) returns the same severity as the last entry
// ("Adult", the top of the scale) rather than something beyond it — a
// restricted account whose ceiling is deliberately set all the way up should
// see unrated content the same way it sees explicitly-"Adult" content, not
// more strictly. Fail-closed already falls out of this: any ceiling below
// the top blocks both alike.
int lookup(const std::string& rating, const std::vector<const char*>& scale) {
    const std::string r = upper(rating);
    for (size_t i = 0; i < scale.size(); ++i)
        if (r == scale[i]) return static_cast<int>(i);
    return static_cast<int>(scale.size()) - 1;
}

} // namespace

namespace RatingSeverity {

int tvRatingSeverity(const std::string& rating) {
    static const std::vector<const char*> kScale = {
        "TV-Y", "TV-Y7", "TV-G", "TV-PG", "TV-14", "TV-MA", "ADULT",
    };
    return lookup(rating, kScale);
}

int movieRatingSeverity(const std::string& rating) {
    static const std::vector<const char*> kScale = {
        "G", "PG", "PG-13", "R", "NC-17", "ADULT",
    };
    return lookup(rating, kScale);
}

} // namespace RatingSeverity
