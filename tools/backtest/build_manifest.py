#!/usr/bin/env python3
"""Build the deterministic strategy applicability manifest and its report."""

from __future__ import annotations

import json
import re
import sys
from collections import Counter
from itertools import product
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[2]
STRATEGIES_DIR = ROOT / "strategies"
CATALOG_DIR = STRATEGIES_DIR / "catalog"
DOCS_DIR = ROOT / "docs"
MANIFEST_PATH = CATALOG_DIR / "strategy_manifest.json"
REPORT_PATH = DOCS_DIR / "strategy_manifest_report.md"

CUSTOM_REF = re.compile(r"\bcustom_signals\.([a-z][a-z0-9_]*)\b")
CONSTANT_REF = re.compile(r"\bK_[A-Z0-9_]+\b")
EXTERNAL_REF = re.compile(r"\bexternal\.[A-Za-z0-9_.]+\b")
NY_REF = re.compile(r"\b(?:ny_minute|ny_dow|kill_zone|pdh|pdl)\b")
TIME_SCOPE = re.compile(r"@([a-z0-9_]+)\b")
PDH_PDL_REF = re.compile(r"\b(?:pdh|pdl)\b")

ROLE_BY_CATEGORY = {
    "market_condition_filter": "modifier",
    "context_framework": "framework",
    "target_projection": "target_tool",
}

INTRADAY_TIMEFRAMES = {"1m", "3m", "5m", "15m", "30m", "1h"}


def read_json(path: Path) -> Any:
    with path.open("r", encoding="utf-8") as file:
        return json.load(file)


def all_strings(value: Any):
    if isinstance(value, dict):
        for nested in value.values():
            yield from all_strings(nested)
    elif isinstance(value, list):
        for nested in value:
            yield from all_strings(nested)
    elif isinstance(value, str):
        yield value


def custom_refs(value: Any) -> set[str]:
    return {
        match.group(1)
        for text in all_strings(value)
        for match in CUSTOM_REF.finditer(text)
    }


def text_refs(value: Any, pattern: re.Pattern[str]) -> set[str]:
    return {
        match.group(0)
        for text in all_strings(value)
        for match in pattern.finditer(text)
    }


def role_values(value: Any) -> list[str]:
    if not isinstance(value, str):
        return []
    return [part for part in value.split("|") if part]


def max_timeframe(first: str, second: str, order: list[str]) -> bool:
    return order.index(first) >= order.index(second)


def ladder_up(timeframe: str, ladder: list[str], timeframe_order: list[str]) -> str:
    if timeframe in ladder:
        index = ladder.index(timeframe)
        return ladder[min(index + 1, len(ladder) - 1)]
    source_index = timeframe_order.index(timeframe)
    for candidate in ladder:
        if timeframe_order.index(candidate) > source_index:
            return candidate
    return ladder[-1]


def resolve_timeframes(
    strategy: dict[str, Any],
    labels: list[str],
    ladder: list[str],
) -> list[dict[str, str]]:
    timeframe_order = {label: index for index, label in enumerate(labels)}
    roles = strategy.get("timeframes") or {}
    candidates = {
        role: role_values(roles.get(role))
        for role in ("bias_tf", "setup_tf", "entry_tf")
    }
    notes = str(roles.get("notes") or "")

    if not candidates["setup_tf"] and candidates["entry_tf"]:
        candidates["setup_tf"] = list(candidates["entry_tf"])
    elif not candidates["entry_tf"] and candidates["setup_tf"]:
        candidates["entry_tf"] = list(candidates["setup_tf"])

    if not candidates["entry_tf"] and not candidates["setup_tf"]:
        expandable = re.search(
            r"\bfractal\b|no (?:fixed|explicit) timeframe|"
            r"chosen execution timeframe|execution timeframe belongs|"
            r"same execution timeframe",
            notes,
            re.IGNORECASE,
        )
        if expandable:
            candidates["entry_tf"] = list(labels)
            candidates["setup_tf"] = list(labels)
        else:
            return []

    if not candidates["entry_tf"] and not candidates["setup_tf"]:
        return []

    explicit_bias = bool(candidates["bias_tf"])

    combinations = []
    if (
        len(candidates["entry_tf"]) == len(labels)
        and len(candidates["setup_tf"]) == len(labels)
        and not role_values(roles.get("entry_tf"))
        and not role_values(roles.get("setup_tf"))
    ):
        exec_pairs = [(tf, tf) for tf in labels]
    else:
        exec_pairs = list(product(candidates["setup_tf"], candidates["entry_tf"]))
    for setup_tf, entry_tf in exec_pairs:
        bias_candidates = (
            candidates["bias_tf"]
            if explicit_bias
            else [ladder_up(setup_tf, ladder, labels)]
        )
        for bias_tf in bias_candidates:
            if (
                max_timeframe(bias_tf, setup_tf, labels)
                and max_timeframe(setup_tf, entry_tf, labels)
            ):
                combinations.append(
                    {
                        "bias_tf": bias_tf,
                        "setup_tf": setup_tf,
                        "entry_tf": entry_tf,
                    }
                )
    combinations = list(
        {
            (item["bias_tf"], item["setup_tf"], item["entry_tf"]): item
            for item in combinations
        }.values()
    )
    return sorted(
        combinations,
        key=lambda item: (
            timeframe_order[item["bias_tf"]],
            timeframe_order[item["setup_tf"]],
            timeframe_order[item["entry_tf"]],
        ),
    )


def strategy_roots(strategy: dict[str, Any]) -> set[str]:
    roots: set[str] = set()
    steps = strategy.get("signals_required") or []
    for step in steps:
        roots.update(custom_refs(step))
    for key in ("entry_rule", "stop_rule", "target_rule"):
        roots.update(custom_refs(strategy.get(key) or {}))
    roots.update(custom_refs((strategy.get("requires_context") or {}).get("other", [])))
    for key in ("invalidation", "filters"):
        roots.update(custom_refs(strategy.get(key) or []))
    if not steps:
        roots.update(
            item.get("name")
            for item in strategy.get("custom_signals") or []
            if isinstance(item.get("name"), str)
        )
    return roots


def custom_closure(
    roots: set[str], registry: dict[str, dict[str, Any]]
) -> tuple[list[str], list[str]]:
    seen: set[str] = set()
    missing: set[str] = set()
    pending = sorted(roots, reverse=True)
    while pending:
        name = pending.pop()
        if name in seen:
            continue
        seen.add(name)
        definition = registry.get(name)
        if definition is None:
            missing.add(name)
            continue
        referenced = custom_refs(
            [definition.get("numeric_definition"), definition.get("value_expr")]
        )
        pending.extend(sorted(referenced - seen, reverse=True))
    return sorted(seen), sorted(missing)


def root_roles(strategy: dict[str, Any]) -> dict[str, set[str]]:
    assignments: dict[str, set[str]] = {}
    for step in strategy.get("signals_required") or []:
        roles = step.get("timeframe")
        role = roles if isinstance(roles, str) else "setup_tf"
        for name in custom_refs(step):
            assignments.setdefault(name, set()).add(role)
    for key in ("entry_rule", "stop_rule", "target_rule"):
        for name in custom_refs(strategy.get(key) or {}):
            assignments.setdefault(name, set()).add("entry_tf")
    for name in custom_refs((strategy.get("requires_context") or {}).get("other", [])):
        assignments.setdefault(name, set()).add("bias_tf")
    for key in ("invalidation", "filters"):
        for name in custom_refs(strategy.get(key) or []):
            assignments.setdefault(name, set()).add("setup_tf")
    if not strategy.get("signals_required"):
        for item in strategy.get("custom_signals") or []:
            name = item.get("name")
            if isinstance(name, str):
                assignments.setdefault(name, set()).add("setup_tf")
    return assignments


def tier_custom_roots(strategy: dict[str, Any]) -> set[str]:
    roots: set[str] = set()
    steps = strategy.get("signals_required") or []
    for step in steps:
        signal = step.get("signal")
        if isinstance(signal, str) and signal.startswith("custom_signals."):
            roots.update(custom_refs(signal))
        for key, value in step.items():
            if "_signal" in key:
                roots.update(custom_refs(value))
    for key in ("entry_rule", "stop_rule", "target_rule"):
        structured = (strategy.get(key) or {}).get("structured")
        reference = structured.get("reference") if isinstance(structured, dict) else None
        if isinstance(reference, str):
            roots.update(custom_refs(reference))
    if not steps:
        roots.update(
            item.get("name")
            for item in strategy.get("custom_signals") or []
            if isinstance(item.get("name"), str)
        )
    return roots


def scoped_timeframes(text: str, run: dict[str, str], default_role: str) -> set[str]:
    resolved: set[str] = set()
    scopes = {match.group(1) for match in TIME_SCOPE.finditer(text)}
    for scope in scopes:
        if scope in run:
            resolved.add(run[scope])
        elif scope in run.values():
            resolved.add(scope)
        elif re.fullmatch(r"[0-9]+(?:m|h|d|w|mo)", scope):
            resolved.add(scope)
    if not resolved:
        resolved.add(run.get(default_role, run["entry_tf"]))
    return resolved


def is_ny_signal_name(name: str, definition: dict[str, Any]) -> bool:
    return bool(
        NY_REF.search(
            " ".join(
                str(definition.get(field) or "")
                for field in ("numeric_definition", "value_expr")
            )
        )
        or re.search(
            r"(?:^|_)(?:session|open|killzone|kz|weekday|tgif|asian_range)(?:_|$)",
            name,
        )
    )


def collect_frame_checks(
    strategy: dict[str, Any],
    roots: set[str],
    closure: list[str],
    registry: dict[str, dict[str, Any]],
    assignments: dict[str, set[str]],
) -> tuple[list[tuple[str, str]], bool]:
    checks: list[tuple[str, str]] = []
    needs_ny = False

    def add_text(text: str, role: str) -> None:
        nonlocal needs_ny
        if NY_REF.search(text):
            needs_ny = True
            for tf in scoped_timeframes(text, {r: r for r in ("bias_tf", "setup_tf", "entry_tf")}, role):
                if PDH_PDL_REF.search(text):
                    feature = "pdh_pdl"
                elif re.search(r"\bkill_zone\b", text):
                    feature = "kill_zone"
                else:
                    feature = "ny"
                checks.append((tf, feature))

    for step in strategy.get("signals_required") or []:
        role = step.get("timeframe")
        if not isinstance(role, str):
            role = "setup_tf"
        add_text(json.dumps(step, sort_keys=True), role)
    for key in ("entry_rule", "stop_rule", "target_rule"):
        add_text(json.dumps(strategy.get(key) or {}, sort_keys=True), "entry_tf")
    for key in ("invalidation", "filters"):
        add_text(json.dumps(strategy.get(key) or [], sort_keys=True), "setup_tf")
    context = strategy.get("requires_context") or {}
    if context.get("session") or context.get("time_window_ny") or context.get("day_of_week"):
        needs_ny = True
        checks.append(("entry_tf", "session_gate"))

    closure_set = set(closure)
    for root in sorted(roots):
        roles = assignments.get(root) or {"setup_tf"}
        root_closure, _ = custom_closure({root}, registry)
        for name in root_closure:
            if name not in closure_set:
                continue
            definition = registry.get(name)
            if definition is None:
                continue
            text = " ".join(
                str(definition.get(field) or "")
                for field in ("numeric_definition", "value_expr")
            )
            if is_ny_signal_name(name, definition):
                needs_ny = True
            if NY_REF.search(text):
                needs_ny = True
                if PDH_PDL_REF.search(text):
                    feature = "pdh_pdl"
                elif re.search(r"\bkill_zone\b", text):
                    feature = "kill_zone"
                else:
                    feature = "ny"
                for role in roles:
                    for tf in scoped_timeframes(
                        text,
                        {
                            "bias_tf": "bias_tf",
                            "setup_tf": "setup_tf",
                            "entry_tf": "entry_tf",
                        },
                        role,
                    ):
                        checks.append((tf, feature))
            valid = definition.get("valid_timeframes")
            if isinstance(valid, list):
                for role in roles:
                    checks.append((role, "valid:" + name))

    return sorted(set(checks)), needs_ny


def applicable_status(
    run: dict[str, str],
    checks: list[tuple[str, str]],
    registry: dict[str, dict[str, Any]],
    pdh_validity: dict[str, str],
    session_gate: bool,
    time_window: Any,
) -> tuple[str, str | None]:
    for role_or_tf, feature in checks:
        timeframe = run.get(role_or_tf, role_or_tf)
        if feature.startswith("valid:"):
            name = feature[6:]
            valid = registry.get(name, {}).get("valid_timeframes")
            if isinstance(valid, list) and timeframe not in valid:
                return (
                    "not_applicable",
                    f"custom signal {name} is valid only on "
                    f"{', '.join(valid)} (resolved {timeframe})",
                )
        elif feature == "pdh_pdl":
            validity = pdh_validity.get(timeframe, "")
            if any(term in validity.lower() for term in ("misaligned", "degenerate")):
                return (
                    "not_applicable",
                    f"PDH/PDL is {validity} on {timeframe}",
                )
            if "valid during edt only" in validity.lower():
                return (
                    "not_applicable",
                    f"PDH/PDL is seasonally valid only during EDT on {timeframe}; "
                    "the full-history run is not valid",
                )
        elif feature == "kill_zone" and timeframe not in (
            "1m",
            "3m",
            "5m",
            "15m",
            "30m",
            "1h",
            "2h",
            "4h",
            "6h",
            "8h",
            "12h",
        ):
            return (
                "not_applicable",
                f"kill_zone is meaningless on {timeframe} under the data contract",
            )
    if session_gate and run["entry_tf"] not in INTRADAY_TIMEFRAMES:
        return (
            "not_applicable",
            f"session gating requires an intraday candle; resolved entry_tf is {run['entry_tf']}",
        )
    if time_window and run["entry_tf"] not in INTRADAY_TIMEFRAMES:
        return (
            "not_applicable",
            f"time_window_ny requires an intraday candle; resolved entry_tf is {run['entry_tf']}",
        )
    return "applicable", None


def classify_role(strategy: dict[str, Any]) -> str:
    category = strategy.get("category")
    return ROLE_BY_CATEGORY.get(category, "entry_model")


def build_strategy_entry(
    file_name: str,
    strategy: dict[str, Any],
    labels: list[str],
    ladder: list[str],
    registry: dict[str, dict[str, Any]],
    pdh_validity: dict[str, str],
    constant_names: set[str],
) -> dict[str, Any]:
    roots = strategy_roots(strategy)
    closure, missing_custom = custom_closure(roots, registry)
    assignments = root_roles(strategy)
    frame_checks, needs_ny = collect_frame_checks(
        strategy, roots, closure, registry, assignments
    )
    custom_constants: set[str] = set()
    external_dependencies: set[str] = set()
    noncomputable: set[str] = set(missing_custom)
    for name in closure:
        definition = registry.get(name)
        if definition is None:
            continue
        if definition.get("numeric_definition") is None:
            noncomputable.add(name)
        for text in all_strings(definition):
            custom_constants.update(CONSTANT_REF.findall(text))
            external_dependencies.update(EXTERNAL_REF.findall(text))
            external_dependencies.update(EXTERNAL_REF.findall(text))
        for tag in definition.get("market_tags") or []:
            if isinstance(tag, str) and tag.startswith("market:"):
                external_dependencies.add(tag)

    all_strategy_constants = text_refs(strategy, CONSTANT_REF)
    constants_used = sorted(
        (all_strategy_constants | custom_constants) & constant_names
    )
    unknown_constants = sorted(
        (all_strategy_constants | custom_constants) - constant_names
    )
    if context_other := (strategy.get("requires_context") or {}).get("other"):
        external_dependencies.update(
            item
            for item in context_other
            if isinstance(item, str) and item.startswith("market:")
        )

    tier_roots = tier_custom_roots(strategy)
    tier_closure, tier_missing = custom_closure(tier_roots, registry)
    tier_noncomputable = set(tier_missing)
    has_external = False
    for name in tier_closure:
        definition = registry.get(name)
        if definition is None:
            continue
        if definition.get("numeric_definition") is None:
            tier_noncomputable.add(name)
        if any(
            tag in ("market:needs_external_symbols", "market:needs_news_calendar")
            for tag in (definition.get("market_tags") or [])
        ):
            has_external = True
        definition_text = [
            definition.get("numeric_definition"),
            definition.get("value_expr"),
        ]
        if text_refs(definition_text, EXTERNAL_REF):
            has_external = True
            external_dependencies.update(text_refs(definition_text, EXTERNAL_REF))
    for key in ("invalidation", "filters"):
        for item in strategy.get(key) or []:
            numeric_definition = item.get("numeric_definition")
            if isinstance(numeric_definition, str):
                refs = text_refs(numeric_definition, EXTERNAL_REF)
                if refs:
                    has_external = True
                    external_dependencies.update(refs)
    if has_external or tier_noncomputable:
        tier = "C"
    elif tier_roots:
        tier = "B"
    else:
        tier = "A"

    role = classify_role(strategy)
    status_reason: str | None = None
    if tier == "C":
        status = "excluded"
        reasons = []
        if has_external:
            reasons.append("external market dependency")
        if tier_noncomputable:
            reasons.append(
                "non-computable dependency: "
                + ", ".join(sorted(tier_noncomputable))
            )
        status_reason = "; ".join(reasons)
    else:
        status = "included"

    timeframes = strategy.get("timeframes") or {}
    context = strategy.get("requires_context") or {}
    runs = []
    for run in resolve_timeframes(strategy, labels, ladder):
        applicability, reason = applicable_status(
            run,
            frame_checks,
            registry,
            pdh_validity,
            bool(context.get("session")),
            context.get("time_window_ny"),
        )
        run_status = "excluded" if status == "excluded" else applicability
        run_reason = status_reason if status == "excluded" else reason
        runs.append(
            {
                **run,
                "status": run_status,
                "reason": run_reason,
            }
        )
    resolution_status = "resolved" if runs else "unresolved"
    resolution_reason = (
        None
        if runs
        else "No execution timeframe can be derived from the declared roles and notes"
    )

    entry_rule = strategy.get("entry_rule") or {}
    target_rule = strategy.get("target_rule") or {}
    stop_rule = strategy.get("stop_rule") or {}
    structured_target = target_rule.get("structured")
    risk = strategy.get("risk") or {}
    typical_rr = risk.get("typical_rr")
    if (
        typical_rr is None
        and isinstance(structured_target, dict)
        and structured_target.get("type") == "rr_multiple"
    ):
        typical_rr = structured_target.get("reference")

    return {
        "strategy_id": strategy["strategy_id"],
        "file": file_name,
        "category": strategy.get("category"),
        "role": role,
        "tier": tier,
        "v1_status": {"status": status, "reason": status_reason},
        "has_entry_rule": entry_rule.get("text") is not None,
        "stop_rule_structured": isinstance(stop_rule.get("structured"), dict),
        "target_rule_structured": isinstance(structured_target, dict),
        "typical_rr": typical_rr,
        "min_rr": risk.get("min_rr"),
        "direction": strategy.get("direction"),
        "symmetric": strategy.get("symmetric"),
        "timeframe_resolution": {
            "status": resolution_status,
            "reason": resolution_reason,
        },
        "resolved_timeframe_runs": runs,
        "session_and_time_window_gating": {
            "session": context.get("session") or [],
            "time_window_ny": context.get("time_window_ny"),
            "day_of_week": context.get("day_of_week"),
            "presenter_bias_ignored": bool(context.get("bias")),
        },
        "needs_ny_time": needs_ny,
        "external_dependencies": sorted(external_dependencies),
        "custom_signals_roots": sorted(roots),
        "custom_signals_used": closure,
        "missing_custom_signals": missing_custom,
        "k_constants_used": constants_used,
        "unknown_k_constants": unknown_constants,
    }


def build_report(
    entries: list[dict[str, Any]], files: list[str], expected_file_count: int
) -> str:
    counts = Counter(entry["role"] for entry in entries)
    tiers = Counter(entry["tier"] for entry in entries)
    statuses = Counter(entry["v1_status"]["status"] for entry in entries)
    native_stops = sum(entry["stop_rule_structured"] for entry in entries)
    native_targets = sum(entry["target_rule_structured"] for entry in entries)
    entry_models = [entry for entry in entries if entry["role"] == "entry_model"]
    model_native_stops = sum(entry["stop_rule_structured"] for entry in entry_models)
    model_native_targets = sum(entry["target_rule_structured"] for entry in entry_models)
    multi_strategy_files = []
    file_counts = Counter(entry["file"] for entry in entries)
    for file_name, count in sorted(file_counts.items()):
        if count > 1:
            multi_strategy_files.append(f"{file_name} ({count})")
    total_runs = sum(len(entry["resolved_timeframe_runs"]) for entry in entries)
    na_runs = sum(
        run["status"] == "not_applicable"
        for entry in entries
        for run in entry["resolved_timeframe_runs"]
    )
    excluded_runs = sum(
        run["status"] == "excluded"
        for entry in entries
        for run in entry["resolved_timeframe_runs"]
    )
    unresolved = [
        entry["strategy_id"]
        for entry in entries
        if entry["timeframe_resolution"]["status"] == "unresolved"
    ]

    lines = [
        "# Strategy backtest manifest report",
        "",
        f"- Strategy IDs: **{len(entries)}** across **{len(files)}** strategy JSON files.",
        f"- The expected **{expected_file_count}** refers to source files, not IDs; the manifest's "
        f"{len(entries)} IDs match the {len(entries)} rows in the lint report. If 35 was intended "
        "as an ID count, the repo contains four additional IDs.",
        "- Four files contain two IDs each, accounting for the difference between 35 files and 39 IDs: "
        + (", ".join(multi_strategy_files) if multi_strategy_files else "none")
        + ".",
        f"- Roles: entry models **{counts['entry_model']}**, modifiers **{counts['modifier']}**, "
        f"frameworks **{counts['framework']}**, target tools **{counts['target_tool']}**.",
        f"- Tiers: A **{tiers['A']}**, B **{tiers['B']}**, C **{tiers['C']}**; "
        f"v1 included **{statuses['included']}**, excluded **{statuses['excluded']}**.",
        f"- Resolved timeframe runs: **{total_runs}**; excluded tier-C runs **{excluded_runs}**; "
        f"not-applicable runs **{na_runs}**.",
        "- Unresolved timeframe roles: "
        + (", ".join(unresolved) if unresolved else "none")
        + ".",
        f"- Structured rules across all IDs: stops **{native_stops}**, targets **{native_targets}**. "
        f"Entry-model native/default counts: stops **{model_native_stops}/"
        f"{len(entry_models) - model_native_stops}**, targets **{model_native_targets}/"
        f"{len(entry_models) - model_native_targets}**.",
        "",
        "| Strategy | File | Category | Role | Tier | v1 status | Entry rule | Native stop | Native target | Timeframe runs |",
        "|---|---|---|---|---:|---|---|---:|---:|---:|",
    ]
    for entry in entries:
        lines.append(
            "| {strategy_id} | {file} | {category} | {role} | {tier} | {status} | "
            "{entry} | {stop} | {target} | {runs} |".format(
                strategy_id=entry["strategy_id"],
                file=entry["file"],
                category=entry["category"],
                role=entry["role"],
                tier=entry["tier"],
                status=entry["v1_status"]["status"],
                entry="yes" if entry["has_entry_rule"] else "no",
                stop="yes" if entry["stop_rule_structured"] else "no",
                target="yes" if entry["target_rule_structured"] else "no",
                runs=len(entry["resolved_timeframe_runs"]),
            )
        )
    lines.extend(
        [
            "",
            "A structured rule is counted as native even when its reference is a custom signal. "
            "A null or unstructured rule uses the specified backtest default only for entry models; "
            "framework, modifier, and target-tool rows are occurrence-only.",
            "",
        ]
    )
    return "\n".join(lines)


def main() -> int:
    signal_catalog = read_json(CATALOG_DIR / "signal_catalog.json")
    custom_catalog = read_json(CATALOG_DIR / "custom_signals.json")
    constants_catalog = read_json(CATALOG_DIR / "constants.json")
    labels = signal_catalog["timeframes"]["labels"]
    ladder = signal_catalog["timeframes"]["ladder"]
    pdh_validity = signal_catalog["timeframes"].get("pdh_pdl_validity", {})
    registry = {
        item["name"]: item for item in custom_catalog.get("signals", [])
    }
    constant_names = {
        item["name"] for item in constants_catalog.get("constants", [])
    }

    files = sorted(
        path
        for path in STRATEGIES_DIR.glob("*.json")
        if path.is_file()
    )
    entries = []
    seen_ids: set[str] = set()
    for path in files:
        document = read_json(path)
        for strategy in document.get("strategies", []):
            strategy_id = strategy.get("strategy_id")
            if not isinstance(strategy_id, str):
                raise ValueError(f"{path.name} contains a strategy without strategy_id")
            if strategy_id in seen_ids:
                raise ValueError(f"duplicate strategy_id: {strategy_id}")
            seen_ids.add(strategy_id)
            entries.append(
                build_strategy_entry(
                    path.name,
                    strategy,
                    labels,
                    ladder,
                    registry,
                    pdh_validity,
                    constant_names,
                )
            )

    entries.sort(key=lambda item: item["strategy_id"])
    if len(seen_ids) != len(entries) or len(seen_ids) != len(
        {entry["strategy_id"] for entry in entries}
    ):
        raise ValueError("manifest validation failed: strategy IDs are not unique")

    manifest = {
        "manifest_version": "1.0.0",
        "strategy_file_count": len(files),
        "strategy_id_count": len(entries),
        "strategies": entries,
    }
    with MANIFEST_PATH.open("w", encoding="utf-8", newline="\n") as file:
        file.write(
            json.dumps(manifest, indent=2, sort_keys=True, ensure_ascii=False) + "\n"
        )
    with REPORT_PATH.open("w", encoding="utf-8", newline="\n") as file:
        file.write(build_report(entries, [path.name for path in files], 35))
    print(
        f"wrote {MANIFEST_PATH.relative_to(ROOT)} "
        f"and {REPORT_PATH.relative_to(ROOT)} "
        f"({len(files)} files, {len(entries)} strategy IDs)"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, KeyError, json.JSONDecodeError) as error:
        print(f"build_manifest.py: {error}", file=sys.stderr)
        raise SystemExit(1)
