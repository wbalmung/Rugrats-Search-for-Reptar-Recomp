# Publishing rules (read before every commit, push or release)

The public repository must contain **only our own source code and docs**. One game file is enough for a DMCA
takedown, and git keeps deleted files in history, so a mistake can't simply be deleted afterwards (the history
would have to be rewritten and the repo may already be copied). These rules are permanent.

## Never commit, push, upload or attach to a release

- the disc image (`.bin`, `.cue`, any other image format) or any file from it
- `build/` — all of it: `extract/` (the game's EXE and file list), `gen/` (the game's code translated to C),
  `obj/`, `Rugrats.exe`, `rugrats.ini`, saves, logs, screenshots, audio clips
- **any built `Rugrats.exe`**, including the test build: it contains the game's code. Releases ship source and the
  setup bundle (see "Before a release"), which builds the game on the user's PC from the user's own disc.
- save states (`.state`) and memory cards (`.mcd`): they hold the game's RAM (code and data)
- screenshots, VRAM dumps, recordings, audio/video clips of the game
- `backup/`, `bench/`, `saves_kept/`, `Rugrats - Test Build/`, `HANDOFF.md` (private dev notes with local paths)
- anything with personal info (names, e-mail, local paths)
- code copied from other projects without checking its licence (PCSX-Redux is GPL: use as a reference only)

## What may be public

`README.md`, `PUBLISHING.md`, `LICENSE`, `Launcher.bat` (starts `recomp/launcher.py`), `.gitignore`,
`.gitattributes`, and `recomp/` source files (`.c .h .py
.lua .md .txt`), plus `recomp/mods/<id>/mod.ini` (example mods we wrote: names, addresses and values only;
never a file from the game, never a mod someone else made without their licence). Example mods may also carry
`.txt` files we wrote ourselves (e.g. `custom_hint/hint01.txt`); never a file extracted from the disc, an edited
copy of one, or a `.bps` patch (patches carry the changed game bytes; they belong to mod authors, not this repo). Hardware constants from the psx-spx docs are fine; tables copied out of the game are not.

## Safety nets (all three must stay in place)

1. `.gitignore` is an **allow-list**: everything is ignored except the files above.
2. `recomp/tools/release_check.py` refuses: files outside the allow-list, non-source file types, binary data,
   generated game code (`build/gen` style), large numeric tables, encoded blobs, files over 256 KB, local paths and
   the personal strings in `release_private_patterns.txt` (repo root, local only, never committed). It also refuses
   any e-mail address that would become public: commit author / committer and tag tagger must be a GitHub noreply
   address (`<id>+<user>@users.noreply.github.com`, set per repo with `git config --local user.email ...`; GitHub:
   Settings > Emails), and commit / tag messages may contain no other address (noreply trailers are fine).
3. Git hooks run it: `.git/hooks/pre-commit` (staged files + the identity git is about to commit with) and
   `.git/hooks/pre-push` (every commit and annotated tag being pushed, files, addresses and message, not only the
   last). After a fresh clone, set the noreply `user.email` for the clone too. Hooks live in `.git/` and are not part of the repo: after a fresh clone, put them back:
   ```
   printf '#!/bin/sh\nexec python -I "$(git rev-parse --show-toplevel)/recomp/tools/release_check.py"\n' > .git/hooks/pre-commit
   printf '#!/bin/sh\nexec python -I "$(git rev-parse --show-toplevel)/recomp/tools/release_check.py" --push\n' > .git/hooks/pre-push
   ```
   Never use `git commit --no-verify` or `git push --no-verify`. Never `git add -f`.

Manual check of everything committed: `python -I recomp/tools/release_check.py --tree HEAD` (files, addresses and
message of that commit).

## Before a release

- Run the manual check; `git status` must show nothing unexpected.
- Release assets: source only, plus the **setup bundle** `dist/RugratsRecomp.zip` made by
  `python -I recomp/tools/make_bundle.py` and by nothing else. It holds `RugratsLauncher.exe` (our own start stub,
  compiled by the script from `recomp/tools/launcher_stub.c`, icon drawn in code: the only exe of ours that may ship),
  our source (the same files git publishes, each
  passed through the release check), an unchanged copy of Python and of the llvm-mingw compiler with their licence
  texts, and no game data: the user's own disc is still needed. Never build the bundle by hand, never add files to
  it, and never commit `dist/`. Never attach `Rugrats.exe`, game files, saves or screenshots of the game.
- Screenshots or videos for the README/release page: decide case by case (a few gameplay screenshots are common
  for fan projects, but keep them out of the repo itself; link or attach them on the release page only if wanted).
- If something slipped into a commit: do NOT push. If it was already pushed: make the repo private at once, then
  rewrite history (`git filter-repo`) and force-push, and treat the file as leaked.
