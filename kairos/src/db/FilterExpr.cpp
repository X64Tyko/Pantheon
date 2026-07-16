#include "FilterExpr.h"
#include <algorithm>
#include <cctype>
#include <memory>
#include <optional>
#include <unordered_map>

namespace {

// ─── Field registry — mirrors FIELD_DEFS in hades/src/components/media/
//     PickerFilters.tsx. ops[0] is the default used when no comparator
//     matched the typed value. ─────────────────────────────────────────────

struct FieldSpec { std::vector<std::string> ops; };

const std::unordered_map<std::string, FieldSpec>& fieldRegistry() {
    static const std::unordered_map<std::string, FieldSpec> reg = {
        {"source",          {{"is", "is_not"}}},
        {"title",           {{"contains", "does_not_contain", "begins_with", "ends_with", "is", "is_not"}}},
        {"genre",           {{"is", "is_not", "contains", "does_not_contain"}}},
        {"year",            {{"is", "lt", "gt"}}},
        {"content_rating",  {{"is", "is_not", "contains", "does_not_contain"}}},
        {"studio",          {{"contains", "does_not_contain", "begins_with", "ends_with", "is", "is_not"}}},
        {"director",        {{"is", "is_not", "contains", "does_not_contain"}}},
        {"actor",           {{"is", "is_not", "contains", "does_not_contain"}}},
        {"writer",          {{"is", "is_not", "contains", "does_not_contain"}}},
        {"country",         {{"is", "is_not", "contains", "does_not_contain"}}},
        {"collection",      {{"is", "is_not", "contains", "does_not_contain"}}},
        {"network",         {{"is", "is_not", "contains", "does_not_contain"}}},
        {"label",           {{"is", "is_not", "contains", "does_not_contain"}}},
        {"resolution",      {{"is", "is_not"}}},
        {"decade",          {{"is"}}},
        {"audience_rating", {{"gte", "lte", "gt", "lt"}}},
        {"duration",        {{"gte", "lte", "gt", "lt"}}},
        {"added",           {{"in_last", "before", "after"}}},
    };
    return reg;
}

std::optional<std::string> resolveField(const std::string& raw) {
    std::string key;
    key.reserve(raw.size());
    for (char c : raw) key += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (key == "rating") key = "content_rating";
    if (fieldRegistry().count(key)) return key;
    return std::nullopt;
}

bool fieldSupportsOp(const std::string& field, const std::string& op) {
    auto it = fieldRegistry().find(field);
    if (it == fieldRegistry().end()) return false;
    return std::find(it->second.ops.begin(), it->second.ops.end(), op) != it->second.ops.end();
}

std::string defaultOp(const std::string& field) {
    auto it = fieldRegistry().find(field);
    return (it != fieldRegistry().end() && !it->second.ops.empty()) ? it->second.ops[0] : "is";
}

// ─── Tokenizer ──────────────────────────────────────────────────────────────

enum class TokKind { LParen, RParen, And, Or, Not, Word };
struct Token { TokKind kind; std::string text; };

std::string toUpper(const std::string& s) {
    std::string r; r.reserve(s.size());
    for (char c : s) r += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return r;
}

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

std::vector<Token> tokenize(const std::string& src) {
    std::vector<Token> toks;
    size_t i = 0, n = src.size();
    while (i < n) {
        char c = src[i];
        if (isSpace(c)) { ++i; continue; }
        if (c == '(') { toks.push_back({TokKind::LParen, "("}); ++i; continue; }
        if (c == ')') { toks.push_back({TokKind::RParen, ")"}); ++i; continue; }

        bool inQuote = false;
        std::string buf;
        while (i < n) {
            char ch = src[i];
            if (inQuote) {
                buf += ch;
                if (ch == '"') inQuote = false;
                ++i;
                continue;
            }
            if (ch == '"') { inQuote = true; buf += ch; ++i; continue; }
            if (isSpace(ch) || ch == '(' || ch == ')') break;
            buf += ch;
            ++i;
        }
        if (buf.empty()) { ++i; continue; }

        std::string upper = toUpper(buf);
        if (upper == "AND" || upper == "&&") toks.push_back({TokKind::And, buf});
        else if (upper == "OR" || upper == "||") toks.push_back({TokKind::Or, buf});
        else if (upper == "NOT") toks.push_back({TokKind::Not, buf});
        else toks.push_back({TokKind::Word, buf});
    }
    return toks;
}

// ─── AST ────────────────────────────────────────────────────────────────────

struct Node;
using NodePtr = std::shared_ptr<Node>;

struct Node {
    enum class Type { Clause, Word, Group } type;
    // Clause
    std::string field, op, value;
    // Word
    std::string word_value;
    bool word_negate = false;
    // Group
    std::string match; // "all" | "any"
    std::vector<NodePtr> children;
    bool group_negate = false;
};

NodePtr makeClause(std::string field, std::string op, std::string value) {
    auto n = std::make_shared<Node>();
    n->type = Node::Type::Clause; n->field = std::move(field); n->op = std::move(op); n->value = std::move(value);
    return n;
}
NodePtr makeWord(std::string value, bool negate = false) {
    auto n = std::make_shared<Node>();
    n->type = Node::Type::Word; n->word_value = std::move(value); n->word_negate = negate;
    return n;
}
NodePtr makeGroup(std::string match, std::vector<NodePtr> children, bool negate = false) {
    auto n = std::make_shared<Node>();
    n->type = Node::Type::Group; n->match = std::move(match); n->children = std::move(children); n->group_negate = negate;
    return n;
}

std::string stripQuotes(const std::string& s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') return s.substr(1, s.size() - 2);
    return s;
}

NodePtr negateNode(const NodePtr& node) {
    if (node->type == Node::Type::Clause) {
        if (node->op == "is")       return makeClause(node->field, "is_not", node->value);
        if (node->op == "contains") return makeClause(node->field, "does_not_contain", node->value);
        // No natural negated counterpart for this op — wrap generically.
        return makeGroup("all", {node}, true);
    }
    if (node->type == Node::Type::Word)  return makeWord(node->word_value, !node->word_negate);
    return makeGroup(node->match, node->children, !node->group_negate);
}

// Attempts FIELD:VALUE. Returns nullptr (caller falls back to a plain word)
// if the prefix before the first ':' isn't a known field/alias.
NodePtr tryParseClause(const std::string& raw) {
    auto colon = raw.find(':');
    if (colon == std::string::npos || colon == 0) return nullptr;
    auto field = resolveField(raw.substr(0, colon));
    if (!field) return nullptr;

    std::string valuePart = raw.substr(colon + 1);
    static const std::vector<std::pair<std::string, std::string>> cmpPrefixes = {
        {">=", "gte"}, {"<=", "lte"}, {">", "gt"}, {"<", "lt"},
    };
    std::string op;

    if (*field == "added") {
        // 'added' doesn't support generic lt/gt/gte/lte (its real ops are
        // in_last/before/after), so it's handled entirely separately from
        // the generic comparator-prefix loop below:
        //   added:<30d / 30d / 30   -> in_last, N days (unit-less = days)
        //   added:>2024-01-01       -> after 2024-01-01
        //   added:<2024-01-01       -> before 2024-01-01
        bool hasCmp = !valuePart.empty() && (valuePart.front() == '<' || valuePart.front() == '>');
        std::string body = hasCmp ? valuePart.substr(1) : valuePart;
        char last = body.empty() ? '\0' : static_cast<char>(std::tolower(static_cast<unsigned char>(body.back())));
        bool hasUnit = last == 'd' || last == 'w' || last == 'm' || last == 'y';
        std::string numPart = hasUnit ? body.substr(0, body.size() - 1) : body;
        bool allDigits = !numPart.empty() && std::all_of(numPart.begin(), numPart.end(), [](unsigned char c){ return std::isdigit(c); });
        if (allDigits) {
            int n = std::stoi(numPart);
            int mult = !hasUnit ? 1 : last == 'd' ? 1 : last == 'w' ? 7 : last == 'm' ? 30 : 365;
            op = "in_last";
            valuePart = std::to_string(n * mult);
        } else if (hasCmp) {
            op = valuePart.front() == '>' ? "after" : "before";
            valuePart = body;
        }
    }

    // Wildcard markers for contains/begins_with/ends_with — there's no
    // comparator-symbol equivalent for these (only gt/gte/lt/lte get one),
    // so the frontend serializer (filterSyntax.ts) emits *value*/value*/
    // *value for them instead; mirrored here so the backend parses the same
    // canon syntax it's handed identically. Skipped for an explicitly
    // quoted value ("field:\"*literal*\"") — quoting means take it exactly,
    // asterisks included, not "treat as a wildcard."
    if (op.empty() && (valuePart.empty() || valuePart.front() != '"')) {
        bool startsStar = !valuePart.empty() && valuePart.front() == '*';
        bool endsStar   = valuePart.size() > 1 && valuePart.back() == '*';
        if (startsStar && endsStar && fieldSupportsOp(*field, "contains")) {
            op = "contains";
            valuePart = valuePart.substr(1, valuePart.size() - 2);
        } else if (endsStar && fieldSupportsOp(*field, "begins_with")) {
            op = "begins_with";
            valuePart = valuePart.substr(0, valuePart.size() - 1);
        } else if (startsStar && fieldSupportsOp(*field, "ends_with")) {
            op = "ends_with";
            valuePart = valuePart.substr(1);
        }
    }

    if (op.empty()) {
        for (const auto& [sym, id] : cmpPrefixes) {
            if (valuePart.size() >= sym.size() && valuePart.compare(0, sym.size(), sym) == 0 && fieldSupportsOp(*field, id)) {
                op = id;
                valuePart = valuePart.substr(sym.size());
                break;
            }
        }
    }

    std::string value = stripQuotes(valuePart);

    if (op.empty() || !fieldSupportsOp(*field, op)) op = defaultOp(*field);

    if (*field == "decade" && !value.empty() && (value.back() == 's' || value.back() == 'S'))
        value.pop_back();

    return makeClause(*field, op, value);
}

// ─── Parser (recursive descent) ─────────────────────────────────────────────

class Parser {
public:
    explicit Parser(std::vector<Token> toks) : toks_(std::move(toks)) {}

    NodePtr parseExpr() { return parseOr(); }

private:
    std::vector<Token> toks_;
    size_t pos_ = 0;

    const Token* peek() const { return pos_ < toks_.size() ? &toks_[pos_] : nullptr; }
    const Token* next() { return pos_ < toks_.size() ? &toks_[pos_++] : nullptr; }

    NodePtr parseOr() {
        auto first = parseAnd();
        std::vector<NodePtr> children{first};
        while (peek() && peek()->kind == TokKind::Or) { next(); children.push_back(parseAnd()); }
        return children.size() == 1 ? first : makeGroup("any", children);
    }

    NodePtr parseAnd() {
        auto first = parseTerm();
        std::vector<NodePtr> children{first};
        for (;;) {
            const Token* t = peek();
            if (!t) break;
            if (t->kind == TokKind::Or || t->kind == TokKind::RParen) break;
            if (t->kind == TokKind::And) next();
            children.push_back(parseTerm());
        }
        return children.size() == 1 ? first : makeGroup("all", children);
    }

    NodePtr parseTerm() {
        const Token* t = peek();
        if (!t) return makeWord("");

        if (t->kind == TokKind::Not) { next(); return negateNode(parseTerm()); }
        if (t->kind == TokKind::LParen) {
            next();
            auto inner = parseExpr();
            if (peek() && peek()->kind == TokKind::RParen) next();
            return inner;
        }

        next();
        std::string raw = t->text;
        bool negate = false;
        if (!raw.empty() && raw.front() == '-' && raw.size() > 1) { negate = true; raw = raw.substr(1); }

        if (auto clause = tryParseClause(raw)) return negate ? negateNode(clause) : clause;
        auto word = makeWord(stripQuotes(raw), negate);
        return word;
    }
};

NodePtr parseFilterAst(const std::string& text) {
    auto toks = tokenize(text);
    if (toks.empty()) return makeGroup("all", {});
    return Parser(toks).parseExpr();
}

// ─── Compiler: AST → SQL fragment + binds ──────────────────────────────────

std::string likeEscape(const std::string& v) { return v; } // values are bound as params, no literal escaping needed

struct Compiler {
    FilterEntity entity;
    std::string alias;
    std::vector<std::string> binds;

    std::string col(const std::string& name) const { return alias + "." + name; }

    // JSON-array fields: genre/label/actor/country/collection.
    std::string jsonCol(const std::string& field) const {
        static const std::unordered_map<std::string, std::string> map = {
            {"genre", "genres"}, {"label", "labels"}, {"actor", "actors"},
            {"country", "countries"}, {"collection", "collections"},
        };
        return map.at(field);
    }
    bool isJsonField(const std::string& field) const {
        return field == "genre" || field == "label" || field == "actor" || field == "country" || field == "collection";
    }

    // Every fragment returned here is wrapped in its own parens by the
    // caller (compileGroup) before being AND/OR-joined, so internal
    // connectives (decade's range, resolution's EXISTS, etc.) are always
    // precedence-safe regardless of what the parent group does with siblings.
    std::string compileClause(const Node& n) {
        const std::string& f = n.field;
        const std::string& op = n.op;

        if (f == "director" || f == "writer") {
            if (entity == FilterEntity::Show) return "1=0"; // no such concept for shows
            std::string c = col(f);
            if (op == "is")               { binds.push_back(n.value); return c + " = ?"; }
            if (op == "is_not")           { binds.push_back(n.value); return c + " != ?"; }
            if (op == "contains")         { binds.push_back(n.value); return c + " LIKE '%' || ? || '%'"; }
            if (op == "does_not_contain") { binds.push_back(n.value); return "(" + c + " IS NULL OR " + c + " NOT LIKE '%' || ? || '%')"; }
        }

        if (f == "source") {
            // Which media source (Plex/Jellyfin/Local/Emby, by source_id —
            // not source_type, since a user can have more than one Plex
            // server) an item is mapped from. Unlike 'library', there's no
            // separate dedicated UI/param for this (library_ids has
            // SourceSwitcher.tsx on the Library page), so it's a normal
            // canon-syntax clause, fully AND/OR/negatable like any other
            // field, rather than extracted out of the filter string.
            std::string kairos_id_col = entity == FilterEntity::Show ? col("show_id") : col("movie_id");
            std::string item_type_lit = entity == FilterEntity::Show ? "'show'" : "'movie'";
            std::string sub = "SELECT 1 FROM source_mapping sm_src WHERE sm_src.kairos_id = " + kairos_id_col +
                               " AND sm_src.item_type = " + item_type_lit + " AND sm_src.source_id = ?";
            binds.push_back(n.value);
            return (op == "is_not" ? "NOT EXISTS (" : "EXISTS (") + sub + ")";
        }

        if (isJsonField(f)) {
            std::string c = col(jsonCol(f));
            std::string tag = "fx_" + std::to_string(binds.size());
            if (op == "is") {
                binds.push_back(n.value);
                return "EXISTS (SELECT 1 FROM json_each(NULLIF(" + c + ",'')) " + tag + " WHERE " + tag + ".value = ?)";
            }
            if (op == "is_not") {
                binds.push_back(n.value);
                return "NOT EXISTS (SELECT 1 FROM json_each(NULLIF(" + c + ",'')) " + tag + " WHERE " + tag + ".value = ?)";
            }
            if (op == "contains") {
                binds.push_back(n.value);
                return "EXISTS (SELECT 1 FROM json_each(NULLIF(" + c + ",'')) " + tag + " WHERE " + tag + ".value LIKE '%' || ? || '%')";
            }
            if (op == "does_not_contain") {
                binds.push_back(n.value);
                return "NOT EXISTS (SELECT 1 FROM json_each(NULLIF(" + c + ",'')) " + tag + " WHERE " + tag + ".value LIKE '%' || ? || '%')";
            }
        }

        if (f == "network") {
            // Movies have no network column at all (only shows do — see
            // Database.cpp's ALTER TABLE show ADD COLUMN network) — same
            // constant-false treatment as director/writer on shows, just
            // inverted. Referencing m.network directly would throw "no such
            // column" and silently drop the whole query (Promise.all
            // rejects, LibraryStore.fetch()'s catch leaves stale results on
            // screen looking like "the filter did nothing").
            if (entity == FilterEntity::Movie) return "1=0";
        }

        if (f == "content_rating" || f == "network" || f == "studio" || f == "title") {
            std::string c = f == "title" ? col("title") : col(f);
            if (op == "is")               { binds.push_back(n.value); return c + " = ?"; }
            if (op == "is_not")           { binds.push_back(n.value); return "(" + c + " IS NULL OR " + c + " != ?)"; }
            if (op == "contains")         { binds.push_back(n.value); return c + " LIKE '%' || ? || '%'"; }
            if (op == "does_not_contain") { binds.push_back(n.value); return "(" + c + " IS NULL OR " + c + " NOT LIKE '%' || ? || '%')"; }
            if (op == "begins_with")      { binds.push_back(n.value); return c + " LIKE ? || '%'"; }
            if (op == "ends_with")        { binds.push_back(n.value); return c + " LIKE '%' || ?"; }
        }

        if (f == "year") {
            std::string c = col("year");
            if (op == "is") { binds.push_back(n.value); return c + " = CAST(? AS INTEGER)"; }
            if (op == "lt") { binds.push_back(n.value); return c + " < CAST(? AS INTEGER)"; }
            if (op == "gt") { binds.push_back(n.value); return c + " > CAST(? AS INTEGER)"; }
        }

        if (f == "audience_rating") {
            std::string c = col("audience_rating");
            if (op == "gte") { binds.push_back(n.value); return c + " >= CAST(? AS REAL)"; }
            if (op == "lte") { binds.push_back(n.value); return c + " <= CAST(? AS REAL)"; }
            if (op == "gt")  { binds.push_back(n.value); return c + " > CAST(? AS REAL)"; }
            if (op == "lt")  { binds.push_back(n.value); return c + " < CAST(? AS REAL)"; }
        }

        if (f == "duration") {
            // Value is minutes (see PickerFilters.tsx's placeholder); compare
            // against duration_ms. Shows have no single duration column — use
            // the average episode runtime via a scalar subquery so this stays
            // a plain WHERE-safe boolean expression (no HAVING needed).
            std::string expr = entity == FilterEntity::Movie
                ? col("duration_ms")
                : "(SELECT AVG(e_dur.duration_ms) FROM episode e_dur WHERE e_dur.show_id = " + col("show_id") + ")";
            std::string cmp = op == "gte" ? ">=" : op == "lte" ? "<=" : op == "gt" ? ">" : "<";
            binds.push_back(n.value);
            return "(" + expr + " " + cmp + " (CAST(? AS REAL) * 60000))";
        }

        if (f == "decade") {
            binds.push_back(n.value);
            binds.push_back(n.value);
            std::string c = col("year");
            return "(" + c + " >= CAST(? AS INTEGER) AND " + c + " < CAST(? AS INTEGER) + 10)";
        }

        if (f == "resolution") {
            if (entity == FilterEntity::Movie) {
                std::string c = col("resolution_label");
                if (op == "is_not") { binds.push_back(n.value); return "(" + c + " IS NULL OR " + c + " != ?)"; }
                binds.push_back(n.value);
                return c + " = ?";
            }
            std::string sub = "SELECT 1 FROM episode e_res WHERE e_res.show_id = " + col("show_id") + " AND e_res.resolution_label = ?";
            binds.push_back(n.value);
            return (op == "is_not" ? "NOT EXISTS (" : "EXISTS (") + sub + ")";
        }

        if (f == "added") {
            std::string c = col("added_at");
            if (op == "in_last") { binds.push_back(n.value); return c + " >= strftime('%s','now') - (CAST(? AS INTEGER) * 86400)"; }
            if (op == "before")  { binds.push_back(n.value); return c + " < strftime('%s', ?)"; }
            if (op == "after")   { binds.push_back(n.value); return c + " > strftime('%s', ?)"; }
        }

        return "1=1"; // unrecognized (field,op) combo — no-op rather than a broken query
    }

    std::string searchableColsSql() const {
        // OR-across-fields fragment for a single fuzzy word. Mirrors the
        // field set already used by searchShows/searchMovies's free-text `q`.
        std::vector<std::string> parts = { col("title") + " LIKE '%' || ? || '%'" };
        if (entity == FilterEntity::Show) parts.push_back(col("network") + " LIKE '%' || ? || '%'");
        parts.push_back(col("studio") + " LIKE '%' || ? || '%'");
        if (entity == FilterEntity::Movie) {
            parts.push_back(col("director") + " LIKE '%' || ? || '%'");
            parts.push_back(col("writer") + " LIKE '%' || ? || '%'");
        }
        for (const char* jc : {"labels", "genres", "actors", "countries", "collections"}) {
            std::string tag = std::string("fw_") + jc;
            parts.push_back("EXISTS (SELECT 1 FROM json_each(NULLIF(" + col(jc) + ",'')) " + tag +
                             " WHERE " + tag + ".value LIKE '%' || ? || '%')");
        }
        std::string sql = "(";
        for (size_t i = 0; i < parts.size(); ++i) sql += (i ? " OR " : "") + parts[i];
        sql += ")";
        return sql;
    }

    std::string compileWord(const Node& n) {
        std::string sql = searchableColsSql();
        int count = static_cast<int>(std::count(sql.begin(), sql.end(), '?'));
        for (int i = 0; i < count; ++i) binds.push_back(n.word_value);
        return n.word_negate ? "NOT " + sql : sql;
    }

    std::string compileGroup(const Node& n) {
        if (n.children.empty()) return "1=1";
        std::string joiner = n.match == "any" ? " OR " : " AND ";
        std::string sql;
        for (size_t i = 0; i < n.children.size(); ++i) {
            std::string childSql = "(" + compileNode(*n.children[i]) + ")";
            sql += (i ? joiner : "") + childSql;
        }
        return n.group_negate ? "NOT (" + sql + ")" : sql;
    }

    std::string compileNode(const Node& n) {
        switch (n.type) {
            case Node::Type::Clause: return compileClause(n);
            case Node::Type::Word:   return compileWord(n);
            case Node::Type::Group:  return compileGroup(n);
        }
        return "1=1";
    }
};

} // namespace

FilterCompileResult compileFilterExpr(const std::string& text, FilterEntity entity, const std::string& alias) {
    FilterCompileResult result;
    if (text.empty()) return result;

    auto ast = parseFilterAst(text);
    Compiler compiler{entity, alias, {}};
    std::string sql = compiler.compileNode(*ast);
    if (sql.empty()) sql = "1=1";
    result.sql = sql;
    result.binds = std::move(compiler.binds);
    return result;
}
