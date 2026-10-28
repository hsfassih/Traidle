"""Merge finished workers' wip files into the real pipeline and lint.  usage: python scripts/strategy_work/merge_batch.py stem [stem ...]"""
import glob, os, shutil, subprocess, sys

SW = os.path.join("scripts", "strategy_work")
stems = [s.replace(".json", "") for s in sys.argv[1:]]
for s in stems:
    for pref in ("sem_", "regx_"):
        src = os.path.join(SW, "wip", pref + s + ".py")
        if os.path.exists(src):
            shutil.move(src, os.path.join(SW, pref + s + ".py"))
done = open(os.path.join(SW, "done.txt")).read().split()
for s in stems:
    if s + ".json" not in done:
        done.append(s + ".json")
open(os.path.join(SW, "done.txt"), "w").write("\n".join(done) + "\n")
py = sys.executable
for cmd in ([py, os.path.join(SW, "build_constants.py")], [py, os.path.join(SW, "build_registry.py")]):
    r = subprocess.run(cmd, capture_output=True, text=True)
    print(r.stdout.strip(), r.stderr.strip()[-800:])
    if r.returncode:
        sys.exit(1)
code = "import sys; sys.path.insert(0,'scripts/strategy_work'); import normalize; normalize.run(%r)" % [s + ".json" for s in stems]
r = subprocess.run([py, "-c", code], capture_output=True, text=True)
print(r.stdout.strip(), r.stderr.strip()[-1500:])
subprocess.run([py, os.path.join(SW, "finalize.py")])
env = dict(os.environ, PATH=r"C:\msys64\ucrt64\bin;" + os.environ["PATH"])
lint = subprocess.run([os.path.join("build-lint-mingw", "strategy_lint.exe"), "strategies", "--report", "strategy-lint-report.md"],
                      capture_output=True, text=True, env=env, encoding="utf-8")
pref = tuple(d + f for d in ("ERROR ", "WARN  ") for f in done + ["custom_signals", "constants"])
for l in lint.stdout.splitlines():
    if l.startswith(pref) and "unused constant" not in l:
        print(l)
print([l for l in lint.stdout.splitlines() if l.startswith("strategies=")])
