#include "native_duration.h"
#include "path_utf8.h"
#include "utf8_util.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <vector>
#if defined(_WIN32)
#include "win_compat.h"
#include <io.h>
#else
#include <unistd.h>
#endif

namespace muisc {

namespace {

// Thin wrappers so the parsers below stay identical on every platform.
//
// Two Windows-only details matter here. fs::path::value_type is wchar_t, so
// the POSIX `open(path.c_str(), ...)` would not even compile -- and routing
// through path.string() instead would mangle any non-ASCII filename, which is
// exactly what the yt-dlp cache is full of. _wopen takes the native wide path
// directly. And _O_BINARY is mandatory: without it the CRT translates CRLF
// inside what are binary audio headers, corrupting every offset.
int open_for_probe(const fs::path& path) {
#if defined(_WIN32)
    return _wopen(path.c_str(), _O_RDONLY | _O_BINARY);
#else
    return ::open(path.c_str(), O_RDONLY);
#endif
}

long long file_size_of(int fd) {
#if defined(_WIN32)
    struct _stat64 st;
    if (_fstat64(fd, &st) != 0) return -1;
    return static_cast<long long>(st.st_size);
#else
    struct stat st;
    if (fstat(fd, &st) != 0) return -1;
    return static_cast<long long>(st.st_size);
#endif
}

void close_probe(int fd) {
#if defined(_WIN32)
    _close(fd);
#else
    ::close(fd);
#endif
}

// Positional read. Windows has no pread; win_pread() emulates it with an
// OVERLAPPED offset, which -- like the real thing -- leaves the descriptor's
// own file pointer untouched.
long long pread_at(int fd, void* buf, size_t count, long long offset) {
#if defined(_WIN32)
    return static_cast<long long>(muisc::win_pread(fd, buf, count, offset));
#else
    return static_cast<long long>(::pread(fd, buf, count, static_cast<off_t>(offset)));
#endif
}

} // namespace

namespace {

uint32_t read_be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
uint64_t read_be64(const uint8_t* p) {
    return (uint64_t(read_be32(p)) << 32) | uint64_t(read_be32(p + 4));
}
uint32_t read_le32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
uint64_t read_le64(const uint8_t* p) {
    return uint64_t(read_le32(p)) | (uint64_t(read_le32(p + 4)) << 32);
}

// MP4/M4A/AAC 'mvhd' atom.
uint32_t parse_mp4_duration(int fd, long long file_size) {
    long long offset = 0;
    uint8_t hdr[8];
    while (offset + 8 <= file_size) {
        if (pread_at(fd, hdr, 8, offset) != 8) break;
        uint32_t atom_size = read_be32(hdr);
        if (atom_size == 0) break;

        if (std::memcmp(hdr + 4, "moov", 4) == 0) {
            long long sub_offset = offset + 8;
            long long moov_end = offset + atom_size;
            while (sub_offset + 8 <= moov_end) {
                uint8_t sub_hdr[8];
                if (pread_at(fd, sub_hdr, 8, sub_offset) != 8) break;
                uint32_t sub_size = read_be32(sub_hdr);
                if (sub_size == 0) break;

                if (std::memcmp(sub_hdr + 4, "mvhd", 4) == 0) {
                    uint8_t mvhd[32];
                    if (pread_at(fd, mvhd, sizeof(mvhd), sub_offset + 8) >= 24) {
                        uint8_t version = mvhd[0];
                        if (version == 0) {
                            uint32_t timescale = read_be32(mvhd + 12);
                            uint32_t duration = read_be32(mvhd + 16);
                            if (timescale > 0) return duration / timescale;
                        } else if (version == 1) {
                            uint32_t timescale = read_be32(mvhd + 20);
                            uint64_t duration = read_be64(mvhd + 24);
                            if (timescale > 0) return static_cast<uint32_t>(duration / timescale);
                        }
                    }
                    return 0;
                }
                sub_offset += sub_size;
            }
        }
        offset += atom_size;
    }
    return 0;
}

// OGG/Opus/Vorbis: last page's granule position.
uint32_t parse_ogg_duration(int fd, long long file_size) {
    if (file_size < 4096) return 0;
    long long seek_pos = (file_size > 65536) ? (file_size - 65536) : 0;
    size_t read_len = static_cast<size_t>(file_size - seek_pos);

    std::vector<uint8_t> buf(read_len);
    if (pread_at(fd, buf.data(), read_len, seek_pos) != static_cast<long long>(read_len)) return 0;

    for (long long i = static_cast<long long>(read_len) - 14; i >= 0; --i) {
        if (std::memcmp(buf.data() + i, "OggS", 4) == 0) {
            uint64_t granule = read_le64(buf.data() + i + 6);
            if (granule > 0 && granule != static_cast<uint64_t>(-1)) {
                return static_cast<uint32_t>(granule / 48000); // standard Opus 48kHz clock
            }
        }
    }
    return 0;
}

// MP3: first valid frame header, Xing/Info VBR field if present, else
// bitrate-based estimate from remaining file size.
uint32_t parse_mp3_duration(int fd, long long file_size) {
    static const int bitrate_tbl[2][3][16] = {
        {{0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448, 0},
         {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0},
         {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0}},
        {{0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256, 0},
         {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0},
         {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0}}};
    static const int freq_tbl[3][4] = {
        {44100, 48000, 32000, 0}, {22050, 24000, 16000, 0}, {11025, 12000, 8000, 0}};

    long long offset = 0;
    uint8_t id3_hdr[10];
    if (pread_at(fd, id3_hdr, 10, 0) == 10 && std::memcmp(id3_hdr, "ID3", 3) == 0) {
        uint32_t tag_size = ((id3_hdr[6] & 0x7F) << 21) | ((id3_hdr[7] & 0x7F) << 14) |
                             ((id3_hdr[8] & 0x7F) << 7) | (id3_hdr[9] & 0x7F);
        offset = 10 + tag_size;
    }

    uint8_t scan_buf[8192];
    long long read_bytes = pread_at(fd, scan_buf, sizeof(scan_buf), offset);
    if (read_bytes < 4) return 0;

    for (long long i = 0; i < read_bytes - 4; ++i) {
        if (scan_buf[i] == 0xFF && (scan_buf[i + 1] & 0xE0) == 0xE0) {
            uint8_t b1 = scan_buf[i + 1], b2 = scan_buf[i + 2], b3 = scan_buf[i + 3];
            int ver_idx = (b1 >> 3) & 0x03;
            int lay_idx = (b1 >> 1) & 0x03;
            int br_idx = (b2 >> 4) & 0x0F;
            int sr_idx = (b2 >> 2) & 0x03;
            int channel_mode = (b3 >> 6) & 0x03;
            if (ver_idx == 1 || lay_idx == 0 || br_idx == 0x0F || sr_idx == 3) continue;

            int v_slot = (ver_idx == 3) ? 0 : 1;
            int l_slot = (lay_idx == 3) ? 0 : (lay_idx == 2 ? 1 : 2);
            int freq_ver = (ver_idx == 3) ? 0 : (ver_idx == 2 ? 1 : 2);
            int bitrate_kbps = bitrate_tbl[v_slot][l_slot][br_idx];
            int sample_rate = freq_tbl[freq_ver][sr_idx];
            if (bitrate_kbps == 0 || sample_rate == 0) continue;

            int xing_offset = (ver_idx == 3) ? (channel_mode == 3 ? 17 : 32) : (channel_mode == 3 ? 9 : 17);
            if (i + 4 + xing_offset + 12 < read_bytes) {
                const uint8_t* xing_ptr = scan_buf + i + 4 + xing_offset;
                if (std::memcmp(xing_ptr, "Xing", 4) == 0 || std::memcmp(xing_ptr, "Info", 4) == 0) {
                    uint32_t flags = read_be32(xing_ptr + 4);
                    if (flags & 0x01) {
                        uint32_t total_frames = read_be32(xing_ptr + 8);
                        int samples_per_frame = (lay_idx == 3) ? 384 : ((ver_idx == 3) ? 1152 : 576);
                        return static_cast<uint32_t>((uint64_t(total_frames) * samples_per_frame) / sample_rate);
                    }
                }
            }
            long long audio_bytes = (file_size > offset) ? (file_size - offset) : 0;
            return static_cast<uint32_t>((audio_bytes * 8) / (bitrate_kbps * 1000));
        }
    }
    return 0;
}

// FLAC STREAMINFO block.
uint32_t parse_flac_duration(int fd) {
    uint8_t buf[42];
    if (pread_at(fd, buf, 42, 0) != 42 || std::memcmp(buf, "fLaC", 4) != 0 || (buf[4] & 0x7F) != 0) return 0;
    uint32_t sample_rate = (uint32_t(buf[18]) << 12) | (uint32_t(buf[19]) << 4) | (uint32_t(buf[20]) >> 4);
    uint64_t total_samples = (uint64_t(buf[20] & 0x0F) << 32) | (uint64_t(buf[21]) << 24) |
                              (uint64_t(buf[22]) << 16) | (uint64_t(buf[23]) << 8) | uint64_t(buf[24]);
    return (sample_rate > 0) ? static_cast<uint32_t>(total_samples / sample_rate) : 0;
}

} // namespace

uint32_t probe_duration_native(const fs::path& path) {
    int fd = open_for_probe(path);
    if (fd < 0) return 0;
    long long size = file_size_of(fd);
    if (size <= 0) {
        close_probe(fd);
        return 0;
    }
    struct { long long st_size; } st{size};   // keeps the parser calls below unchanged

    uint32_t duration = 0;
    std::string ext = ascii_lower_str(path_utf8(path.extension()));

    if (ext == ".m4a" || ext == ".mp4" || ext == ".aac") duration = parse_mp4_duration(fd, st.st_size);
    else if (ext == ".opus" || ext == ".ogg") duration = parse_ogg_duration(fd, st.st_size);
    else if (ext == ".mp3") duration = parse_mp3_duration(fd, st.st_size);
    else if (ext == ".flac") duration = parse_flac_duration(fd);

    close_probe(fd);
    return duration;
}

} // namespace muisc
