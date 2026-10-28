"""Sandbox build + lint for ONE strategy file.

usage (from repo root):  python scripts/strategy_work/sbx.py <stem> [--full]
Copies raw files, catalog sources, merged sem/regx modules and the WIP modules of <stem> into a temp dir,
rebuilds constants + registry there, normalizes <stem>.json semantically (all other files mechanically,
or from the real strategies/ dir when listed in done.txt), lints, and prints only the diagnostics of <stem>.json.
The shared repo files are never written.
"""
import glob, os, re, shutil, subprocess, sys, tempfile

REPO = os.getcwd()
SW = os.path.join(REPO, "scripts", "strategy_work")
EXE = os.path.join(REPO, "build-lint-mingw", "strategy_lint.exe")


def main():
    stem = sys.argv[1].replace(".json", "")
    box = os.path.join(tempfile.gettempdir(), "sbx_" + stem)
    def _rw(fn, path, exc):
        os.chmod(path, 0o700)
        fn(path)
    if os.path.exists(box):
        shutil.rmtree(box, onexc=_rw)
    os.makedirs(os.path.join(box, "strategies", "catalog"))
    os.makedirs(os.path.join(box, "scripts", "strategy_work"))
    shutil.copytree(os.path.join(REPO, "strategies", "raw"), os.path.join(box, "strategies", "raw"))
    shutil.copy(os.path.join(REPO, "strategies", "catalog", "signal_catalog.json"), os.path.join(box, "strategies", "catalog"))
    for p in glob.glob(os.path.join(SW, "*.py")):
        shutil.copy(p, os.path.join(box, "scripts", "strategy_work"))
    for name in ("regx_%s.py" % stem, "sem_%s.py" % stem):
        wip = os.path.join(SW, "wip", name)
        if os.path.exists(wip):
            shutil.copy(wip, os.path.join(box, "scripts", "strategy_work", name))
    done = []
    dp = os.path.join(SW, "done.txt")
    if os.path.exists(dp):
        done = [l.strip() for l in open(dp) if l.strip()]
    for f in done:
        shutil.copy(os.path.join(REPO, "strategies", f), os.path.join(box, "strategies", f))
    env = dict(os.environ, PYTHONIOENCODING="utf-8")
    run = lambda *a: subprocess.run(a, cwd=box, env=env, capture_output=True, text=True, encoding="utf-8")
    for s in ("build_constants.py", "build_registry.py"):
        r = run(sys.executable, os.path.join("scripts", "strategy_work", s))
        if r.returncode:
            print("BUILD FAILED", s, r.stderr[-1500:])
            return 1
    others = [os.path.basename(p) for p in glob.glob(os.path.join(REPO, "strategies", "raw", "*.json"))
              if os.path.basename(p) not in done and os.path.basename(p) != stem + ".json"]
    code = ("import sys; sys.path.insert(0,'scripts/strategy_work'); import normalize; "
            "normalize.run(%r, semantic=False); normalize.run([%r])" % (others, stem + ".json"))
    r = run(sys.executable, "-c", code)
    if r.returncode:
        print("NORMALIZE FAILED\n", r.stderr[-2500:])
        return 1
    lint = subprocess.run([EXE, os.path.join(box, "strategies"), "--report", os.path.join(box, "report.md")],
                          env=dict(os.environ, PATH=r"C:\msys64\ucrt64\bin;" + os.environ["PATH"]),
                          capture_output=True, text=True, encoding="utf-8")
    mine = stem + ".json"
    keep = [l for l in lint.stdout.splitlines()
            if l.startswith(("ERROR " + mine, "WARN  " + mine, "ERROR custom_signals.json", "ERROR constants.json", "WARN  constants.json"))]
    errs = [l for l in keep if l.startswith("ERROR")]
    print("\n".join(keep) if keep else "(no diagnostics for %s)" % mine)
    print("---- %d errors, %d warnings for %s" % (len(errs), len(keep) - len(errs), mine))
    rep = open(os.path.join(box, "report.md"), encoding="utf-8").read()
    tiers = [l for l in rep.splitlines() if l.startswith("|") and ("| " + mine + " |") in l]
    print("\n".join(tiers))
    print("sandbox output:", os.path.join(box, "strategies", mine))
    return 1 if errs else 0


if __name__ == "__main__":
    sys.exit(main())
