#include "metadata_probe.h"
#include "process_util.h"
#include "path_utf8.h"
#include "utf8_util.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>
#include <system_error>

namespace muisc {

static std::string to_upper(std::string s) {
    return ascii_upper_str(std::move(s));
}

double probe_duration_seconds(const fs::path& file) {
    std::string cmd = "ffprobe -v error -show_entries format=duration -of csv=p=0 "
                       + shell_quote(path_utf8(file));
    ProcResult r = run_capture(cmd);
    if (r.out.empty()) return -1.0;
    try {
        return std::stod(r.out);
    } catch (...) {
        return -1.0;
    }
}

RowMeta probe_row_meta(const fs::path& file) {
    RowMeta rm;
    // Pulls title/album alongside artist now (still one ffprobe call, so
    // no extra subprocess cost) so the library-wide search can match
    // against embedded tags, not just the artist column and the
    // filename-derived title. stream_tags is also requested: ffmpeg's
    // Ogg Vorbis/Opus muxers expose title/artist/album as TAGS ON THE
    // AUDIO STREAM rather than on the container ("format") -- format_tags
    // alone comes back completely empty for those two formats even
    // though the file plainly has metadata (confirmed against real
    // ffmpeg-muxed .ogg/.opus files: format_tags is empty, stream_tags
    // has everything). Keeping format_tags too covers everything else
    // (MP3, FLAC, M4A, ...), where it's the one that's populated.
    std::string cmd = "ffprobe -v error "
                       "-show_entries format=duration:format_tags=artist,title,album,date:"
                       "stream_tags=artist,title,album,date "
                       "-of default=noprint_wrappers=1 " + shell_quote(path_utf8(file));
    ProcResult r = run_capture(cmd);
    // A real probe attempt happened either way -- mark it resolved even on
    // an empty/failed result so callers don't keep retrying an untagged or
    // unreadable file forever. Only the fields actually parsed below get
    // filled in; everything else stays at its default (empty/-1).
    rm.tags_resolved = true;
    if (r.out.empty()) return rm;

    std::istringstream stream(r.out);
    std::string line;
    while (std::getline(stream, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        if (!val.empty() && val.back() == '\r') val.pop_back();
        if (val.empty() || val == "N/A") continue;

        if (key == "duration") {
            try { rm.duration_sec = std::stod(val); } catch (...) {}
        } else {
            // Case-insensitive on purpose: ffprobe reports a tag's key
            // exactly as it's stored on disk, and containers disagree on
            // casing. In particular Matroska/WebM's muxer writes any tag
            // ffmpeg newly sets using the Matroska spec's canonical
            // UPPERCASE names (ARTIST, TITLE, DATE, ...), while an
            // untouched pre-existing tag on the same file keeps whatever
            // casing it already had (often lowercase). A file that's had
            // exactly one field edited can genuinely have "TAG:title" and
            // "TAG:ARTIST" side by side -- an exact-case match here would
            // read the just-edited field back as blank even though the
            // save itself succeeded.
            std::string upper_key = to_upper(key);
            if (upper_key == "TAG:ARTIST") {
                // First-wins: with stream_tags also requested, a file with
                // several streams (e.g. an attached-picture "video" stream
                // alongside the audio) can print more than one TAG:artist
                // line. format_tags is listed first and is the authoritative
                // one when present; don't let a later, possibly-blank or
                // irrelevant stream's tags clobber it.
                if (rm.artist.empty()) rm.artist = val;
            } else if (upper_key == "TAG:TITLE") {
                if (rm.title.empty()) rm.title = val;
            } else if (upper_key == "TAG:ALBUM") {
                if (rm.album.empty()) rm.album = val;
            } else if (upper_key == "TAG:DATE") {
                // Containers store the year very differently (ID3's TYER="1999",
                // ID3v2.4's TDRC="1999-05-01", Vorbis' DATE, MP4's ©day) --
                // ffprobe just reports whatever it found, so keep only the
                // leading 4-digit year. Same rule probe_metadata() applies to
                // its own md.year, so the editor and the metadata panel agree.
                if (rm.year.empty()) rm.year = (val.size() >= 4) ? val.substr(0, 4) : val;
            }
        }
    }
    return rm;
}

TrackMetadata probe_metadata(const fs::path& file, const std::string& fallback_name,
                              const std::string& fallback_artist, const std::string& location_label) {
    TrackMetadata md;
    md.name = fallback_name;
    md.artist = fallback_artist.empty() ? "-" : fallback_artist;
    md.location = location_label;

    std::error_code ec;
    auto bytes = fs::file_size(file, ec);
    if (!ec) {
        double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
        std::ostringstream oss;
        oss.precision(2);
        oss << std::fixed << mb << "MB";
        md.file_size = oss.str();
    }

    std::string cmd = "ffprobe -v error "
                       "-show_entries format=duration:format_tags=artist,date,title:stream=sample_rate,codec_name "
                       "-of default=noprint_wrappers=1 " + shell_quote(path_utf8(file));
    ProcResult r = run_capture(cmd);
    if (!r.ok() && r.out.empty()) return md;

    std::istringstream stream(r.out);
    std::string line;
    while (std::getline(stream, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        if (!val.empty() && val.back() == '\r') val.pop_back();
        if (val.empty() || val == "N/A") continue;

        if (key == "sample_rate") {
            md.sampling = val + "KHz"; // matches the mockup's (unconventional) unit label
        } else if (key == "codec_name") {
            md.format = to_upper(val);
        } else {
            // Case-insensitive for the same reason as probe_row_meta() --
            // see its comment. A freshly-edited tag on a Matroska/WebM file
            // can come back as "TAG:ARTIST" even while an untouched sibling
            // tag on the same file is still "TAG:title".
            std::string upper_key = to_upper(key);
            if (upper_key == "TAG:TITLE") {
                md.name = val;
            } else if (upper_key == "TAG:ARTIST") {
                md.artist = val;
            } else if (upper_key == "TAG:DATE") {
                md.year = val.substr(0, 4);
            }
        }
    }
    return md;
}

} // namespace muisc
