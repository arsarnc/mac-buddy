"""Claude Code token usage, read from Claude Code's local logs (~/.claude/projects/**/*.jsonl).

Every API response Claude Code receives is logged with its token usage. This reads only new lines
each time (files are followed by byte offset) and keeps the last 24 hours in memory.
Usage from claude.ai or the desktop chat is not in these logs, and plan limits are not stored locally.
"""
import glob
import json
import os
import time
from datetime import datetime

LOG_GLOB = os.path.expanduser("~/.claude/projects/**/*.jsonl")
KEEP_SEC = 24 * 3600
BLOCK_SEC = 5 * 3600  # Claude plan usage resets in 5-hour windows


def short_model(name):
    """'claude-opus-5-5' -> 'Opus 5.5'"""
    if not name or not name.startswith("claude-"):
        return name or "?"
    parts = name[len("claude-"):].split("-")
    family = parts[0].capitalize()
    version = ".".join(p for p in parts[1:] if p.isdigit() and len(p) <= 2)
    return f"{family} {version}".strip()


class ClaudeUsage:
    def __init__(self):
        self.offsets = {}  # file -> bytes already read
        self.events = []   # (ts, model, inp, out, cache_read, cache_write, project)
        self.seen = set()  # (message id, request id): the same response can be logged more than once
        self.project_of = {}  # log file -> folder the session started in

    def _read_new(self):
        cutoff = time.time() - KEEP_SEC
        for path in glob.glob(LOG_GLOB, recursive=True):
            try:
                st = os.stat(path)
            except OSError:
                continue
            if st.st_mtime < cutoff:
                continue
            start = self.offsets.get(path, 0)
            if st.st_size < start:  # file was rewritten
                start = 0
            if st.st_size == start:
                continue
            with open(path, "rb") as f:
                f.seek(start)
                data = f.read()
            end = data.rfind(b"\n") + 1  # leave a half-written last line for next time
            self.offsets[path] = start + end
            for raw in data[:end].splitlines():
                if b'"usage"' not in raw:
                    continue
                try:
                    d = json.loads(raw)
                except ValueError:
                    continue
                msg = d.get("message") or {}
                u = msg.get("usage")
                ts = d.get("timestamp")
                model = msg.get("model")
                if not u or not ts or not model or model.startswith("<"):
                    continue
                key = (msg.get("id"), d.get("requestId"))
                if key in self.seen:
                    continue
                self.seen.add(key)
                try:
                    t = datetime.fromisoformat(ts.replace("Z", "+00:00")).timestamp()
                except ValueError:
                    continue
                if t < cutoff:
                    continue
                cwd = (d.get("cwd") or "").rstrip("/")
                project = self.project_of.setdefault(path, os.path.basename(cwd) or "?")
                self.events.append((t, model, u.get("input_tokens", 0), u.get("output_tokens", 0),
                                    u.get("cache_read_input_tokens", 0), u.get("cache_creation_input_tokens", 0),
                                    project))
        self.events.sort(key=lambda e: e[0])
        self.events = [e for e in self.events if e[0] >= cutoff]

    def summary(self):
        """Compact dict for the board (keys kept short to fit the serial line)."""
        self._read_new()
        now = time.time()
        midnight = datetime.now().replace(hour=0, minute=0, second=0, microsecond=0).timestamp()
        today = [e for e in self.events if e[0] >= midnight]

        def totals(evs):
            return [sum(e[i] for e in evs) for i in (2, 3, 4, 5)]  # in, out, cache read, cache write

        # Current 5-hour window: starts at the hour of the first request after a 5-hour gap
        block_start = None
        for e in self.events:
            if block_start is None or e[0] >= block_start + BLOCK_SEC:
                block_start = e[0] - e[0] % 3600
        block = [e for e in self.events if block_start and e[0] >= block_start]
        active = block_start is not None and now < block_start + BLOCK_SEC

        # Output tokens per minute, last 60 minutes (oldest first)
        per_min = [0] * 60
        for e in self.events:
            age = int((now - e[0]) // 60)
            if 0 <= age < 60:
                per_min[59 - age] += e[3]

        models, projects = {}, {}
        for e in today:
            m = models.setdefault(short_model(e[1]), [0, 0])
            m[0] += e[3]
            m[1] += 1
            projects[e[6]] = projects.get(e[6], 0) + e[2] + e[3] + e[5]
        top_models = sorted(models.items(), key=lambda kv: -kv[1][0])[:3]
        top_projects = sorted(projects.items(), key=lambda kv: -kv[1])[:4]

        bt = totals(block) if active else [0, 0, 0, 0]
        mins = max((now - block_start) / 60, 1) if active else 1
        return {
            "td": totals(today) + [len(today)],                     # today: in, out, cache read, cache write, requests
            "bk": [int(block_start or 0), int((block_start or 0) + BLOCK_SEC), *bt, len(block) if active else 0,
                   int(active)],                                     # window: start, end, in, out, cr, cw, requests, active
            "br": round(bt[1] / mins) if active else 0,             # output tokens/min in this window
            "pm": per_min,
            "md": [[k[:14], v[0], v[1]] for k, v in top_models],   # model, output tokens, requests
            "pj": [[k[:18], v] for k, v in top_projects],          # project, tokens (in + out + cache write)
        }


if __name__ == "__main__":
    t = time.time()
    s = ClaudeUsage().summary()
    print(f"{time.time() - t:.2f}s", json.dumps(s, separators=(",", ":")))
    print(len(json.dumps(s, separators=(",", ":"))), "bytes")
