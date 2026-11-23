#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Drop SARIF results located in the paths .github/codeql/codeql-config.yml ignores.

CodeQL's paths-ignore does not apply to compiled languages: the C++
extractor analyses every file the build compiles, the vendored headers
included. codeql.yml therefore analyses without uploading, passes the SARIF
through this script, and uploads what remains, so code scanning holds
first-party code only. The ignore list is read from the config file, so it
lives in one place (and scripts/check-version-consistency.sh holds it to
lib/VENDORED.md).

    scripts/filter-sarif.py <config.yml> <in.sarif> <out.sarif>
"""
import json
import re
import sys


def ignored_paths(config_path):
    paths = []
    inside = False
    for line in open(config_path, encoding="utf-8"):
        if re.match(r"^paths-ignore:\s*$", line):
            inside = True
            continue
        if inside:
            match = re.match(r"^\s+-\s+([^\s#]+)", line)
            if match:
                paths.append(match.group(1).rstrip("/"))
            elif line.strip() and not line.lstrip().startswith("#"):
                inside = False
    return paths


def is_ignored(uri, paths):
    uri = uri.removeprefix("file://").lstrip("./")
    return any(uri == p or uri.startswith(p + "/") for p in paths)


def main():
    config, source, target = sys.argv[1:4]
    paths = ignored_paths(config)
    if not paths:
        sys.exit("filter-sarif: no paths-ignore entries in " + config)
    sarif = json.load(open(source, encoding="utf-8"))
    kept = dropped = 0
    for run in sarif.get("runs", []):
        results = []
        for result in run.get("results", []):
            locations = result.get("locations") or [{}]
            uri = locations[0].get("physicalLocation", {}).get("artifactLocation", {}).get("uri", "")
            if is_ignored(uri, paths):
                dropped += 1
            else:
                results.append(result)
                kept += 1
        run["results"] = results
    json.dump(sarif, open(target, "w", encoding="utf-8"))
    print(f"filter-sarif: kept {kept} result(s), dropped {dropped} in {', '.join(paths)}")


if __name__ == "__main__":
    main()
