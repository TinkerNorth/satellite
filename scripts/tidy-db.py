#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
#
# scripts/check-tidy.sh's compile database, made clang's: reads the build's
# compile_commands.json (argv[1]), writes the first-party entries to argv[2]
# with what only gcc understands rewritten (see check-tidy.sh), and prints the
# first-party translation units, one per line.
import json, os, re, sys
root = os.getcwd()
seen = set()
kept = []
for entry in json.load(open(sys.argv[1])):
    path = os.path.normpath(os.path.join(entry["directory"], entry["file"]))
    rel = os.path.relpath(path, root)
    if rel.split(os.sep)[0] not in ("src", "tests", "tools"):
        continue
    if "arguments" in entry:
        args = ["-flto" if re.fullmatch(r"-flto=.*", a) else a for a in entry["arguments"]]
        compiler, rest = args[0], args[1:]
    else:
        # The command line stays text: MinGW Makefiles' response-file paths
        # carry backslashes a shell-style split would eat.
        command = re.sub(r"(?<=\s)-flto=\S+", "-flto", entry["command"])
        first = re.match(r'\s*("[^"]*"|\S+)', command)
        compiler, rest = first.group(1), command[first.end():]
    unquoted = compiler.strip('"').replace("\\", "/")
    extra_args = []
    if "mingw" in unquoted.lower():
        sysroot = os.path.normpath(os.path.join(os.path.dirname(unquoted), ".."))
        extra_args = ["--target=x86_64-w64-mingw32", "--sysroot=" + sysroot.replace("\\", "/")]
    out = {"directory": entry["directory"], "file": entry["file"]}
    if isinstance(rest, list):
        out["arguments"] = [compiler] + extra_args + rest
    else:
        out["command"] = " ".join([compiler] + extra_args) + rest
    kept.append(out)
    if rel not in seen:
        seen.add(rel)
        print(rel.replace(os.sep, "/"))
json.dump(kept, open(sys.argv[2], "w"), indent=1)
