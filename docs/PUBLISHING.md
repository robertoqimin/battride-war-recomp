# Publishing the code

`PUBLIC_FILES.txt` is the explicit file allowlist for this source package. `.gitignore` excludes the game, firmware, saves, generated output, builds, and logs. Do not use `git add -f` on those directories.

## Check and package

From the project root:

```bash
python3 -m unittest discover -s tests -p test_publication.py
python3 scripts/build/export_source.py
python3 scripts/build/export_source.py --output /tmp/kamenriderbattridewarsport-source.zip \
  --manifest /tmp/kamenriderbattridewarsport-source-manifest.json
```

Output paths must not already exist. The exporter never overwrites existing files. It accepts only explicitly listed text files, rejects symlinks and private paths, and creates a ZIP without Git history or local data. This check does not replace content review when the code changes in the future.

To publish through the GitHub interface, extract this ZIP and upload the files inside its directory, rather than your entire local tree.

## Publish with Git

Create an empty repository on your GitHub account. Then, from the port root, if there is no Git repository yet:

```bash
git init --initial-branch=main
git add --pathspec-from-file=PUBLIC_FILES.txt
git diff --cached --stat
git diff --cached --name-only
```

Check that the list contains no `game/`, `firmware/`, `gamedata/`, `src/recomp/`, `spu/`, `analysis/`, or `build*/` paths. Then commit and configure your repository URL:

```bash
git commit -m "Prepare experimental source release"
git remote add origin https://github.com/YOUR_USERNAME/battride-war-recomp.git
git push -u origin main
```

Replace `YOUR_USERNAME`. If Git, remotes, or commits already exist, inspect the current state and adapt the commands; do not reinitialize or replace history. Local preparation does not commit, create a remote repository, or push automatically.

Suggested description:

> Battride War Recomp: an experimental native Linux port of Kamen Rider: Battride War (BLJS10220) built with PS3Recomp and Vulkan. Playable through the opening mission (STG02), about 55-58 fps in ordinary fighting on a Ryzen 5 4600H. The AppImage builds the game from your own disc; no game, firmware, keys, or saves included.

Do not advertise this as a complete game or stable port. The documentation describes exactly the section validated. Licenses and credits are in `LICENSE` and `THIRD_PARTY_NOTICES.md`.
