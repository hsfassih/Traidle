#include "expr.h"

#include <cctype>

namespace {

enum class Tok { Num, Time, Ident, Dot, LBr, RBr, At, LPar, RPar, Comma, Op, End };

struct Token {
    Tok kind;
    string text;
};

vector<Token> tokenize(const string& s, vector<string>& errors) {
    vector<Token> out;
    size_t i = 0;
    while (i < s.size()) {
        const char c = s[i];
        if (isspace(static_cast<unsigned char>(c))) { ++i; continue; }
        if (isdigit(static_cast<unsigned char>(c))) {
            size_t j = i;
            while (j < s.size() && isdigit(static_cast<unsigned char>(s[j]))) ++j;
            if (j - i == 2 && j < s.size() && s[j] == ':' && j + 2 < s.size() + 0 &&
                isdigit(static_cast<unsigned char>(s[j + 1])) && isdigit(static_cast<unsigned char>(s[j + 2]))) {
                const int hh = stoi(s.substr(i, 2));
                const int mm = stoi(s.substr(j + 1, 2));
                if (hh > 24 || mm > 59 || (hh == 24 && mm != 0)) errors.push_back("invalid time literal " + s.substr(i, 5));
                out.push_back({Tok::Time, s.substr(i, 5)});
                i = j + 3;
                continue;
            }
            if (j < s.size() && s[j] == '.' && j + 1 < s.size() && isdigit(static_cast<unsigned char>(s[j + 1]))) {
                ++j;
                while (j < s.size() && isdigit(static_cast<unsigned char>(s[j]))) ++j;
            }
            out.push_back({Tok::Num, s.substr(i, j - i)});
            i = j;
            continue;
        }
        if (isalpha(static_cast<unsigned char>(c)) || c == '_') {
            size_t j = i;
            while (j < s.size() && (isalnum(static_cast<unsigned char>(s[j])) || s[j] == '_')) ++j;
            out.push_back({Tok::Ident, s.substr(i, j - i)});
            i = j;
            continue;
        }
        const string two = s.substr(i, 2);
        if (two == "==" || two == "!=" || two == "<=" || two == ">=") {
            out.push_back({Tok::Op, two});
            i += 2;
            continue;
        }
        switch (c) {
            case '.': out.push_back({Tok::Dot, "."}); break;
            case '[': out.push_back({Tok::LBr, "["}); break;
            case ']': out.push_back({Tok::RBr, "]"}); break;
            case '@': out.push_back({Tok::At, "@"}); break;
            case '(': out.push_back({Tok::LPar, "("}); break;
            case ')': out.push_back({Tok::RPar, ")"}); break;
            case ',': out.push_back({Tok::Comma, ","}); break;
            case '+': case '-': case '*': case '/': case '<': case '>':
                out.push_back({Tok::Op, string(1, c)});
                break;
            default:
                errors.push_back(string("illegal character '") + c + "'");
        }
        ++i;
    }
    out.push_back({Tok::End, ""});
    return out;
}

class Parser {
public:
    Parser(vector<Token> toks, const Vocabulary& v, const set<string>& customs, ExprResult& r)
        : t_(move(toks)), v_(v), customs_(customs), r_(r) {}

    void run() {
        expr();
        if (peek().kind != Tok::End) err("unexpected token '" + peek().text + "'");
    }

private:
    const Token& peek() const { return t_[p_]; }
    Token next() { return t_[p_ < t_.size() - 1 ? p_++ : p_]; }
    bool isWord(const string& w) const { return peek().kind == Tok::Ident && peek().text == w; }
    void err(const string& m) {
        if (r_.errors.size() < 8) r_.errors.push_back(m);
    }
    void expect(Tok k, const string& what) {
        if (peek().kind != k) {
            err("expected " + what + " but found '" + peek().text + "'");
            return;
        }
        next();
    }

    void expr() { orExpr(); }
    void orExpr() {
        andExpr();
        while (isWord("OR")) { next(); andExpr(); }
    }
    void andExpr() {
        notExpr();
        while (isWord("AND")) { next(); notExpr(); }
    }
    void notExpr() {
        if (isWord("NOT")) { next(); notExpr(); return; }
        cmp();
    }
    void cmp() {
        add();
        const auto& tk = peek();
        if (tk.kind == Tok::Op && (tk.text == "==" || tk.text == "!=" || tk.text == "<" || tk.text == "<=" ||
                                   tk.text == ">" || tk.text == ">=")) {
            next();
            add();
        }
    }
    void add() {
        mul();
        while (peek().kind == Tok::Op && (peek().text == "+" || peek().text == "-")) { next(); mul(); }
    }
    void mul() {
        unary();
        while (peek().kind == Tok::Op && (peek().text == "*" || peek().text == "/")) { next(); unary(); }
    }
    void unary() {
        if (peek().kind == Tok::Op && peek().text == "-") { next(); unary(); return; }
        primary();
    }
    void primary() {
        const Token tk = peek();
        if (tk.kind == Tok::Num || tk.kind == Tok::Time) { next(); return; }
        if (tk.kind == Tok::LPar) {
            next();
            expr();
            expect(Tok::RPar, "')'");
            return;
        }
        if (tk.kind != Tok::Ident) {
            err("unexpected token '" + tk.text + "'");
            next();
            return;
        }
        if (tk.text == "AND" || tk.text == "OR" || tk.text == "NOT") {
            err("misplaced operator " + tk.text);
            next();
            return;
        }
        if (tk.text.rfind("K_", 0) == 0) {
            next();
            r_.constantRefs.insert(tk.text);
            if (!v_.constants.count(tk.text)) err("unknown constant " + tk.text);
            return;
        }
        if (t_[p_ + 1].kind == Tok::LPar) {
            next();
            call(tk.text);
            return;
        }
        reference();
    }
    void call(const string& name) {
        const auto it = v_.functions.find(name);
        if (it == v_.functions.end()) err("unknown function " + name);
        next();
        int argc = 0;
        if (peek().kind != Tok::RPar) {
            expr();
            ++argc;
            while (peek().kind == Tok::Comma) { next(); expr(); ++argc; }
        }
        expect(Tok::RPar, "')'");
        if (it != v_.functions.end()) {
            const auto [lo, hi] = it->second;
            if (argc < lo || (hi >= 0 && argc > hi)) err("wrong argument count for " + name);
        }
    }
    void history() {
        if (peek().kind == Tok::LBr && t_[p_ + 1].kind == Tok::Op && t_[p_ + 1].text == "-") {
            next();
            next();
            add();
            expect(Tok::RBr, "']'");
        }
    }
    void scope() {
        if (peek().kind != Tok::At) return;
        next();
        if (peek().kind == Tok::Num) {
            string label = next().text;
            if (peek().kind == Tok::Ident) label += next().text;
            if (!v_.scopes.count(label)) err("unknown scope @" + label);
            r_.scopeLabels.insert(label);
            return;
        }
        if (peek().kind != Tok::Ident) { err("bad scope"); return; }
        const string label = next().text;
        if (!v_.scopes.count(label)) err("unknown scope @" + label);
        r_.scopeLabels.insert(label);
    }
    void reference() {
        const string root = next().text;
        if (root == "true" || root == "false") return;
        if (root == "custom_signals") {
            expect(Tok::Dot, "'.'");
            if (peek().kind != Tok::Ident) { err("custom_signals needs a name"); return; }
            const string name = next().text;
            r_.customRefs.insert(name);
            if (!customs_.count(name)) err("undefined custom signal custom_signals." + name);
            history();
            scope();
            return;
        }
        if (root == "external") {
            r_.external = true;
            while (peek().kind == Tok::Dot || peek().kind == Tok::Ident || peek().kind == Tok::LBr ||
                   peek().kind == Tok::RBr || peek().kind == Tok::Num) {
                if (peek().kind == Tok::LBr && t_[p_ + 1].kind == Tok::Op) break;
                next();
            }
            history();
            scope();
            return;
        }
        if (v_.collections.count(root)) {
            if (peek().kind == Tok::LBr && t_[p_ + 1].kind == Tok::Num) {
                next();
                next();
                expect(Tok::RBr, "']'");
            } else if (peek().kind == Tok::LBr) {
                err("collection " + root + " takes a non-negative literal index");
            }
            if (peek().kind != Tok::Dot) { err("collection " + root + " needs a field"); return; }
            next();
            if (peek().kind != Tok::Ident) { err("missing field after " + root); return; }
            const string field = next().text;
            const auto& fields = v_.collectionFields.at(root);
            if (!fields.count(field)) err("unknown field " + root + "." + field);
            r_.catalogRefs.insert(root + "." + field);
            history();
            scope();
            return;
        }
        string path = root;
        while (peek().kind == Tok::Dot) {
            next();
            if (peek().kind != Tok::Ident) { err("bad path " + path); return; }
            path += "." + next().text;
        }
        if (path == root && v_.enumWords.count(root) && !v_.bareNames.count(root)) return;
        if (v_.bareNames.count(path) || v_.paths.count(path)) {
            r_.catalogRefs.insert(path);
        } else {
            err("unknown identifier " + path);
        }
        history();
        scope();
    }

    vector<Token> t_;
    size_t p_ = 0;
    const Vocabulary& v_;
    const set<string>& customs_;
    ExprResult& r_;
};

}  // namespace

ExprResult checkExpression(const string& text, const Vocabulary& vocab, const set<string>& customNames) {
    ExprResult r;
    int depth = 0;
    for (const char c : text) {
        if (c == '(') ++depth;
        if (c == ')' && --depth < 0) break;
    }
    if (depth != 0) r.errors.push_back("unbalanced parentheses");
    auto toks = tokenize(text, r.errors);
    if (!r.errors.empty()) return r;
    Parser parser(move(toks), vocab, customNames, r);
    parser.run();
    return r;
}
