#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Turn the roadmap issues (label `roadmap`) into deploy/site/roadmap.json.

The roadmap page reads the issues from GitHub live; this file is its fallback when the
GitHub API is unreachable or rate-limited, refreshed by scripts/deploy-site.sh.
Usage: roadmap-snapshot.py <issues.json from the GitHub API> <roadmap.json to update>
"""
import datetime, json, sys

LANES = ("planned", "next", "doing", "testing")

def card(issue, authors):
    if issue.get("pull_request") or issue["user"]["login"] not in authors:
        return None
    names = [l["name"] for l in issue["labels"]]
    if issue["state"] == "closed":
        if issue.get("state_reason") == "not_planned":
            return None
        column = "done"
    else:
        lanes = [n[len("status: "):] for n in names if n.startswith("status: ")]
        column = next((l for l in LANES if l in lanes), "planned")
    body = (issue.get("body") or "").split("\n---\n")[0].strip()
    c = {"id": "gh-%d" % issue["number"], "number": issue["number"], "url": issue["html_url"],
         "column": column, "title": issue["title"], "text": body,
         "tags": [n[len("area: "):] for n in names if n.startswith("area: ")],
         "boards": [n[len("board: "):] for n in names if n.startswith("board: ")]}
    ms = (issue.get("milestone") or {}).get("title")
    if ms:
        c["shipped" if column == "done" or column == "testing" else "target"] = ms
    return c

issues = json.load(open(sys.argv[1]))
rm = json.load(open(sys.argv[2]))
authors = set(rm.get("authors", []))
cards = [c for c in (card(i, authors) for i in issues) if c]
if not cards:
    sys.exit("no roadmap issues found; keeping %s as it is" % sys.argv[2])
rm["cards"] = sorted(cards, key=lambda c: c["number"])
rm["updated"] = datetime.date.today().isoformat()
json.dump(rm, open(sys.argv[2], "w"), indent=2, ensure_ascii=False)
open(sys.argv[2], "a").write("\n")
print("roadmap.json: %d cards from GitHub" % len(cards))
