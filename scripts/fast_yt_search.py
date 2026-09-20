#!/usr/bin/env python3
import urllib.request
import json
import sys
import re

# See fetch_lyrics.py for why this matters on Windows: json.dumps() already
# ASCII-escapes everything below, so nothing here can currently trigger the
# UnicodeEncodeError that script was fixed for -- but forcing UTF-8 up front
# keeps that true even if this file is edited later to print anything raw.
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

def parse_length_text(text):
    """"3:45" / "1:02:03" -> seconds. None if unparseable."""
    if not text:
        return None
    parts = text.strip().split(":")
    try:
        parts = [int(p) for p in parts]
    except ValueError:
        return None
    seconds = 0
    for p in parts:
        seconds = seconds * 60 + p
    return seconds


def search(query, limit=5, min_duration_sec=90):
    url = "https://www.youtube.com/youtubei/v1/search"
    headers = {
        "Content-Type": "application/json",
        "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64)"
    }
    data = {
        "context": {
            "client": {
                "clientName": "WEB",
                "clientVersion": "2.20210721.00.00"
            }
        },
        "query": query
    }
    
    req = urllib.request.Request(url, data=json.dumps(data).encode("utf-8"), headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=5) as response:
            res = json.loads(response.read().decode())
    except Exception as e:
        print("{}", file=sys.stderr)
        return

    contents = res.get("contents", {}).get("twoColumnSearchResultsRenderer", {}).get("primaryContents", {}).get("sectionListRenderer", {}).get("contents", [])
    if not contents:
        return
        
    items = contents[0].get("itemSectionRenderer", {}).get("contents", [])
    
    count = 0
    for item in items:
        video = item.get("videoRenderer")
        if not video:
            continue
        vid_id = video.get("videoId")
        title = video.get("title", {}).get("runs", [{}])[0].get("text", "")
        uploader = video.get("ownerText", {}).get("runs", [{}])[0].get("text", "")

        # yt-dlp's search used "categories *= 'Music' & duration >= 90" to
        # keep shorts/trailers/live streams out of results. This search
        # endpoint's response has no category field at all (getting one
        # would mean an extra request per video, defeating the point of
        # a fast path), so that half can't be replicated -- but duration
        # is right here as a display string, so at least the "no shorts,
        # no live streams" half is kept. A video with no lengthText at
        # all (live, premiere, upcoming) is excluded, same as before:
        # yt-dlp's filter also rejects an entry when the field it's
        # testing is simply absent.
        duration = parse_length_text(
            video.get("lengthText", {}).get("simpleText")
        )
        if duration is None or duration < min_duration_sec:
            continue

        if vid_id and title:
            # Output in yt-dlp flat-playlist json format for compatibility
            out = {
                "id": vid_id,
                "title": title,
                "uploader": uploader,
                "duration": duration,
            }
            print(json.dumps(out))
            count += 1
            if count >= limit:
                break

if __name__ == "__main__":
    query = sys.argv[1]
    limit = int(sys.argv[2]) if len(sys.argv) > 2 else 5
    search(query, limit)
