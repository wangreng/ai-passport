#!/usr/bin/env python3
"""Build the Word Box word pack from packs/words.txt.

Output (packs/out/):
  pack.txt          manifest the device downloads first
  a/<slug>.w.adp    pronunciation of the word
  a/<slug>.c.adp    pronunciation of the chunk (or the word again if no chunk)

words.txt: one card per line, fields separated by | (or a full-width ｜ or a tab):
  borrow | borrow a book | 借
  heavy | 重的
  jacket
Blank lines and lines starting with # are ignored.

Audio: British English TTS (edge-tts en-GB, falling back to gTTS co.uk),
16 kHz mono, IMA ADPCM. File layout: b"WA", version 1, reserved 0,
uint32 LE sample count, then 4-bit codes, low nibble first, predictor and
step index both starting at 0.
"""
import asyncio
import hashlib
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

RATE = 16000
MAX_WORD_SAMPLES = RATE * 2          # 2 s
MAX_CHUNK_SAMPLES = RATE * 3 // 1    # 3 s; word + chunk must fit one 23 KB device slot
VOICE = "en-GB-SoniaNeural"

STEP_TABLE = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767,
]
INDEX_TABLE = [-1, -1, -1, -1, 2, 4, 6, 8]


def adpcm_encode(samples: list[int]) -> bytes:
    pred, index = 0, 0
    codes = []
    for s in samples:
        step = STEP_TABLE[index]
        diff = s - pred
        code = 0
        if diff < 0:
            code = 8
            diff = -diff
        delta = step >> 3
        if diff >= step:
            code |= 4
            diff -= step
            delta += step
        if diff >= step >> 1:
            code |= 2
            diff -= step >> 1
            delta += step >> 1
        if diff >= step >> 2:
            code |= 1
            delta += step >> 2
        pred = pred - delta if code & 8 else pred + delta
        pred = max(-32768, min(32767, pred))
        index = max(0, min(88, index + INDEX_TABLE[code & 7]))
        codes.append(code)
    if len(codes) % 2:
        codes.append(0)
    body = bytes(codes[i] | (codes[i + 1] << 4) for i in range(0, len(codes), 2))
    return b"WA" + bytes([1, 0]) + struct.pack("<I", len(samples)) + body


SEP = re.compile(r"\s*(?:\||｜|\t)\s*")


def parse_words(text: str) -> list[tuple[str, str, str]]:
    cards, seen = [], set()
    for raw in text.splitlines():
        line = raw.strip().strip("　")
        if not line or line.startswith("#"):
            continue
        fields = SEP.split(line)[:3]
        en = fields[0].strip()
        if not en or en.lower() in seen:
            continue
        seen.add(en.lower())
        if len(fields) == 1:
            cards.append((en, "", ""))
        elif len(fields) == 2:
            cards.append((en, "", fields[1].strip()))
        else:
            cards.append((en, fields[1].strip(), fields[2].strip()))
    return cards


def slug_for(en: str) -> str:
    base = re.sub(r"[^a-z0-9]+", "-", en.lower()).strip("-")[:12] or "w"
    return f"{base}-{hashlib.sha1(en.lower().encode()).hexdigest()[:6]}"


async def _edge(text: str, out: Path) -> None:
    import edge_tts  # noqa: PLC0415
    await edge_tts.Communicate(text, VOICE, rate="-10%").save(str(out))


def tts_mp3(text: str, out: Path) -> None:
    for attempt in range(3):
        try:
            asyncio.run(_edge(text, out))
            if out.stat().st_size > 0:
                return
        except Exception as exc:  # noqa: BLE001
            print(f"  edge-tts failed ({exc}); retry {attempt + 1}", file=sys.stderr)
            time.sleep(2)
    from gtts import gTTS  # noqa: PLC0415
    gTTS(text=text, lang="en", tld="co.uk", slow=False).save(str(out))


def to_pcm(mp3: Path, max_samples: int) -> list[int]:
    raw = subprocess.run(
        ["ffmpeg", "-v", "error", "-i", str(mp3),
         "-af", "silenceremove=start_periods=1:start_threshold=-45dB:"
                "stop_periods=-1:stop_threshold=-45dB:stop_duration=0.25,"
                "volume=1.6,alimiter=limit=0.95",
         "-ar", str(RATE), "-ac", "1", "-f", "s16le", "-"],
        check=True, capture_output=True,
    ).stdout
    samples = list(struct.unpack(f"<{len(raw) // 2}h", raw[: len(raw) // 2 * 2]))
    if len(samples) > max_samples:
        samples = samples[:max_samples]
        fade = min(800, len(samples))
        for i in range(fade):
            samples[-1 - i] = samples[-1 - i] * i // fade
    return samples


def clip(text: str, path: Path, max_samples: int, tmp: Path) -> None:
    mp3 = tmp / "t.mp3"
    tts_mp3(text, mp3)
    path.write_bytes(adpcm_encode(to_pcm(mp3, max_samples)))


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: build_wordpack.py packs/words.txt packs/out", file=sys.stderr)
        return 2
    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    cards = parse_words(src.read_text(encoding="utf-8"))
    audio_dir = out / "a"
    old = {p.name for p in audio_dir.glob("*.adp")} if audio_dir.exists() else set()
    audio_dir.mkdir(parents=True, exist_ok=True)

    lines = []
    keep = set()
    with tempfile.TemporaryDirectory() as tmpdir:
        tmp = Path(tmpdir)
        for en, chunk, zh in cards:
            slug = slug_for(en)
            w, c = audio_dir / f"{slug}.w.adp", audio_dir / f"{slug}.c.adp"
            keep.update({w.name, c.name})
            # A chunk change needs new chunk audio: key the chunk file on its text too.
            stamp = audio_dir / f"{slug}.c.txt"
            if not w.exists():
                print(f"tts word  {en}")
                clip(en, w, MAX_WORD_SAMPLES, tmp)
            if not c.exists() or not stamp.exists() or stamp.read_text() != (chunk or en):
                print(f"tts chunk {chunk or en}")
                clip(chunk or en, c, MAX_CHUNK_SAMPLES, tmp)
                stamp.write_text(chunk or en)
            keep.add(stamp.name)
            lines.append(f"{en}|{chunk}|{zh}|{slug}")

    for name in old - keep:
        (audio_dir / name).unlink(missing_ok=True)
    for p in audio_dir.glob("*.c.txt"):
        if p.name not in keep:
            p.unlink()

    digest = hashlib.sha1("\n".join(lines).encode()).hexdigest()[:12]
    manifest = "#wordpack 1\nversion=" + digest + "\n" + "\n".join(lines) + "\n"
    (out / "pack.txt").write_text(manifest, encoding="utf-8")
    print(f"pack {digest}: {len(lines)} cards")
    if not shutil.which("ffmpeg"):
        print("warning: ffmpeg missing", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
