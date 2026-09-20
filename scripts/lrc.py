import sys
import requests
import xml.etree.ElementTree as ET


BETTER_LYRICS_API = "https://lyrics-api.boidu.dev/getLyrics"
LRCLIB_API = "https://lrclib.net/api/get"


def parse_time(value):
    """
    Convert TTML time to milliseconds.

    Supports:
        00:01:23.456
        1:23.456
        23.456
    """

    parts = value.split(":")

    if len(parts) == 3:
        hours = int(parts[0])
        minutes = int(parts[1])
        seconds = float(parts[2])

        total = (
            hours * 3600
            + minutes * 60
            + seconds
        )

    elif len(parts) == 2:
        minutes = int(parts[0])
        seconds = float(parts[1])

        total = (
            minutes * 60
            + seconds
        )

    else:
        total = float(parts[0])

    return round(total * 1000)


def lrc_timestamp(ms):
    """
    [00:12.345]
    """

    minutes = ms // 60000
    seconds = (ms % 60000) // 1000
    milliseconds = ms % 1000

    return (
        f"[{minutes:02d}:"
        f"{seconds:02d}."
        f"{milliseconds:03d}]"
    )


def enhanced_timestamp(ms):
    """
    <00:12.345>
    """

    minutes = ms // 60000
    seconds = (ms % 60000) // 1000
    milliseconds = ms % 1000

    return (
        f"<{minutes:02d}:"
        f"{seconds:02d}."
        f"{milliseconds:03d}>"
    )


def ttml_to_enhanced_lrc(ttml):
    """
    Convert Better Lyrics TTML to Enhanced LRC.

    Example:

    [00:12.345]<00:12.345>Hello <00:12.800>world
    """

    root = ET.fromstring(ttml)

    ns = {
        "tt": "http://www.w3.org/ns/ttml"
    }

    lines = []

    for p in root.findall(".//tt:p", ns):

        line_begin = p.get("begin")

        if not line_begin:
            continue

        words = []

        for span in p.iter():

            # We only want actual span elements.
            if not span.tag.endswith("span"):
                continue

            word_begin = span.get("begin")

            if not word_begin:
                continue

            text = "".join(span.itertext())

            if not text:
                continue

            timestamp = enhanced_timestamp(
                parse_time(word_begin)
            )

            words.append(
                timestamp + text
            )

        if not words:
            continue

        line_timestamp = lrc_timestamp(
            parse_time(line_begin)
        )

        lines.append(
            line_timestamp +
            "".join(words).strip()
        )

    return "\n".join(lines)


def ttml_to_lrc(ttml):
    """
    Convert Better Lyrics TTML to normal line-level LRC.
    """

    root = ET.fromstring(ttml)

    ns = {
        "tt": "http://www.w3.org/ns/ttml"
    }

    lines = []

    for p in root.findall(".//tt:p", ns):

        line_begin = p.get("begin")

        if not line_begin:
            continue

        words = []

        for span in p.iter():

            if not span.tag.endswith("span"):
                continue

            if not span.get("begin"):
                continue

            text = "".join(span.itertext())

            if text:
                words.append(text)

        text = "".join(words).strip()

        if not text:
            continue

        timestamp = lrc_timestamp(
            parse_time(line_begin)
        )

        lines.append(
            timestamp + text
        )

    return "\n".join(lines)


def get_better_lyrics(song, artist):
    """
    Get raw TTML from Better Lyrics.
    """

    response = requests.get(
        BETTER_LYRICS_API,
        params={
            "s": song,
            "a": artist,
        },
        timeout=10,
    )

    response.raise_for_status()

    data = response.json()

    if "ttml" not in data:
        raise RuntimeError(
            f"Better Lyrics returned no TTML: {data}"
        )

    ttml = data["ttml"]

    return {
        "source": "better-lyrics",
        "ttml": ttml,
        "lrc": ttml_to_lrc(ttml),
        "enhanced_lrc": ttml_to_enhanced_lrc(ttml),
    }


def get_lrclib_lyrics(song, artist):
    """
    LRCLIB fallback.

    No API key required.
    """

    response = requests.get(
        LRCLIB_API,
        params={
            "track_name": song,
            "artist_name": artist,
        },
        timeout=10,
    )

    response.raise_for_status()

    data = response.json()

    if not data:
        raise RuntimeError(
            "LRCLIB returned no result"
        )

    synced = data.get("syncedLyrics")

    if not synced:
        raise RuntimeError(
            "LRCLIB has no synchronized lyrics"
        )

    return {
        "source": "lrclib",
        "ttml": None,
        "lrc": synced,
        "enhanced_lrc": None,
    }


def get_lyrics(song, artist):
    """
    Lyrics resolution order:

        1. Better Lyrics TTML
        2. LRCLIB synced LRC
    """
    # These trace lines used to go to stdout, which is also the channel
    # fetch_lyrics.py's caller (mousiki's C++ side) reads its single-line
    # JSON result from. It happened to still work, because that side
    # extracts fields with a regex search rather than requiring stdout to
    # contain *only* the JSON object -- but that's incidental, not a
    # contract, and any of these lines coincidentally containing a
    # substring like `"ok":` would have broken it. stderr is where
    # diagnostic noise belongs; it's discarded by the caller either way.

    try:
        print("Trying Better Lyrics...", file=sys.stderr)

        return get_better_lyrics(
            song,
            artist
        )

    except Exception as e:

        print(
            f"Better Lyrics failed: {e}", file=sys.stderr
        )

    try:
        print("Trying LRCLIB...", file=sys.stderr)

        return get_lrclib_lyrics(
            song,
            artist
        )

    except Exception as e:

        print(
            f"LRCLIB failed: {e}", file=sys.stderr
        )

    raise RuntimeError(
        "No synchronized lyrics found."
    )


if __name__ == "__main__":

    song = "Shape of You"
    artist = "Ed Sheeran"

    try:

        result = get_lyrics(
            song,
            artist
        )

        print()
        print("SOURCE:", result["source"])

        print()
        print("=== LRC ===")
        print(result["lrc"])

        if result["enhanced_lrc"]:

            print()
            print("=== ENHANCED LRC ===")
            print(result["enhanced_lrc"])

    except Exception as e:

        print()
        print("Request failed:")
        print(e)
