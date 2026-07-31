// Implements ADR-001-shellrunner-grammar-and-dispatch.md §3's recursive-descent parser.
//
// Word/atom scanning (`scan_atoms`) is implemented as a plain iterative loop, NOT as
// mutually-recursive helper functions. §3/finding 12 anticipated a possible implementation shape
// where nested `${...}`/quoted/literal atom parsing calls back into itself recursively and sized
// the shared depth counter to bound that case too ("31 nested `if`s wrapping a word with 31
// nested `${...}`"). This grammar's `word := (literal | '$' NAME | '${' NAME '}' | quoted)+` is a
// flat Kleene-plus over atoms with no way to nest one `${...}` inside another (`NAME` is a plain
// identifier), so an iterative scan is both sufficient and strictly safer than a recursive one:
// there is no native-stack cost per atom at all, regardless of how many atoms a word contains.
// The only genuinely recursive productions in this grammar are `if`/`for`, because their bodies
// (`command_line`) can themselves contain another `if`/`for` — `parse_if`/`parse_for` are the
// only two places `depth_` is touched, via the `DepthGuard` RAII type below, which increments on
// entry and unconditionally decrements on every exit path (including early returns on syntax
// errors) — closing the "does decrement actually happen on every return path" half of finding 12
// that a monotonic (never-decremented) counter would fail identically against the "open, never
// closed" `if`-10000-deep probe alone.

#include "backends/native_jail/shell_parser.hpp"

#include <array>
#include <cctype>
#include <new>
#include <span>
#include <string>
#include <string_view>

namespace agentengine::shell {

namespace {

ae::error contract_error(std::string message, std::string code) {
    return ae::error{failure_class::contract, std::move(message), std::move(code)};
}

ae::error resource_error(std::string message, std::string code) {
    return ae::error{failure_class::resource, std::move(message), std::move(code)};
}

bool is_ident_start(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool is_ident_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}
bool is_inline_ws(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
bool is_operator_start(char c) {
    return c == '|' || c == '&' || c == ';' || c == '<' || c == '>';
}

constexpr std::array<std::string_view, 6> kBodyStopKeywords = {"then", "else", "fi", "in", "do", "done"};

class Parser {
public:
    explicit Parser(std::string_view src, std::pmr::polymorphic_allocator<> alloc)
        : src_(src), alloc_(alloc) {}

    result<ScriptNode> parse_top() {
        auto list = parse_statement_list({});
        if (!list) return std::unexpected(list.error());
        skip_ws_and_semis();
        if (!at_end()) {
            return std::unexpected(
                contract_error("unexpected trailing input at offset " + std::to_string(pos_),
                                "shell.trailing_garbage"));
        }
        ScriptNode node(alloc_);
        node.statements = std::move(*list);
        return node;
    }

private:
    std::string_view                src_;
    std::size_t                     pos_   = 0;
    std::size_t                     depth_ = 0;
    std::size_t                     token_count_ = 0;
    std::pmr::polymorphic_allocator<> alloc_;

    // ---- low-level cursor helpers -----------------------------------------------------------
    [[nodiscard]] bool at_end() const { return pos_ >= src_.size(); }
    [[nodiscard]] char peek() const { return src_[pos_]; }
    [[nodiscard]] char peek2() const { return pos_ + 1 < src_.size() ? src_[pos_ + 1] : '\0'; }
    void advance() { ++pos_; }

    void skip_inline_ws() {
        while (!at_end() && is_inline_ws(peek())) advance();
    }
    void skip_ws_and_semis() {
        while (!at_end() && (is_inline_ws(peek()) || peek() == ';')) advance();
    }

    [[nodiscard]] result<void> bump_token_budget() {
        if (++token_count_ > kMaxTokens) {
            return std::unexpected(
                resource_error("token budget exceeded (" + std::to_string(kMaxTokens) + ")",
                                "shell.too_many_tokens"));
        }
        return {};
    }

    // Peeks whether the identifier starting at the current position (after skipping inline
    // whitespace) exactly equals `kw`, with a real word boundary after it (not a prefix of a
    // longer identifier such as "iffy"). Never consumes.
    [[nodiscard]] bool peek_is_keyword(std::string_view kw) const {
        std::size_t p = pos_;
        while (p < src_.size() && is_inline_ws(src_[p])) ++p;
        std::size_t start = p;
        while (p < src_.size() && is_ident_char(src_[p])) ++p;
        std::string_view ident = src_.substr(start, p - start);
        return ident == kw;
    }

    [[nodiscard]] bool peek_is_any_keyword(std::span<std::string_view const> kws) const {
        for (auto kw : kws) {
            if (peek_is_keyword(kw)) return true;
        }
        return false;
    }

    [[nodiscard]] result<void> expect_keyword(std::string_view kw) {
        skip_inline_ws();
        std::size_t start = pos_;
        while (!at_end() && is_ident_char(peek())) advance();
        std::string_view ident = src_.substr(start, pos_ - start);
        if (ident != kw) {
            return std::unexpected(contract_error(
                "expected '" + std::string(kw) + "' at offset " + std::to_string(start),
                "shell.syntax_error"));
        }
        return bump_token_budget();
    }

    // Only used where the caller has ALREADY confirmed (via peek_is_keyword) that the keyword is
    // present — consumes it unconditionally.
    void consume_keyword_unchecked(std::string_view kw) {
        skip_inline_ws();
        pos_ += kw.size();
    }

    [[nodiscard]] bool looks_like_assignment() const {
        std::size_t p = pos_;
        if (p >= src_.size() || !is_ident_start(src_[p])) return false;
        while (p < src_.size() && is_ident_char(src_[p])) ++p;
        return p < src_.size() && src_[p] == '=';
    }

    // ---- word / atom scanning (iterative — see file header) ----------------------------------
    [[nodiscard]] result<Word> scan_atoms(bool allow_empty) {
        Word w(alloc_);
        bool any = false;
        while (!at_end()) {
            char c = peek();
            if (is_inline_ws(c) || is_operator_start(c)) break;
            any = true;
            if (auto r = bump_token_budget(); !r) return std::unexpected(r.error());
            if (c == '\'' || c == '"') {
                char quote = c;
                advance();
                std::pmr::string text(alloc_);
                bool closed = false;
                while (!at_end()) {
                    char cc = peek();
                    advance();
                    if (cc == quote) { closed = true; break; }
                    text.push_back(cc);
                }
                if (!closed) {
                    return std::unexpected(
                        contract_error("unterminated quote", "shell.unterminated_quote"));
                }
                // finding 11: quoted atoms never expand — no $NAME substitution is attempted
                // inside them at all, not even scanned for.
                w.atoms.push_back(WordAtom{word_atom_kind::quoted, std::move(text)});
            } else if (c == '$') {
                advance();
                if (!at_end() && peek() == '{') {
                    advance();
                    std::pmr::string name(alloc_);
                    while (!at_end() && is_ident_char(peek())) { name.push_back(peek()); advance(); }
                    if (at_end() || peek() != '}' || name.empty()) {
                        return std::unexpected(
                            contract_error("malformed ${...} reference", "shell.bad_var_ref"));
                    }
                    advance(); // '}'
                    w.atoms.push_back(WordAtom{word_atom_kind::var_ref, std::move(name)});
                } else if (!at_end() && is_ident_start(peek())) {
                    std::pmr::string name(alloc_);
                    while (!at_end() && is_ident_char(peek())) { name.push_back(peek()); advance(); }
                    w.atoms.push_back(WordAtom{word_atom_kind::var_ref, std::move(name)});
                } else {
                    std::pmr::string text(1, '$', alloc_);
                    w.atoms.push_back(WordAtom{word_atom_kind::literal, std::move(text)});
                }
            } else {
                std::pmr::string text(alloc_);
                while (!at_end()) {
                    char cc = peek();
                    if (is_inline_ws(cc) || is_operator_start(cc) || cc == '\'' || cc == '"' ||
                        cc == '$') {
                        break;
                    }
                    text.push_back(cc);
                    advance();
                }
                w.atoms.push_back(WordAtom{word_atom_kind::literal, std::move(text)});
            }
        }
        if (!any && !allow_empty) {
            return std::unexpected(contract_error("expected a word", "shell.expected_word"));
        }
        return w;
    }

    [[nodiscard]] result<Word> parse_word() { return scan_atoms(/*allow_empty=*/false); }

    // ---- grammar productions ------------------------------------------------------------------
    [[nodiscard]] result<SimpleCommandNode> parse_simple_command() {
        SimpleCommandNode cmd(alloc_);
        while (true) {
            skip_inline_ws();
            if (!looks_like_assignment()) break;
            std::pmr::string name(alloc_);
            while (!at_end() && is_ident_char(peek())) { name.push_back(peek()); advance(); }
            advance(); // '='
            if (auto r = bump_token_budget(); !r) return std::unexpected(r.error());
            auto value = scan_atoms(/*allow_empty=*/true);
            if (!value) return std::unexpected(value.error());
            cmd.assigns.push_back(Assignment{std::move(name), std::move(*value)});
        }
        while (true) {
            skip_inline_ws();
            if (at_end() || is_operator_start(peek())) break;
            if (peek_is_any_keyword(kBodyStopKeywords)) break;
            auto w = parse_word();
            if (!w) return std::unexpected(w.error());
            cmd.words.push_back(std::move(*w));
        }
        while (true) {
            skip_inline_ws();
            if (at_end()) break;
            char c = peek();
            redirect_kind kind;
            if (c == '<') {
                advance();
                kind = redirect_kind::input;
            } else if (c == '>') {
                advance();
                if (!at_end() && peek() == '>') { advance(); kind = redirect_kind::append; }
                else kind = redirect_kind::output;
            } else {
                break;
            }
            if (auto r = bump_token_budget(); !r) return std::unexpected(r.error());
            skip_inline_ws();
            auto target = parse_word();
            if (!target) return std::unexpected(target.error());
            cmd.redirects.push_back(Redirect{kind, std::move(*target)});
        }
        if (cmd.words.empty()) {
            return std::unexpected(
                contract_error("expected a command name", "shell.expected_command"));
        }
        return cmd;
    }

    [[nodiscard]] result<PipelineNode> parse_pipeline() {
        PipelineNode pl(alloc_);
        auto first = parse_simple_command();
        if (!first) return std::unexpected(first.error());
        pl.commands.push_back(std::move(*first));
        while (true) {
            skip_inline_ws();
            if (at_end() || peek() != '|' || peek2() == '|') break; // '|' but not '||'
            advance();
            if (auto r = bump_token_budget(); !r) return std::unexpected(r.error());
            skip_inline_ws();
            auto next = parse_simple_command();
            if (!next) return std::unexpected(next.error());
            pl.commands.push_back(std::move(*next));
        }
        return pl;
    }

    [[nodiscard]] result<AndOrNode> parse_and_or() {
        AndOrNode ao(alloc_);
        auto first = parse_pipeline();
        if (!first) return std::unexpected(first.error());
        ao.pipelines.push_back(std::move(*first));
        while (true) {
            skip_inline_ws();
            bool is_and;
            if (!at_end() && peek() == '&' && peek2() == '&') { is_and = true; }
            else if (!at_end() && peek() == '|' && peek2() == '|') { is_and = false; }
            else break;
            advance();
            advance();
            if (auto r = bump_token_budget(); !r) return std::unexpected(r.error());
            skip_inline_ws();
            auto next = parse_pipeline();
            if (!next) return std::unexpected(next.error());
            ao.pipelines.push_back(std::move(*next));
            ao.is_and.push_back(is_and);
        }
        return ao;
    }

    // Shared depth counter — the ONLY two entry points capable of recursive descent in this
    // grammar (see file header). RAII guarantees the decrement happens on every exit path,
    // including the early returns on a syntax error inside the body.
    class DepthGuard {
    public:
        explicit DepthGuard(std::size_t& depth) : depth_(depth) { ++depth_; }
        ~DepthGuard() { --depth_; }
        [[nodiscard]] bool ok() const { return depth_ <= kMaxNestingDepth; }
    private:
        std::size_t& depth_;
    };

    // Returns an arena-allocated StatementNode* (see shell_grammar.hpp's comment on why IfNode/
    // ForNode/ScriptNode hold StatementNode* rather than StatementNode by value).
    [[nodiscard]] result<StatementNode*> parse_statement() {
        if (peek_is_keyword("if")) {
            auto n = parse_if();
            if (!n) return std::unexpected(n.error());
            return alloc_.new_object<StatementNode>(
                std::variant<AndOrNode, IfNode, ForNode>(std::move(*n)));
        }
        if (peek_is_keyword("for")) {
            auto n = parse_for();
            if (!n) return std::unexpected(n.error());
            return alloc_.new_object<StatementNode>(
                std::variant<AndOrNode, IfNode, ForNode>(std::move(*n)));
        }
        auto n = parse_and_or();
        if (!n) return std::unexpected(n.error());
        return alloc_.new_object<StatementNode>(
            std::variant<AndOrNode, IfNode, ForNode>(std::move(*n)));
    }

    [[nodiscard]] result<std::pmr::vector<StatementNode*>>
    parse_statement_list(std::span<std::string_view const> stop_keywords) {
        std::pmr::vector<StatementNode*> list(alloc_);
        while (true) {
            skip_ws_and_semis();
            if (at_end()) break;
            if (!stop_keywords.empty() && peek_is_any_keyword(stop_keywords)) break;
            auto stmt = parse_statement();
            if (!stmt) return std::unexpected(stmt.error());
            list.push_back(*stmt);
        }
        return list;
    }

    [[nodiscard]] result<IfNode> parse_if() {
        DepthGuard guard(depth_);
        if (!guard.ok()) {
            return std::unexpected(
                resource_error("nesting depth exceeded (" + std::to_string(kMaxNestingDepth) + ")",
                                "shell.nesting_too_deep"));
        }
        if (auto r = expect_keyword("if"); !r) return std::unexpected(r.error());
        skip_inline_ws();
        auto cond = parse_pipeline();
        if (!cond) return std::unexpected(cond.error());
        if (auto r = expect_keyword("then"); !r) return std::unexpected(r.error());

        IfNode node(alloc_);
        node.cond = std::move(*cond);

        std::array<std::string_view, 2> then_stop{"else", "fi"};
        auto then_stmts = parse_statement_list(then_stop);
        if (!then_stmts) return std::unexpected(then_stmts.error());
        node.then_body = std::move(*then_stmts);

        if (peek_is_keyword("else")) {
            consume_keyword_unchecked("else");
            if (auto r = bump_token_budget(); !r) return std::unexpected(r.error());
            std::array<std::string_view, 1> else_stop{"fi"};
            auto else_stmts = parse_statement_list(else_stop);
            if (!else_stmts) return std::unexpected(else_stmts.error());
            node.else_body = std::move(*else_stmts);
        }
        if (auto r = expect_keyword("fi"); !r) return std::unexpected(r.error());
        return node;
    }

    [[nodiscard]] result<ForNode> parse_for() {
        DepthGuard guard(depth_);
        if (!guard.ok()) {
            return std::unexpected(
                resource_error("nesting depth exceeded (" + std::to_string(kMaxNestingDepth) + ")",
                                "shell.nesting_too_deep"));
        }
        if (auto r = expect_keyword("for"); !r) return std::unexpected(r.error());
        skip_inline_ws();
        if (at_end() || !is_ident_start(peek())) {
            return std::unexpected(
                contract_error("expected loop variable name", "shell.syntax_error"));
        }
        std::pmr::string var(alloc_);
        while (!at_end() && is_ident_char(peek())) { var.push_back(peek()); advance(); }
        if (auto r = bump_token_budget(); !r) return std::unexpected(r.error());
        if (auto r = expect_keyword("in"); !r) return std::unexpected(r.error());

        ForNode node(alloc_);
        node.var = std::move(var);
        while (true) {
            skip_inline_ws();
            if (peek_is_keyword("do")) break;
            if (at_end() || is_operator_start(peek())) {
                return std::unexpected(
                    contract_error("expected 'do'", "shell.syntax_error"));
            }
            auto w = parse_word();
            if (!w) return std::unexpected(w.error());
            node.items.push_back(std::move(*w));
        }
        if (node.items.empty()) {
            return std::unexpected(contract_error("'for ... in' needs at least one word",
                                                    "shell.syntax_error"));
        }
        if (auto r = expect_keyword("do"); !r) return std::unexpected(r.error());
        std::array<std::string_view, 1> done_stop{"done"};
        auto body = parse_statement_list(done_stop);
        if (!body) return std::unexpected(body.error());
        node.body = std::move(*body);
        if (auto r = expect_keyword("done"); !r) return std::unexpected(r.error());
        return node;
    }
};

} // namespace

result<ParsedScript> parse(std::string_view source) {
    if (source.size() > kMaxSourceBytes) {
        return std::unexpected(resource_error(
            "source exceeds " + std::to_string(kMaxSourceBytes) + " bytes", "shell.source_too_large"));
    }

    ParsedScript parsed;
    parsed.arena_storage = std::make_unique<std::byte[]>(kArenaBytes);
    parsed.resource = std::make_unique<std::pmr::monotonic_buffer_resource>(
        parsed.arena_storage.get(), kArenaBytes, std::pmr::null_memory_resource());

    try {
        Parser parser(source, std::pmr::polymorphic_allocator<>(parsed.resource.get()));
        auto script = parser.parse_top();
        if (!script) return std::unexpected(script.error());
        parsed.script.emplace(std::move(*script));
        return parsed;
    } catch (std::bad_alloc const&) {
        return std::unexpected(
            resource_error("AST arena exhausted", "shell.arena_exhausted"));
    }
}

} // namespace agentengine::shell
