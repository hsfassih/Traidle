#include "lint.h"

#include <functional>
#include <regex>

namespace {

bool inEnumE(const Catalog& cat, const string& e, const json& v) {
    if (!v.is_string()) return false;
    const auto it = cat.enums.find(e);
    return it != cat.enums.end() && it->second.count(v.get<string>());
}

const StrategyInfo* findStrategy(const Corpus& corpus, const string& id) {
    for (const auto& s : corpus.strategies) if (s.id == id) return &s;
    return nullptr;
}

double num(const json& v) { return v.is_number() ? v.get<double>() : 0.0; }

}  // namespace

void checkExample(const string& file, const json& e, const Catalog& cat, const Registry& reg, Corpus& corpus,
                  Diagnostics& d) {
    const string id = e.value("example_id", json("")).get<string>();
    const string W = file + ":example:" + id;
    if (!isSnake(id)) d.error(W, "example_id is not snake_case");
    for (const char* k : {"example_id", "strategy_id", "video_span", "example_role", "chart_kind", "frames",
                          "setup_walkthrough", "trade", "presenter_commentary", "engine_check", "uncertainties"})
        if (!e.contains(k)) d.error(W, string("missing key ") + k);
    const string sid = e.value("strategy_id", json("")).is_string() ? e["strategy_id"].get<string>() : "";
    const StrategyInfo* owner = findStrategy(corpus, sid);
    if (!owner) d.error(W, "strategy_id '" + sid + "' does not resolve");
    else if (owner->file != file) d.error(W, "example lives in a different file than its strategy");
    const json vs = e.value("video_span", json::object());
    for (const char* k : {"start", "end", "canonical_frame_ts"}) if (!isTs(vs.value(k, json()))) d.error(W, string("video_span.") + k + " bad ts");
    if (!inEnumE(cat, "example_role", e.value("example_role", json()))) d.error(W, "example_role invalid");
    if (!inEnumE(cat, "chart_kind", e.value("chart_kind", json()))) d.error(W, "chart_kind invalid");

    set<string> customs;
    if (owner) customs = owner->customNames;
    set<string> frameIds;
    for (auto& f : e.value("frames", json::array())) {
        const string fw = W + ":frame:" + f.value("frame_id", json("?")).dump();
        if (f.value("frame_id", json()).is_string()) frameIds.insert(f["frame_id"].get<string>());
        const json tf = f.value("timeframe", json());
        if (!tf.is_null() && !isTfField(tf, cat, false)) d.error(fw, "timeframe " + tf.dump() + " not a Traidle label");
        const json tz = f.value("chart_timezone", json());
        if (!tz.is_null() && !inEnumE(cat, "chart_timezone", tz)) d.error(fw, "chart_timezone must be IANA name or null");
        if (!inEnumE(cat, "data_locator_confidence", f.value("data_locator_confidence", json()))) d.error(fw, "data_locator_confidence invalid");
        if (!inEnumE(cat, "precision", f.value("precision", json()))) d.error(fw, "precision invalid");
        string prevTime;
        for (auto& row : f.value("candles", json::array())) {
            if (!row.is_array() || row.size() < 6) { d.error(fw, "candle row malformed"); continue; }
            const double o = num(row[2]), h = num(row[3]), l = num(row[4]), c = num(row[5]);
            if (h < max(o, c) || l > min(o, c) || l > h) d.error(fw, "OHLC sanity failed at row " + row[0].dump());
            if (row[1].is_string()) {
                if (!prevTime.empty() && row[1].get<string>() <= prevTime) d.error(fw, "candle times not ascending at row " + row[0].dump());
                prevTime = row[1].get<string>();
            }
        }
        const json so = f.value("signals_observed", json::object());
        for (auto& x : so.value("fvgs", json::array())) if (!inEnumE(cat, "fvg_state", x.value("state", json()))) d.error(fw, "fvgs[].state invalid " + x.value("state", json()).dump());
        for (auto& x : so.value("structural_events", json::array())) if (!inEnumE(cat, "structural_event_kind", x.value("kind", json()))) d.error(fw, "structural_events[].kind invalid " + x.value("kind", json()).dump());
        for (auto& x : so.value("order_blocks", json::array())) if (!inEnumE(cat, "order_block_subtype", x.value("subtype", json()))) d.error(fw, "order_blocks[].subtype invalid " + x.value("subtype", json()).dump());
        if (!inEnumE(cat, "kill_zone_observed", so.value("kill_zone", json()))) d.error(fw, "kill_zone invalid " + so.value("kill_zone", json()).dump());
        const json b = so.value("bias", json());
        if (!b.is_null() && !inEnumE(cat, "bias", b)) d.error(fw, "bias invalid " + b.dump());
        for (auto& x : f.value("custom_signals_observed", json::array())) {
            const json n = x.value("name", json());
            if (!n.is_string() || (!customs.count(n.get<string>()) && !reg.entries.count(n.get<string>())))
                d.error(fw, "custom_signals_observed name " + n.dump() + " not defined in strategy or registry");
        }
    }
    for (auto& w : e.value("setup_walkthrough", json::array())) {
        const json m = w.value("maps_to_signal_step", json());
        if (!m.is_null() && (!m.is_number_integer() || !owner || !owner->steps.count(m.get<int>())))
            d.error(W, "maps_to_signal_step " + m.dump() + " does not exist in the strategy");
        if (!isTs(w.value("ts", json()))) d.error(W, "setup_walkthrough ts invalid");
        const json fid = w.value("frame_id", json());
        if (!fid.is_null() && (!fid.is_string() || !frameIds.count(fid.get<string>()))) d.error(W, "walkthrough frame_id " + fid.dump() + " unknown");
    }
    const json tr = e.value("trade", json::object());
    if (!inEnumE(cat, "trade_outcome", tr.value("outcome", json()))) d.error(W, "trade.outcome invalid " + tr.value("outcome", json()).dump());
    const json dir = tr.value("direction", json());
    if (!dir.is_null() && !inEnumE(cat, "trade_direction", dir)) d.error(W, "trade.direction invalid");
    const json prec = tr.value("precision", json());
    if (!prec.is_null() && !inEnumE(cat, "precision", prec)) d.error(W, "trade.precision invalid");
    const json ec = e.value("engine_check", json::object());
    for (auto& x : ec.value("should_detect", json::array()))
        if (!x.is_string() || !cat.detections.count(x.get<string>())) d.error(W, "engine_check.should_detect " + x.dump() + " is not a catalog detection");
    for (auto& x : ec.value("may_differ", json::array())) {
        const json s = x.value("signal", json());
        bool ok = s.is_string() && (cat.detections.count(s.get<string>()) || cat.signals.count(s.get<string>()));
        if (s.is_string() && s.get<string>().rfind("custom_signals.", 0) == 0) {
            const string n = s.get<string>().substr(15);
            ok = customs.count(n) || reg.entries.count(n);
        }
        if (!ok) d.error(W, "engine_check.may_differ signal " + s.dump() + " does not resolve");
    }
    for (auto& u : e.value("uncertainties", json::array())) if (!isTs(u.value("ts", json()))) d.error(W, "uncertainty ts invalid");
}

TierInfo computeTier(const StrategyInfo& s, const Catalog& cat, const Registry& reg) {
    TierInfo t;
    const json& o = s.obj;
    const json tf = o.value("timeframes", json::object());
    for (const char* r : {"bias_tf", "setup_tf", "entry_tf"}) {
        const json v = tf.value(r, json());
        if (!v.is_string()) continue;
        string str = v.get<string>();
        size_t start = 0;
        while (true) {
            const size_t bar = str.find('|', start);
            t.timeframes.insert(str.substr(start, bar == string::npos ? string::npos : bar - start));
            if (bar == string::npos) break;
            start = bar + 1;
        }
    }
    set<string> regNames;
    for (auto& [n, _] : reg.entries) regNames.insert(n);
    map<string, pair<bool, bool>> memo;
    function<pair<bool, bool>(const string&, int)> closure = [&](const string& name, int depth) -> pair<bool, bool> {
        if (memo.count(name)) return memo[name];
        const auto it = reg.entries.find(name);
        if (it == reg.entries.end() || depth > 16) return {false, false};
        const json nd = it->second["numeric_definition"];
        bool computable = !nd.is_null();
        bool external = false;
        for (const json* ex : {&nd, &it->second["value_expr"]}) {
            if (!ex->is_string()) continue;
            auto r = checkExpression(ex->get<string>(), cat.vocab, regNames);
            external = external || r.external;
            for (const auto& l : r.scopeLabels) if (cat.tfLabels.count(l)) t.timeframes.insert(l);
            for (const auto& c : r.customRefs) {
                const auto sub = closure(c, depth + 1);
                computable = computable && sub.first;
                external = external || sub.second;
            }
        }
        return memo[name] = {computable, external};
    };
    bool allEngine = true;
    bool anyNull = false;
    bool anyExternal = false;
    const auto steps = o.value("signals_required", json::array());
    for (auto& st : steps) {
        const string sig = st.value("signal", json("")).get<string>();
        if (sig.rfind("custom_signals.", 0) == 0) {
            allEngine = false;
            const string n = sig.substr(15);
            t.customSignals.insert(n);
            const auto c = closure(n, 0);
            if (!c.first || c.second) { ++t.nonComputableSteps; anyNull = anyNull || !c.first; anyExternal = anyExternal || c.second; }
        } else {
            t.engineSignals.insert(sig);
        }
        for (auto& [k, v] : st.items()) {
            if (k.find("_signal") == string::npos) continue;
            for (auto& ref : v.is_array() ? v : json::array({v})) {
                if (!ref.is_string() || ref.get<string>().rfind("custom_signals.", 0) != 0) continue;
                allEngine = false;
                const string n = ref.get<string>().substr(15);
                t.customSignals.insert(n);
                const auto c = closure(n, 0);
                if (!c.first || c.second) { ++t.nonComputableSteps; anyNull = anyNull || !c.first; anyExternal = anyExternal || c.second; }
            }
        }
        const json tfv = st.value("timeframe", json());
        if (tfv.is_string() && cat.tfLabels.count(tfv.get<string>())) t.timeframes.insert(tfv.get<string>());
    }
    for (const char* key : {"entry_rule", "target_rule", "stop_rule"}) {
        const json sd = o.value(key, json::object()).value("structured", json());
        if (!sd.is_object() || !sd.value("reference", json()).is_string()) continue;
        const string ref = sd["reference"].get<string>();
        if (ref.rfind("custom_signals.", 0) == 0) {
            allEngine = false;
            const string n = ref.substr(15);
            t.customSignals.insert(n);
            const auto c = closure(n, 0);
            anyExternal = anyExternal || c.second;
            anyNull = anyNull || !c.first;
        }
    }
    for (const char* key : {"invalidation", "filters"}) {
        for (auto& it : o.value(key, json::array())) {
            const json nd = it.value("numeric_definition", json());
            if (nd.is_string() && nd.get<string>().find("external.") != string::npos) anyExternal = true;
        }
    }
    if (steps.empty()) {
        for (auto& cs : o.value("custom_signals", json::array())) {
            const string n = cs.value("name", json("")).get<string>();
            t.customSignals.insert(n);
            allEngine = false;
            const auto c = closure(n, 0);
            anyNull = anyNull || !c.first;
            anyExternal = anyExternal || c.second;
        }
    }
    t.tier = (anyNull || anyExternal) ? 'C' : (allEngine ? 'A' : 'B');
    return t;
}

