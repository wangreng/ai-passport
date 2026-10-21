# Word Box word pack

This branch only holds the word list for the Word Box firmware
(`feature/ket-word-boxes`) and the generated pack the device downloads.

- Edit `packs/words.txt` (one card per line: `english | chunk | 中文`).
- Pushing it runs `.github/workflows/wordpack.yml`, which generates British
  English audio with `tools/build_wordpack.py` and commits `packs/out/`.
- The device reads `https://raw.githubusercontent.com/wangreng/ai-passport/wordpack/packs/out/pack.txt`.
