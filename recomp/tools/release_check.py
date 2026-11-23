"""Refuse to commit or push anything that is not our own plain source (see PUBLISHING.md).

The public repo must never hold game data: the disc, the game's EXE, files the build makes (build/extract,
build/gen = the game's code translated to C, Rugrats.exe), save states, memory cards, screenshots, audio or video.

usage:
  python -I recomp/tools/release_check.py            check the files staged for commit (pre-commit hook)
  python -I recomp/tools/release_check.py --tree REV check every file in a commit
  python -I recomp/tools/release_check.py --push     read pre-push hook lines from stdin, check every commit pushed

Personal strings (names, e-mail, local paths) to refuse can be listed one per line in
`release_private_patterns.txt` next to the repo root; that file is never committed.

E-mail addresses: git stores the author's and committer's address in every commit (and the tagger's in an annotated
tag), and they become public on push. Only GitHub noreply addresses (`...@users.noreply.github.com`) are accepted
there; commit and tag messages may only contain noreply addresses (e.g. a Co-Authored-By trailer). The commit
identity is checked before each commit (git var), and every pushed commit and tag again.
Exit code 0 = clean, 1 = refused (with the reasons printed).
"""
import os
import re
import subprocess
import sys

ROOT = subprocess.run(["git", "rev-parse", "--show-toplevel"], capture_output=True, text=True).stdout.strip()

# Only these files may exist in the repo. Everything else is refused.
ROOT_FILES = {".gitignore", ".gitattributes", "README.md", "PUBLISHING.md", "LICENSE", "Launcher.bat"}
SOURCE_EXT = {".c", ".h", ".py", ".lua", ".md", ".txt"}
MOD_INI = re.compile(r"recomp/mods/[A-Za-z0-9_.\-]{1,63}/mod\.ini")  # example mods (our own text, MODDING.md)
MAX_BYTES = 256 * 1024          # most source files are well under 100 KB
# Hand-written files allowed to be bigger (the content checks below still apply to them).
SIZE_OK = {"recomp/runtime/hooks.c": 512 * 1024}   # all game hooks in one file, ~270 KB on 2026-11-22

# Things that only appear in game data or in files the build generates.
GEN_LABEL = re.compile(rb"^L_[0-9a-f]{8}:", re.M)            # labels in build/gen/*.c
GEN_FUNC = re.compile(rb"^void f_[0-9a-f]{8}\(void\) \{", re.M)  # functions in build/gen/*.c
NUM_ROW = re.compile(rb"^\s*(-?(0x)?[0-9a-fA-F]+[uU]?,\s*){8,}")  # a row of a numeric table
BLOB = re.compile(rb"[A-Za-z0-9+/=]{200,}")                  # base64 / hex dumps
# Hardware constant tables we wrote from the psx-spx documentation (allowed by name, with a row limit).
TABLE_OK = {"recomp/runtime/spu.c": 70, "recomp/runtime/mdec.c": 12}
NOREPLY = re.compile(r"[^@\s<>]+@users\.noreply\.github\.com", re.I)   # the only identity e-mail allowed
EMAIL = re.compile(rb"[A-Za-z0-9._%+\-]+@[A-Za-z0-9.\-]+\.[A-Za-z]{2,}")
GENERIC_PRIVATE = [rb"[A-Z]:[\\/]+Users[\\/]+[A-Za-z]", rb"/home/[a-z]", rb"AppData[\\/]+Local[\\/]+Temp"]


def git(*a, inp=None):
    r = subprocess.run(["git", *a], cwd=ROOT, capture_output=True, input=inp)
    if r.returncode:
        sys.exit("git " + " ".join(a) + " failed: " + r.stderr.decode(errors="replace"))
    return r.stdout


def private_patterns():
    pats = list(GENERIC_PRIVATE)
    p = os.path.join(ROOT, "release_private_patterns.txt")
    if os.path.exists(p):
        for line in open(p, encoding="utf-8"):
            line = line.strip()
            if line and not line.startswith("#"):
                pats.append(re.escape(line.encode()))
    return [re.compile(x, re.I) for x in pats]


def check_file(path, data, priv):
    bad = []
    top = "/" not in path
    if top and path not in ROOT_FILES:
        bad.append("not an allowed top-level file")
    if not top and not path.startswith("recomp/"):
        bad.append("outside recomp/")
    ext = os.path.splitext(path)[1].lower()
    if not top and ext not in SOURCE_EXT and not MOD_INI.fullmatch(path):
        bad.append(f"file type {ext or '(none)'} is not plain source")
    if len(data) > SIZE_OK.get(path, MAX_BYTES):
        bad.append(f"too big ({len(data)} bytes)")
    try:
        text_ok = b"\0" not in data and data.decode("utf-8") is not None
    except UnicodeDecodeError:
        text_ok = False
    if not text_ok:
        bad.append("binary data (not plain UTF-8 text)")
    if path != "recomp/tools/release_check.py":
        if len(GEN_LABEL.findall(data)) > 20 or len(GEN_FUNC.findall(data)) > 5:
            bad.append("looks like generated game code (build/gen)")
    rows = sum(1 for line in data.splitlines() if NUM_ROW.match(line))
    if rows > TABLE_OK.get(path, 8):
        bad.append(f"{rows} rows of numeric table data")
    if BLOB.search(data):
        bad.append("long encoded blob")
    for p in priv:
        m = p.search(data)
        if m:
            bad.append("personal string / local path: " + m.group(0).decode(errors="replace"))
    return bad


def check_entries(entries, label):
    """entries: list of (path, blob id)."""
    priv = private_patterns()
    errors, seen = [], set()
    for path, blob in entries:
        if (path, blob) in seen:
            continue
        seen.add((path, blob))
        data = git("cat-file", "blob", blob)
        for b in check_file(path, data, priv):
            errors.append(f"  {path}: {b}")
    if errors:
        print(f"RELEASE CHECK FAILED ({label}): nothing may be committed or pushed until these are fixed.")
        print("\n".join(errors))
        print("See PUBLISHING.md. Game data must never reach the public repo.")
        return 1
    print(f"release check ok ({label}: {len(seen)} files)")
    return 0


def email_ok(addr):
    return bool(NOREPLY.fullmatch(addr.strip()))


def message_bad(text, priv):
    """reasons a commit / tag message may not be published"""
    bad = []
    for m in EMAIL.findall(text):
        if b"noreply" not in m.lower():
            bad.append("e-mail address in the message: " + m.decode(errors="replace"))
    for p in priv:
        m = p.search(text)
        if m:
            bad.append("personal string / local path in the message: " + m.group(0).decode(errors="replace"))
    return bad


def check_identity():
    """before a commit: the author / committer address git is about to use"""
    bad = []
    for var in ("GIT_AUTHOR_IDENT", "GIT_COMMITTER_IDENT"):
        ident = git("var", var).decode(errors="replace")
        m = re.search(r"<([^>]*)>", ident)
        addr = m.group(1) if m else ""
        if not email_ok(addr):
            bad.append(f"  {var}: {addr or '(none)'} is not a GitHub noreply address")
    if bad:
        print("RELEASE CHECK FAILED (commit identity): your e-mail would be public in the commit.")
        print("\n".join(bad))
        print("Fix: git config --local user.email <id>+<user>@users.noreply.github.com (GitHub: Settings > Emails)")
        return 1
    print("release check ok (commit identity)")
    return 0


def check_meta(rev, label):
    """a commit's (or annotated tag's) addresses and message"""
    priv = private_patterns()
    bad = []
    if git("cat-file", "-t", rev).strip() == b"tag":
        head, _, msg = git("cat-file", "tag", rev).partition(b"\n\n")
        for line in head.split(b"\n"):
            if line.startswith(b"tagger "):
                m = re.search(rb"<([^>]*)>", line)
                addr = m.group(1).decode(errors="replace") if m else ""
                if not email_ok(addr):
                    bad.append(f"  tagger address {addr or '(none)'} is not a GitHub noreply address")
    else:
        ae, ce, msg = git("log", "-1", "--format=%ae%x00%ce%x00%B", rev).split(b"\0", 2)
        for who, addr in (("author", ae), ("committer", ce)):
            a = addr.decode(errors="replace")
            if not email_ok(a):
                bad.append(f"  {who} address {a or '(none)'} is not a GitHub noreply address")
    bad += ["  " + b for b in message_bad(msg, priv)]
    if bad:
        print(f"RELEASE CHECK FAILED ({label}): an e-mail address or personal string would be public.")
        print("\n".join(bad))
        print("Fix it before pushing (git commit --amend --reset-author, or rewrite history). See PUBLISHING.md.")
        return 1
    print(f"release check ok ({label}: identity and message)")
    return 0


def tree_entries(rev):
    out = git("ls-tree", "-r", "-z", rev).split(b"\0")
    ents = []
    for e in out:
        if not e:
            continue
        meta, path = e.split(b"\t", 1)
        mode, typ, blob = meta.split()
        if typ == b"blob":
            ents.append((path.decode(), blob.decode()))
    return ents


def staged_entries():
    out = git("ls-files", "-s", "-z").split(b"\0")
    ents = []
    for e in out:
        if not e:
            continue
        meta, path = e.split(b"\t", 1)
        mode, blob, stage = meta.split()
        ents.append((path.decode(), blob.decode()))
    return ents


def main():
    a = sys.argv[1:]
    if a[:1] == ["--tree"]:
        return check_entries(tree_entries(a[1]), "commit " + a[1]) | check_meta(a[1], "commit " + a[1])
    if a[:1] == ["--push"]:
        zero = "0" * 40
        rc = 0
        for line in sys.stdin.read().splitlines():
            parts = line.split()
            if len(parts) != 4 or parts[1] == zero:
                continue
            local, remote = parts[1], parts[3]
            if git("cat-file", "-t", local).strip() == b"tag":   # annotated tag: its tagger and message too
                rc |= check_meta(local, "pushed tag " + parts[0])
            rng = [local] if remote == zero else [f"{remote}..{local}"]
            # every commit in the push, not only the last one: a file added then deleted stays in history
            for c in git("rev-list", *rng).decode().split():
                rc |= check_entries(tree_entries(c), "pushed commit " + c[:10])
                rc |= check_meta(c, "pushed commit " + c[:10])
        return rc
    return check_entries(staged_entries(), "staged files") | check_identity()


if __name__ == "__main__":
    sys.exit(main())
