#include "TitleMatch.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <vector>

namespace titlematch {

std::string normalizeTitle(const std::string& s) {
    std::string r;
    r.reserve(s.size());
    for (char c : s) r += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const auto* art : { "the ", "a ", "an " }) {
        if (r.starts_with(art)) { r = r.substr(std::strlen(art)); break; }
    }
    std::string out;
    for (char c : r) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == ' ') out += c;
    }
    return out;
}

// Levenshtein similarity [0,1]
double titleSimilarity(const std::string& a, const std::string& b) {
    std::string na = normalizeTitle(a), nb = normalizeTitle(b);
    if (na == nb) return 1.0;
    if (na.empty() || nb.empty()) return 0.0;

    const size_t m = na.size(), n = nb.size();
    std::vector<std::vector<int>> dp(m + 1, std::vector<int>(n + 1, 0));
    for (size_t i = 0; i <= m; ++i) dp[i][0] = static_cast<int>(i);
    for (size_t j = 0; j <= n; ++j) dp[0][j] = static_cast<int>(j);
    for (size_t i = 1; i <= m; ++i) {
        for (size_t j = 1; j <= n; ++j) {
            int cost = (na[i - 1] == nb[j - 1]) ? 0 : 1;
            dp[i][j] = std::min({ dp[i-1][j]+1, dp[i][j-1]+1, dp[i-1][j-1]+cost });
        }
    }
    int dist = dp[m][n];
    int maxLen = static_cast<int>(std::max(m, n));
    return 1.0 - static_cast<double>(dist) / maxLen;
}

} // namespace titlematch
