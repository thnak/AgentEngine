// Implements mediated_shell_parser.hpp. Two phases: (1) a lexer producing a bounded, flat token
// vector (bounded by kMaxTokens, one pass over the source, quoting/`$NAME` handled inline as part
// of WORD tokens so keyword recognition ("if"/"then"/... ) stays a PARSER-level, position-sensitive
// decision, matching how the grammar itself treats them -- lexically a keyword is just a plain
// word); (2) a recursive-descent parser over that token vector with one-token lookahead, no
// backtracking (ADR-001's Judged design, reproduced fresh).

#include "backends/native_jail/mediated_shell_parser.hpp"

#include <cctype>
#include <vector>

namespace agentengine::native_jail::mediated_shell {

namespace {

result<void> too_large(char const* code) {
    return std::unexpected(error{failure_class::resource, "shell script exceeds this parser's bounds", code});
}
result<void> malformed(std::string message) {
    return std::unexpected(error{failure_class::contract, std::move(message), "shell.parse_error"});
}

bool is_word_delimiter(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ';' || c == '&' || c == '|' || c == '<' ||
           c == '>';
}
bool is_name_start(char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_'; }
bool is_name_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; }

enum class token_kind { word, semi, amp_amp, pipe_pipe, pipe, redirect_in, redirect_out, redirect_append, eof };

struct Token {
    token_kind kind;
    std::pmr::vector<WordAtom> atoms;  // meaningful only when kind == word
};

// Phase 1: lexing. Bounded by kMaxSourceBytes (checked up front) and kMaxTokens (checked per
// token) -- an adversarial 10 MB input is rejected here, before any AST node is ever allocated.
class Lexer {
public:
    Lexer(std::string_view src, std::pmr::memory_resource* mr) : src_(src), mr_(mr) {}

    result<std::vector<Token>> lex() {
        if (src_.size() > kMaxSourceBytes) return std::unexpected(too_large("shell.source_too_large").error());
        std::vector<Token> tokens;
        for (;;) {
            skip_whitespace_and_comments();
            if (pos_ >= src_.size()) {
                tokens.push_back({token_kind::eof, {}});
                break;
            }
            if (tokens.size() >= kMaxTokens) return std::unexpected(too_large("shell.too_many_tokens").error());
            char c = src_[pos_];
            if (c == ';') { tokens.push_back({token_kind::semi, {}}); ++pos_; continue; }
            if (c == '|') {
                if (pos_ + 1 < src_.size() && src_[pos_ + 1] == '|') {
                    tokens.push_back({token_kind::pipe_pipe, {}});
                    pos_ += 2;
                } else {
                    tokens.push_back({token_kind::pipe, {}});
                    ++pos_;
                }
                continue;
            }
            if (c == '&') {
                if (pos_ + 1 < src_.size() && src_[pos_ + 1] == '&') {
                    tokens.push_back({token_kind::amp_amp, {}});
                    pos_ += 2;
                    continue;
                }
                return std::unexpected(malformed("bare '&' is not part of this grammar "
                                                   "(background processes are a capability, not "
                                                   "syntax -- 010 §10 Q6)").error());
            }
            if (c == '<') { tokens.push_back({token_kind::redirect_in, {}}); ++pos_; continue; }
            if (c == '>') {
                if (pos_ + 1 < src_.size() && src_[pos_ + 1] == '>') {
                    tokens.push_back({token_kind::redirect_append, {}});
                    pos_ += 2;
                } else {
                    tokens.push_back({token_kind::redirect_out, {}});
                    ++pos_;
                }
                continue;
            }
            auto word = lex_word();
            if (!word) return std::unexpected(word.error());
            tokens.push_back({token_kind::word, std::move(*word)});
        }
        return tokens;
    }

private:
    void skip_whitespace_and_comments() {
        for (;;) {
            while (pos_ < src_.size() &&
                   (src_[pos_] == ' ' || src_[pos_] == '\t' || src_[pos_] == '\n' || src_[pos_] == '\r')) {
                ++pos_;
            }
            if (pos_ < src_.size() && src_[pos_] == '#') {
                while (pos_ < src_.size() && src_[pos_] != '\n') ++pos_;
                continue;
            }
            break;
        }
    }

    result<std::pmr::vector<WordAtom>> lex_word() {
        std::pmr::vector<WordAtom> atoms(mr_);
        std::pmr::string current_literal(mr_);
        auto flush_literal = [&]() {
            if (!current_literal.empty()) {
                atoms.push_back(WordAtom{word_atom_kind::literal, current_literal});
                current_literal.clear();
            }
        };
        bool consumed_anything = false;
        while (pos_ < src_.size() && !is_word_delimiter(src_[pos_])) {
            consumed_anything = true;
            char c = src_[pos_];
            if (c == '\'') {
                flush_literal();
                ++pos_;
                std::size_t start = pos_;
                while (pos_ < src_.size() && src_[pos_] != '\'') ++pos_;
                if (pos_ >= src_.size()) return std::unexpected(malformed("unterminated single quote").error());
                atoms.push_back(WordAtom{word_atom_kind::quoted,
                                          std::pmr::string(src_.substr(start, pos_ - start), mr_)});
                ++pos_;  // closing quote
            } else if (c == '"') {
                flush_literal();
                ++pos_;
                std::pmr::string content(mr_);
                while (pos_ < src_.size() && src_[pos_] != '"') {
                    if (src_[pos_] == '\\' && pos_ + 1 < src_.size() &&
                        (src_[pos_ + 1] == '"' || src_[pos_ + 1] == '\\')) {
                        content.push_back(src_[pos_ + 1]);
                        pos_ += 2;
                    } else {
                        content.push_back(src_[pos_]);
                        ++pos_;
                    }
                }
                if (pos_ >= src_.size()) return std::unexpected(malformed("unterminated double quote").error());
                atoms.push_back(WordAtom{word_atom_kind::quoted, content});
                ++pos_;  // closing quote
            } else if (c == '$') {
                ++pos_;
                if (pos_ < src_.size() && src_[pos_] == '{') {
                    ++pos_;
                    std::size_t start = pos_;
                    while (pos_ < src_.size() && is_name_char(src_[pos_])) ++pos_;
                    if (pos_ >= src_.size() || src_[pos_] != '}' || pos_ == start) {
                        return std::unexpected(malformed("malformed '${NAME}' expansion").error());
                    }
                    flush_literal();
                    atoms.push_back(WordAtom{word_atom_kind::var_ref,
                                              std::pmr::string(src_.substr(start, pos_ - start), mr_)});
                    ++pos_;  // closing brace
                } else if (pos_ < src_.size() && is_name_start(src_[pos_])) {
                    std::size_t start = pos_;
                    while (pos_ < src_.size() && is_name_char(src_[pos_])) ++pos_;
                    flush_literal();
                    atoms.push_back(WordAtom{word_atom_kind::var_ref,
                                              std::pmr::string(src_.substr(start, pos_ - start), mr_)});
                } else {
                    current_literal.push_back('$');  // a bare '$' with no following NAME -- literal
                }
            } else {
                current_literal.push_back(c);
                ++pos_;
            }
        }
        if (!consumed_anything) return std::unexpected(malformed("expected a word here").error());
        flush_literal();
        return atoms;
    }

    std::string_view src_;
    std::pmr::memory_resource* mr_;
    std::size_t pos_ = 0;
};

// Phase 2: recursive-descent parsing over the lexed token vector, one-token lookahead, no
// backtracking. `depth_` is shared across every `if`/`for` production (ADR-001 finding 12's closed
// "sum, not max" fix), incremented/decremented via RAII so every exit path -- including an early
// error return -- still decrements.
class Parser {
public:
    Parser(std::vector<Token> tokens, std::pmr::memory_resource* mr) : tokens_(std::move(tokens)), mr_(mr) {}

    result<ScriptNode> parse_script() {
        std::pmr::vector<StatementNode*> statements(mr_);
        skip_separators();
        while (!at(token_kind::eof)) {
            auto st = parse_statement();
            if (!st) return std::unexpected(st.error());
            statements.push_back(*st);
            skip_separators();
        }
        return ScriptNode{std::move(statements)};
    }

private:
    struct DepthGuard {
        std::size_t* depth;
        explicit DepthGuard(std::size_t* d) : depth(d) { ++*depth; }
        ~DepthGuard() { --*depth; }
    };

    bool at(token_kind k) const { return tokens_[pos_].kind == k; }
    bool at_word(std::string_view literal) const {
        if (!at(token_kind::word)) return false;
        auto const& atoms = tokens_[pos_].atoms;
        return atoms.size() == 1 && atoms[0].kind == word_atom_kind::literal && atoms[0].text == literal;
    }
    Token const& advance() { return tokens_[pos_++]; }

    void skip_separators() {
        while (at(token_kind::semi)) ++pos_;
    }

    StatementNode* alloc_statement(std::variant<AndOrNode, IfNode, ForNode> value) {
        auto* node = static_cast<StatementNode*>(mr_->allocate(sizeof(StatementNode), alignof(StatementNode)));
        new (node) StatementNode{std::move(value)};
        return node;
    }

    result<StatementNode*> parse_statement() {
        if (at_word("if")) {
            auto n = parse_if();
            if (!n) return std::unexpected(n.error());
            return alloc_statement(std::move(*n));
        }
        if (at_word("for")) {
            auto n = parse_for();
            if (!n) return std::unexpected(n.error());
            return alloc_statement(std::move(*n));
        }
        auto n = parse_and_or();
        if (!n) return std::unexpected(n.error());
        return alloc_statement(std::move(*n));
    }

    result<IfNode> parse_if() {
        if (depth_ >= kMaxNestingDepth) return std::unexpected(too_large("shell.nesting_too_deep").error());
        DepthGuard guard(&depth_);
        advance();  // 'if'
        auto cond = parse_pipeline();
        if (!cond) return std::unexpected(cond.error());
        if (!at_word("then")) return std::unexpected(malformed("expected 'then' after 'if' condition").error());
        advance();
        auto then_body = parse_command_list_until({"else", "fi"});
        if (!then_body) return std::unexpected(then_body.error());
        std::pmr::vector<StatementNode*> else_body(mr_);
        if (at_word("else")) {
            advance();
            auto eb = parse_command_list_until({"fi"});
            if (!eb) return std::unexpected(eb.error());
            else_body = std::move(*eb);
        }
        if (!at_word("fi")) return std::unexpected(malformed("expected 'fi' to close 'if'").error());
        advance();
        return IfNode{std::move(*cond), std::move(*then_body), std::move(else_body)};
    }

    result<ForNode> parse_for() {
        if (depth_ >= kMaxNestingDepth) return std::unexpected(too_large("shell.nesting_too_deep").error());
        DepthGuard guard(&depth_);
        advance();  // 'for'
        if (!at(token_kind::word) || tokens_[pos_].atoms.size() != 1 ||
            tokens_[pos_].atoms[0].kind != word_atom_kind::literal) {
            return std::unexpected(malformed("expected a loop variable NAME after 'for'").error());
        }
        std::pmr::string var = tokens_[pos_].atoms[0].text;
        advance();
        if (!at_word("in")) return std::unexpected(malformed("expected 'in' after 'for NAME'").error());
        advance();
        std::pmr::vector<Word> items(mr_);
        while (at(token_kind::word) && !at_word("do")) {
            items.push_back(Word{tokens_[pos_].atoms});
            advance();
        }
        if (items.empty()) return std::unexpected(malformed("expected at least one item after 'for NAME in'").error());
        if (!at_word("do")) return std::unexpected(malformed("expected 'do' after 'for NAME in items'").error());
        advance();
        auto body = parse_command_list_until({"done"});
        if (!body) return std::unexpected(body.error());
        if (!at_word("done")) return std::unexpected(malformed("expected 'done' to close 'for'").error());
        advance();
        return ForNode{std::move(var), std::move(items), std::move(*body)};
    }

    // Parses zero or more statements until a word matching one of `stop_words` is the next token
    // (consumed by the CALLER, not here) -- used for if/for bodies, which are `command_line`
    // productions (statement lists, not a single statement) bounded by a keyword, not EOF.
    result<std::pmr::vector<StatementNode*>> parse_command_list_until(
        std::initializer_list<std::string_view> stop_words) {
        std::pmr::vector<StatementNode*> statements(mr_);
        skip_separators();
        auto is_stop = [&] {
            for (auto sw : stop_words) {
                if (at_word(sw)) return true;
            }
            return false;
        };
        while (!is_stop()) {
            if (at(token_kind::eof)) return std::unexpected(malformed("unexpected end of script").error());
            auto st = parse_statement();
            if (!st) return std::unexpected(st.error());
            statements.push_back(*st);
            skip_separators();
        }
        return statements;
    }

    result<AndOrNode> parse_and_or() {
        auto first = parse_pipeline();
        if (!first) return std::unexpected(first.error());
        std::pmr::vector<PipelineNode> pipelines(mr_);
        std::pmr::vector<bool> is_and(mr_);
        pipelines.push_back(std::move(*first));
        for (;;) {
            bool and_next;
            if (at(token_kind::amp_amp)) and_next = true;
            else if (at(token_kind::pipe_pipe)) and_next = false;
            else break;
            advance();
            auto next = parse_pipeline();
            if (!next) return std::unexpected(next.error());
            is_and.push_back(and_next);
            pipelines.push_back(std::move(*next));
        }
        return AndOrNode{std::move(pipelines), std::move(is_and)};
    }

    result<PipelineNode> parse_pipeline() {
        std::pmr::vector<SimpleCommandNode> commands(mr_);
        auto first = parse_simple_command();
        if (!first) return std::unexpected(first.error());
        commands.push_back(std::move(*first));
        while (at(token_kind::pipe)) {
            advance();
            auto next = parse_simple_command();
            if (!next) return std::unexpected(next.error());
            commands.push_back(std::move(*next));
        }
        return PipelineNode{std::move(commands)};
    }

    // Splits a leading word of the form NAME=value into an Assignment if `w`'s first atom is a
    // literal containing an unquoted, unexpanded '=' forming a syntactically valid NAME prefix --
    // '=' is ordinary word-lexer content (never a lexical delimiter), so this split happens here,
    // at the grammar level, not in the lexer.
    static std::optional<Assignment> try_split_assignment(Word const& w, std::pmr::memory_resource* mr) {
        if (w.atoms.empty() || w.atoms[0].kind != word_atom_kind::literal) return std::nullopt;
        std::pmr::string const& text = w.atoms[0].text;
        auto eq = text.find('=');
        if (eq == std::pmr::string::npos || eq == 0) return std::nullopt;
        for (std::size_t i = 0; i < eq; ++i) {
            if (!(is_name_char(text[i]) || (i == 0 && is_name_start(text[i])))) return std::nullopt;
        }
        if (!is_name_start(text[0])) return std::nullopt;
        std::pmr::string name(text.substr(0, eq), mr);
        std::pmr::vector<WordAtom> value_atoms(mr);
        std::pmr::string rest(text.substr(eq + 1), mr);
        if (!rest.empty() || w.atoms.size() == 1) {
            value_atoms.push_back(WordAtom{word_atom_kind::literal, std::move(rest)});
        }
        for (std::size_t i = 1; i < w.atoms.size(); ++i) value_atoms.push_back(w.atoms[i]);
        return Assignment{std::move(name), Word{std::move(value_atoms)}};
    }

    // "then"/"else"/"fi"/"do"/"done" are reserved at every position a simple_command's words could
    // otherwise swallow them (the closing keyword of an enclosing if/for is always the next thing
    // after the LAST command of a body, never a legitimate bare argument) -- an UNQUOTED word
    // matching one of these always ends word-collection here, rather than being consumed as an
    // ordinary argument. A script that genuinely wants the literal text must quote it (`echo
    // "then"`), which `at_word` already distinguishes (quoted atoms never match a keyword check,
    // since it inspects the atom's KIND, not just its text).
    bool at_reserved_keyword() const {
        return at_word("then") || at_word("else") || at_word("fi") || at_word("do") || at_word("done");
    }

    result<SimpleCommandNode> parse_simple_command() {
        std::pmr::vector<Assignment> assigns(mr_);
        std::pmr::vector<Word> words(mr_);
        std::pmr::vector<Redirect> redirects(mr_);

        while (at(token_kind::word) && !at_reserved_keyword()) {
            Word w{tokens_[pos_].atoms};
            if (words.empty()) {
                if (auto assign = try_split_assignment(w, mr_)) {
                    assigns.push_back(std::move(*assign));
                    advance();
                    continue;
                }
            }
            words.push_back(std::move(w));
            advance();
            break;  // first non-assignment word found -- fall through to the words+/redirect* loop
        }
        while ((at(token_kind::word) && !at_reserved_keyword()) || at(token_kind::redirect_in) ||
               at(token_kind::redirect_out) || at(token_kind::redirect_append)) {
            if (at(token_kind::word)) {
                words.push_back(Word{tokens_[pos_].atoms});
                advance();
                continue;
            }
            redirect_kind kind = at(token_kind::redirect_in)      ? redirect_kind::input
                                  : at(token_kind::redirect_append) ? redirect_kind::append
                                                                     : redirect_kind::output;
            advance();
            if (!at(token_kind::word)) return std::unexpected(malformed("expected a target word after redirect").error());
            Word target{tokens_[pos_].atoms};
            advance();
            redirects.push_back(Redirect{kind, std::move(target)});
        }
        if (words.empty()) return std::unexpected(malformed("expected a command word").error());
        return SimpleCommandNode{std::move(assigns), std::move(words), std::move(redirects)};
    }

    std::vector<Token> tokens_;
    std::pmr::memory_resource* mr_;
    std::size_t pos_ = 0;
    std::size_t depth_ = 0;
};

}  // namespace

result<ParsedScript> parse(std::string_view source) {
    ParsedScript out;
    out.arena_storage = std::make_unique<std::byte[]>(kArenaBytes);
    // `null_memory_resource()` upstream: once the fixed arena is exhausted, an allocation THROWS
    // rather than silently growing onto the ordinary heap -- exactly the bounded-arena property
    // ADR-001 §2.5.6 required, carried forward as a design finding. Caught below and turned into an
    // ordinary `result<>` error at this function's own boundary (CONVENTIONS.md: no exceptions for
    // control flow) -- everything upstream of this one catch stays exception-free.
    out.resource = std::make_unique<std::pmr::monotonic_buffer_resource>(
        out.arena_storage.get(), kArenaBytes, std::pmr::null_memory_resource());

    try {
        Lexer lexer(source, out.resource.get());
        auto tokens = lexer.lex();
        if (!tokens) return std::unexpected(tokens.error());

        Parser parser(std::move(*tokens), out.resource.get());
        auto script = parser.parse_script();
        if (!script) return std::unexpected(script.error());

        out.script = std::move(*script);
        return out;
    } catch (std::bad_alloc const&) {
        return std::unexpected(error{failure_class::resource,
                                      "shell script exceeds this parser's bounded arena",
                                      "shell.arena_exhausted"});
    }
}

}  // namespace agentengine::native_jail::mediated_shell
