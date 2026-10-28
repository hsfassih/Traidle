#include "lint.h"

#include <regex>

namespace {

const set<string> kStepOps = {"equals", "in", "gt", "gte", "lt", "lte", "between", "crosses_above", "crosses_below",
                              "equals_signal", "gt_signal", "gte_signal", "lt_signal", "lte_signal",
                              "between_signal", "crosses_above_signal", "crosses_below_signal"};
const set<string> kStepBase = {"step", "signal", "timeframe", "optional", "max_candles_after_previous_step",
                               "traidle_supported", "provenance", "ts"};
const set<string> kStrategyKeys = {"strategy_id", "aliases", "category", "summary", "direction", "symmetric",
                                   "timeframes", "requires_context", "glossary", "signals_required",
                                   "custom_signals", "sequence", "invalidation", "filters", "entry_rule",
                                   "target_rule", "stop_rule", "management", "risk", "reinforces",
                                   "conflicts_with", "weight", "historical_stats", "presenter_claims", "notes",
                                   "examples", "uncertainties", "confidence"};
const regex kNoRule(R"(^\s*(not (explicitly )?(defined|specified|covered|provided|stated)|no (explicit|specific)|none\b))", regex::icase);

bool inEnum(const Catalog& cat, const string& e, const json& v) {
    if (!v.is_string()) return false;
    const auto it = cat.enums.find(e);
    return it != cat.enums.end() && it->second.count(v.get<string>());
}

void checkExpr(const json& v, const string& where, const set<string>& customs, const Catalog& cat, Corpus& corpus,
               Diagnostics& d, ExprResult* out = nullptr) {
    if (v.is_null()) return;
    if (!v.is_string()) {
        d.error(where, "numeric_definition must be a string or null");
        return;
    }
    auto r = checkExpression(v.get<string>(), cat.vocab, customs);
    for (const auto& e : r.errors) d.error(where, "numeric_definition: " + e);
    corpus.usedConstants.insert(r.constantRefs.begin(), r.constantRefs.end());
    if (out) *out = r;
}

bool resolvesSignal(const string& sig, const Catalog& cat, const set<string>& customs, bool& isCustom) {
    isCustom = sig.rfind("custom_signals.", 0) == 0;
    if (isCustom) return customs.count(sig.substr(15)) > 0;
    const auto it = cat.signals.find(sig);
    return it != cat.signals.end() && it->second.value("usable_in_steps", false);
}

bool resolvesPriceRef(const string& ref, const Catalog& cat, const set<string>& customs, bool& isCustom) {
    static const regex re(R"(^([a-z_]+)(\[\d+\])?(\.[a-z_0-9]+)?(@[a-z0-9_]+)?$)");
    smatch m;
    isCustom = ref.rfind("custom_signals.", 0) == 0;
    if (isCustom) return customs.count(ref.substr(15)) > 0;
    if (!regex_match(ref, m, re)) return false;
    if (m[4].matched) {
        const string scope = m[4].str().substr(1);
        if (!cat.vocab.scopes.count(scope)) return false;
    }
    string base = m[1].str();
    static const map<string, string> views = {{"bullish_fvg", "fvg"}, {"bearish_fvg", "fvg"},
                                              {"bullish_ob", "order_block"}, {"bearish_ob", "order_block"}};
    if (views.count(base)) base = views.at(base);
    const string path = base + (m[3].matched ? m[3].str() : "");
    if (m[2].matched && !cat.vocab.collections.count(m[1].str())) return false;
    return cat.priceRefs.count(path) > 0;
}

void checkValueDomain(const json& sigDef, const string& op, const json& val, const string& where, Diagnostics& d,
                      const Catalog& cat) {
    const string type = sigDef.value("type", "");
    const json dom = sigDef.value("domain", json());
    auto okScalar = [&](const json& x) {
        if (type == "bool") return x.is_boolean();
        if (type == "enum") return dom.is_string() && inEnum(cat, dom.get<string>(), x);
        return x.is_number();
    };
    if (op == "equals") {
        if (!okScalar(val)) d.error(where, "value " + val.dump() + " outside the domain of the signal");
    } else if (op == "in") {
        if (!val.is_array() || val.empty()) d.error(where, "'in' needs a non-empty array");
        else for (auto& x : val) if (!okScalar(x)) d.error(where, "value " + x.dump() + " outside the domain");
    } else if (op == "between") {
        if (!val.is_array() || val.size() != 2 || !val[0].is_number() || !val[1].is_number())
            d.error(where, "'between' needs [low, high]");
    } else if (op.find("_signal") == string::npos) {
        if (!val.is_number()) d.error(where, op + " needs a number");
        if (type != "number" && type != "int") d.error(where, op + " applied to non-numeric signal");
    }
}

}  // namespace

bool isSnake(const string& s) {
    static const regex re("^[a-z][a-z0-9_]*$");
    return regex_match(s, re);
}

bool isTs(const json& v) {
    if (v.is_null()) return true;
    if (!v.is_string()) return false;
    static const regex mmss("^[0-5][0-9]:[0-5][0-9]$");
    static const regex hhmmss("^(0[1-9]|[1-9][0-9]):[0-5][0-9]:[0-5][0-9]$");
    const string s = v.get<string>();
    return regex_match(s, mmss) || regex_match(s, hhmmss);
}

bool isTfField(const json& v, const Catalog& cat, bool allowRole) {
    if (v.is_null()) return !allowRole;
    if (!v.is_string()) return false;
    const string s = v.get<string>();
    if (allowRole && cat.roles.count(s)) return true;
    size_t start = 0;
    while (true) {
        const size_t bar = s.find('|', start);
        const string part = s.substr(start, bar == string::npos ? string::npos : bar - start);
        if (!cat.tfLabels.count(part)) return false;
        if (bar == string::npos) return true;
        start = bar + 1;
    }
}

void checkGlobalStrings(const json& node, const string& where, Diagnostics& d) {
    if (node.is_object()) {
        for (auto& [k, v] : node.items()) checkGlobalStrings(v, where, d);
    } else if (node.is_array()) {
        for (auto& v : node) checkGlobalStrings(v, where, d);
    } else if (node.is_string()) {
        const string s = node.get<string>();
        if (s.empty() || s == "null" || s == "N/A" || s == "n/a" || s == "None")
            d.error(where, "placeholder string \"" + s + "\" must be JSON null");
    }
}

void checkRegistry(const Catalog& cat, Registry& reg, Corpus& corpus, Diagnostics& d) {
    set<string> names;
    for (auto& [n, _] : reg.entries) names.insert(n);
    for (auto& [name, e] : reg.entries) {
        const string where = "custom_signals.json:" + name;
        if (!isSnake(name)) d.error(where, "name is not snake_case");
        if (!inEnum(cat, "custom_signal_kind", e["kind"])) d.error(where, "kind not in closed set");
        if (!inEnum(cat, "definition_source", e["definition_source"])) d.error(where, "bad definition_source");
        checkExpr(e["numeric_definition"], where, names, cat, corpus, d);
        if (e.contains("value_expr")) checkExpr(e["value_expr"], where + ".value_expr", names, cat, corpus, d);
        if (e["numeric_definition"].is_null() && (!e.contains("not_computable_reason") || e["not_computable_reason"].is_null()))
            d.error(where, "null numeric_definition needs not_computable_reason");
        for (auto& t : e.value("market_tags", json::array()))
            if (!inEnum(cat, "market_tag", t)) d.error(where, "unknown market tag " + t.dump());
    }
}

void checkStrategy(const StrategyInfo& s, const Catalog& cat, const Registry& reg, Corpus& corpus, Diagnostics& d) {
    const json& o = s.obj;
    const string W = s.file + ":" + s.id;
    for (auto& k : kStrategyKeys) if (!o.contains(k)) d.error(W, "missing key " + k);
    if (!isSnake(s.id)) d.error(W, "strategy_id is not snake_case");
    const bool roleCat = inEnum(cat, "category_role", o.value("category", json()));
    if (!inEnum(cat, "category_entry", o.value("category", json())) && !roleCat)
        d.error(W, "category " + o.value("category", json()).dump() + " not in closed set");
    if (!inEnum(cat, "direction", o.value("direction", json()))) d.error(W, "bad direction");
    if (!o.value("symmetric", json()).is_boolean()) d.error(W, "symmetric must be boolean");
    if (!o.value("summary", json()).is_string()) d.error(W, "summary must be a string");

    const json tf = o.value("timeframes", json::object());
    for (const char* r : {"bias_tf", "setup_tf", "entry_tf"})
        if (!isTfField(tf.value(r, json()), cat, false)) d.error(W, string("timeframes.") + r + " invalid: " + tf.value(r, json()).dump());
    if (!tf.value("strict", json()).is_boolean()) d.error(W, "timeframes.strict must be boolean");

    const json rc = o.value("requires_context", json::object());
    for (auto& b : rc.value("bias", json::array())) if (!inEnum(cat, "bias", b)) d.error(W, "requires_context.bias value " + b.dump());
    for (auto& x : rc.value("session", json::array())) if (!inEnum(cat, "session", x)) d.error(W, "requires_context.session value " + x.dump());
    const json tw = rc.value("time_window_ny", json());
    static const regex twRe("^([01][0-9]|2[0-4]):[0-5][0-9]-([01][0-9]|2[0-4]):[0-5][0-9]$");
    if (!tw.is_null() && (!tw.is_string() || !regex_match(tw.get<string>(), twRe))) d.error(W, "time_window_ny must be HH:MM-HH:MM or null");
    const json dow = rc.value("day_of_week", json());
    if (!dow.is_null()) {
        if (!dow.is_array()) d.error(W, "day_of_week must be an array or null");
        else for (auto& x : dow) if (!inEnum(cat, "day_of_week", x)) d.error(W, "day_of_week value " + x.dump());
    }
    for (auto& x : rc.value("other", json::array())) {
        if (!x.is_string()) { d.error(W, "requires_context.other entries must be strings"); continue; }
        const string v = x.get<string>();
        bool isCustom = false;
        if (v.rfind("market:", 0) == 0) { if (!inEnum(cat, "market_tag", x)) d.error(W, "unknown market tag " + v); }
        else if (!resolvesSignal(v, cat, s.customNames, isCustom) && !cat.signals.count(v))
            d.error(W, "requires_context.other '" + v + "' is not a catalog/custom reference or market tag");
    }

    set<string> usedCustom;
    int expected = 1;
    for (auto& st : o.value("signals_required", json::array())) {
        const string w = W + ":step" + st.value("step", json()).dump();
        if (!st.value("step", json()).is_number_integer() || st["step"].get<int>() != expected) d.error(w, "steps must be consecutive from 1");
        ++expected;
        int ops = 0;
        string op;
        for (auto& [k, _] : st.items()) {
            if (kStepOps.count(k)) { ++ops; op = k; }
            else if (!kStepBase.count(k)) d.error(w, "unknown key " + k);
        }
        if (ops != 1) d.error(w, "exactly one operator key required");
        const string sig = st.value("signal", json("")).is_string() ? st["signal"].get<string>() : "";
        bool isCustom = false;
        if (!resolvesSignal(sig, cat, s.customNames, isCustom)) d.error(w, "signal '" + sig + "' does not resolve");
        else if (isCustom) usedCustom.insert(sig.substr(15));
        if (st.value("traidle_supported", json()) != json(!isCustom)) d.error(w, "traidle_supported must be " + string(isCustom ? "false (custom)" : "true (catalog)"));
        if (!isTfField(st.value("timeframe", json()), cat, true)) d.error(w, "timeframe must be a role or label");
        if (!st.value("optional", json()).is_boolean()) d.error(w, "optional must be boolean");
        const json mc = st.value("max_candles_after_previous_step", json());
        if (!mc.is_null() && (!mc.is_number_integer() || mc.get<int>() < 0)) d.error(w, "max_candles_after_previous_step must be a non-negative integer or null");
        if (!inEnum(cat, "provenance", st.value("provenance", json()))) d.error(w, "bad provenance");
        if (!isTs(st.value("ts", json()))) d.error(w, "bad ts");
        if (ops == 1) {
            const json val = st[op];
            if (op.find("_signal") != string::npos) {
                const auto refs = val.is_array() ? val : json::array({val});
                for (auto& r : refs) {
                    bool c2 = false;
                    if (!r.is_string() || (!resolvesSignal(r.get<string>(), cat, s.customNames, c2) && !cat.signals.count(r.get<string>())))
                        d.error(w, "comparison signal " + r.dump() + " does not resolve");
                    else if (c2) usedCustom.insert(r.get<string>().substr(15));
                }
            } else if (isCustom) {
                if (!(op == "equals" && val.is_boolean())) d.error(w, "custom signals take equals true/false");
            } else if (cat.signals.count(sig)) {
                checkValueDomain(cat.signals.at(sig), op, val, w, d, cat);
            }
        }
    }

    set<string> seen;
    for (auto& cs : o.value("custom_signals", json::array())) {
        const string name = cs.value("name", json("")).is_string() ? cs["name"].get<string>() : "";
        const string w = W + ":custom_signals." + name;
        if (!seen.insert(name).second) d.error(w, "duplicate custom signal name");
        if (!isSnake(name)) d.error(w, "name is not snake_case");
        for (const char* k : {"name", "kind", "plain_definition", "numeric_definition", "definition_source", "ts"})
            if (!cs.contains(k)) d.error(w, string("missing key ") + k);
        const auto it = reg.entries.find(name);
        if (it == reg.entries.end()) d.error(w, "not in registry custom_signals.json");
        else for (const char* k : {"kind", "plain_definition", "numeric_definition", "definition_source"})
            if (cs.value(k, json()) != it->second.value(k, json())) d.error(w, string(k) + " differs from the registry");
        ExprResult r;
        checkExpr(cs.value("numeric_definition", json()), w, s.customNames, cat, corpus, d, &r);
        usedCustom.insert(r.customRefs.begin(), r.customRefs.end());
        if (cs.value("numeric_definition", json()).is_null()) d.warn(w, "numeric_definition is null (not computable)");
        if (!isTs(cs.value("ts", json()))) d.error(w, "bad ts");
    }

    for (const char* key : {"invalidation", "filters"}) {
        int i = 0;
        for (auto& it : o.value(key, json::array())) {
            const string w = W + ":" + key + "[" + to_string(i++) + "]";
            if (!it.is_object() || !it.value("text", json()).is_string()) { d.error(w, "needs object with text"); continue; }
            if (!inEnum(cat, "provenance", it.value("provenance", json()))) d.error(w, "bad provenance");
            if (!isTs(it.value("ts", json()))) d.error(w, "bad ts");
            for (auto& [k, _] : it.items()) if (k != "text" && k != "provenance" && k != "ts" && k != "numeric_definition") d.error(w, "unknown key " + k);
            if (!it.contains("numeric_definition") || it["numeric_definition"].is_null()) d.warn(w, "no numeric_definition");
            else {
                ExprResult r;
                checkExpr(it["numeric_definition"], w, s.customNames, cat, corpus, d, &r);
                usedCustom.insert(r.customRefs.begin(), r.customRefs.end());
            }
        }
    }

    bool hasEntry = false;
    for (const char* key : {"entry_rule", "target_rule", "stop_rule"}) {
        const string w = W + ":" + key;
        const json r = o.value(key, json());
        if (!r.is_object()) { d.error(w, "must be an object"); continue; }
        for (const char* k : {"text", "structured", "provenance", "ts", "quote"}) if (!r.contains(k)) d.error(w, string("missing ") + k);
        const json text = r.value("text", json());
        if (text.is_string() && regex_search(text.get<string>(), kNoRule)) d.error(w, "placeholder rule text; use null text/structured/provenance");
        if (text.is_null() && (!r.value("structured", json()).is_null() || !r.value("provenance", json()).is_null()))
            d.error(w, "text null requires structured null and provenance null");
        if (!text.is_null() && !inEnum(cat, "provenance", r.value("provenance", json()))) d.error(w, "bad provenance");
        if (!isTs(r.value("ts", json()))) d.error(w, "bad ts");
        if (string(key) == "entry_rule" && !text.is_null()) hasEntry = true;
        const json sd = r.value("structured", json());
        if (sd.is_null()) continue;
        if (!sd.is_object()) { d.error(w, "structured must be object or null"); continue; }
        for (auto& [k, _] : sd.items()) if (k != "type" && k != "reference" && k != "buffer_atr") d.error(w, "structured: unknown key " + k);
        if (!inEnum(cat, "structured_type", sd.value("type", json()))) d.error(w, "structured.type invalid");
        const json ref = sd.value("reference", json());
        if (sd.value("type", json()) == json("rr_multiple")) {
            if (!ref.is_number()) d.error(w, "rr_multiple reference must be a number");
        } else {
            bool isCustom = false;
            if (!ref.is_string() || !resolvesPriceRef(ref.get<string>(), cat, s.customNames, isCustom))
                d.error(w, "structured.reference " + ref.dump() + " is not a catalog price reference or defined custom signal");
            else if (isCustom) usedCustom.insert(ref.get<string>().substr(15));
        }
        const json buf = sd.value("buffer_atr", json());
        if (buf.is_string()) {
            const string k = buf.get<string>();
            if (!reg.constants.count(k)) d.error(w, "buffer_atr constant " + k + " unknown");
            else corpus.usedConstants.insert(k);
        } else if (!buf.is_null() && !buf.is_number()) d.error(w, "buffer_atr must be number, K_* name or null");
    }
    if (!hasEntry && !roleCat) d.error(W, "no entry rule: category must be a non-entry role");

    for (auto& rc2 : o.value("requires_context", json::object()).value("other", json::array()))
        if (rc2.is_string() && rc2.get<string>().rfind("custom_signals.", 0) == 0) usedCustom.insert(rc2.get<string>().substr(15));
    for (const auto& n : s.customNames) if (!usedCustom.count(n)) d.warn(W, "custom signal " + n + " is not referenced by steps, rules, context, invalidation/filters or other custom signals");

    for (const char* key : {"reinforces", "conflicts_with"}) {
        for (auto& x : o.value(key, json::array())) {
            if (!x.is_string() || !corpus.strategyFile.count(x.get<string>())) d.error(W, string(key) + " entry " + x.dump() + " is not a strategy_id");
        }
    }
    for (auto& x : o.value("examples", json::array())) {
        if (!x.is_string()) { d.error(W, "examples must be example_id strings"); continue; }
        const auto it = corpus.exampleOwner.find(x.get<string>());
        if (it == corpus.exampleOwner.end()) d.error(W, "example " + x.get<string>() + " does not exist");
        else if (it->second != s.id) d.error(W, "example " + x.get<string>() + " belongs to " + it->second);
    }
    if (!inEnum(cat, "confidence_level", o.value("confidence", json::object()).value("level", json()))) d.error(W, "confidence.level invalid");
    for (auto& u : o.value("uncertainties", json::array())) if (!isTs(u.value("ts", json()))) d.error(W, "uncertainty ts invalid");
    for (auto& p : o.value("presenter_claims", json::array())) if (!isTs(p.value("ts", json()))) d.error(W, "presenter_claims ts invalid");
}

