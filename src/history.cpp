#include "history.h"

#include "path_utf8.h"
#include "tiny_json.h"
#if defined(_WIN32)
#include "win_compat.h" // win_localtime -- MSVC's localtime_s has swapped arguments
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <map>
#include <sstream>

namespace muisc {

using namespace tinyjson;

namespace {

// Two limits that keep this file honest over months of use: how many plays
// are kept at all (the History tab only shows 100, but the aggregates need
// history behind them), and how long a silence has to be before it starts a
// new session.
constexpr size_t kMaxPlays = 2000;
constexpr long long kSessionGapSec = 30 * 60;

fs::path history_path() {
    const char* home = std::getenv("HOME");
    // HOME is UTF-8 on every platform this app runs on (win_bootstrap_env()
    // builds it from GetEnvironmentVariableW) -- path_from_utf8() is what
    // keeps a non-ASCII profile name from turning into ANSI garbage here,
    // exactly as in snapshot.cpp.
    fs::path base = home ? path_from_utf8(home) : fs::path(".");
    return base / ".cache" / "mousiki" / "history" / "history.json";
}

Value play_to_json(const HistoryPlay& p) {
    Value v = Value::make_obj();
    v.set("id", Value::make_str(p.id));
    v.set("t", Value::make_str(p.title));
    v.set("a", Value::make_str(p.artist));
    v.set("len", Value::make_num(p.len_sec));
    v.set("heard", Value::make_num(p.listened_sec));
    v.set("at", Value::make_num(static_cast<double>(p.started_at)));
    v.set("done", Value::make_bool(p.finished));
    return v;
}

HistoryPlay play_from_json(const Value& v) {
    HistoryPlay p;
    if (auto* x = v.find("id")) p.id = x->as_string();
    if (auto* x = v.find("t")) p.title = x->as_string();
    if (auto* x = v.find("a")) p.artist = x->as_string();
    if (auto* x = v.find("len")) p.len_sec = x->as_number(0.0);
    if (auto* x = v.find("heard")) p.listened_sec = x->as_number(0.0);
    if (auto* x = v.find("at")) p.started_at = static_cast<long long>(x->as_number(0.0));
    if (auto* x = v.find("done")) p.finished = x->as_bool(false);
    return p;
}

std::tm local_tm(long long unix_sec) {
    std::tm out{};
    std::time_t t = static_cast<std::time_t>(unix_sec);
#if defined(_WIN32)
    out = win_localtime(t);
#else
    localtime_r(&t, &out);
#endif
    return out;
}

// "2026-09-27" -- the day bucket the per-day totals are summed into.
std::string day_key(long long unix_sec) {
    std::tm tm = local_tm(unix_sec);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return buf;
}

} // namespace

// The identity this whole feature aggregates by: a local path as-is, a stream
// as its video id -- so the same YouTube track played from the search list
// and from a playlist counts as one title rather than two.
std::string history_track_id(bool is_local, const std::string& path, const std::string& video_id) {
    if (is_local) return path;
    if (video_id.empty()) return std::string();
    return "yt:" + video_id;
}

// ---------------------------------------------------------------------------
// Store
// ---------------------------------------------------------------------------

void HistoryStore::load() {
    plays_.clear();
    live_index_ = -1;

    fs::path p = history_path();
    std::error_code ec;
    if (!fs::exists(p, ec)) return;

    std::ifstream in(p, std::ios::binary);
    if (!in.is_open()) return;
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string text = ss.str();

    // A missing or corrupt file is not an error: history is a convenience
    // and must never be able to stop the app from starting.
    Value root;
    if (!parse(text, root) || root.type != Type::Object) return;
    if (auto* arr = root.find("plays")) {
        if (arr->type == Type::Array) {
            for (const auto& item : arr->arr) plays_.push_back(play_from_json(item));
        }
    }
    // A play that was still in progress when the app died last time simply
    // stays historical -- it does not get its live record back, which is why
    // it remains a skip rather than being silently re-counted next run.
    if (plays_.size() > kMaxPlays) plays_.resize(kMaxPlays);
}

void HistoryStore::save() const {
    fs::path p = history_path();
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);

    Value root = Value::make_obj();
    root.set("version", Value::make_num(1));
    Value arr = Value::make_arr();
    for (const auto& pl : plays_) arr.arr.push_back(play_to_json(pl));
    root.set("plays", arr);

    // Temp file + rename, exactly like save_snapshot(): this is written after
    // every single track, so a crash mid-write must not be able to leave an
    // unparseable history.json behind.
    fs::path tmp = p;
    tmp += ".tmp";
    std::ofstream out(tmp, std::ios::trunc | std::ios::binary);
    if (!out.is_open()) return;
    out << write(root);
    out.close();
    fs::rename(tmp, p, ec);
    if (ec) {
        std::ofstream direct(p, std::ios::trunc | std::ios::binary);
        if (direct.is_open()) direct << write(root);
    }
}

void HistoryStore::begin_play(const HistoryPlay& p) {
    plays_.insert(plays_.begin(), p);
    live_index_ = 0;
    if (plays_.size() > kMaxPlays) {
        // Trim from the oldest end -- the live record sits at the front, so
        // it can never be the one dropped.
        plays_.erase(plays_.begin() + static_cast<long>(kMaxPlays), plays_.end());
    }
}

void HistoryStore::add_listened(double sec) {
    if (live_index_ < 0 || live_index_ >= static_cast<int>(plays_.size())) return;
    if (!(sec > 0)) return; // NaN/negative guards: this comes straight from a frame delta
    HistoryPlay& p = plays_[live_index_];
    p.listened_sec += sec;
    if (p.len_sec > 0 && p.listened_sec > p.len_sec) p.listened_sec = p.len_sec;
}

void HistoryStore::finish_live(bool completed) {
    if (live_index_ < 0 || live_index_ >= static_cast<int>(plays_.size())) return;
    HistoryPlay& p = plays_[live_index_];
    if (p.len_sec > 0 && p.listened_sec > p.len_sec) p.listened_sec = p.len_sec;
    p.finished = completed;
    live_index_ = -1;
}

double HistoryStore::live_listened() const {
    const HistoryPlay* p = live_play();
    return p ? p->listened_sec : 0.0;
}

double HistoryStore::live_len() const {
    const HistoryPlay* p = live_play();
    return p ? p->len_sec : 0.0;
}

const HistoryPlay* HistoryStore::live_play() const {
    if (live_index_ < 0 || live_index_ >= static_cast<int>(plays_.size())) return nullptr;
    return &plays_[live_index_];
}

// ---------------------------------------------------------------------------
// Aggregation
// ---------------------------------------------------------------------------

std::vector<HistoryTopRow> history_top(const std::vector<HistoryPlay>& plays, bool most_first) {
    std::vector<HistoryTopRow> rows;
    std::map<std::string, size_t> by_id;
    // Newest-first walk: a title's most recent play supplies the display
    // name, so a file renamed since earlier plays shows its current name.
    for (const HistoryPlay& p : plays) {
        if (p.id.empty()) continue;
        auto it = by_id.find(p.id);
        if (it == by_id.end()) {
            by_id[p.id] = rows.size();
            HistoryTopRow r;
            r.id = p.id;
            r.title = p.title;
            r.len_sec = p.len_sec;
            r.plays = 1;
            r.listened_sec = p.listened_sec;
            rows.push_back(r);
        } else {
            HistoryTopRow& r = rows[it->second];
            ++r.plays;
            r.listened_sec += p.listened_sec;
            if (!p.title.empty()) r.title = p.title;
            if (p.len_sec > 0) r.len_sec = p.len_sec;
        }
    }
    std::sort(rows.begin(), rows.end(), [most_first](const HistoryTopRow& a, const HistoryTopRow& b) {
        if (a.plays != b.plays) return most_first ? a.plays > b.plays : a.plays < b.plays;
        if (a.listened_sec != b.listened_sec) return a.listened_sec > b.listened_sec;
        return a.title < b.title;
    });
    return rows;
}

HistoryStats history_stats(const std::vector<HistoryPlay>& plays) {
    HistoryStats s;
    s.plays = static_cast<int>(plays.size());

    std::map<std::string, int> per_title;
    std::map<std::string, double> per_day;
    for (const HistoryPlay& p : plays) {
        if (p.finished) ++s.finished;
        if (!p.id.empty()) ++per_title[p.id];
        s.total_sec += p.listened_sec;
        if (p.started_at > 0) per_day[day_key(p.started_at)] += p.listened_sec;
    }
    s.skipped = s.plays - s.finished;
    s.completion = s.plays > 0 ? static_cast<double>(s.finished) / s.plays : 0.0;
    for (const auto& kv : per_title) {
        if (kv.second > 1) s.replays += kv.second - 1;
    }

    // Sessions: walk newest -> oldest and cut wherever the silence between
    // one play's END and the next one's start exceeds 30 minutes. Measuring
    // from the previous end (not from its start) is what stops one long album
    // or podcast run from being counted as many short sessions.
    int sessions = 0;
    double session_sum = 0.0;
    long long sess_start = 0, sess_end = 0; // earliest start / latest end of the session being grown
    bool in_session = false;
    for (size_t i = plays.size(); i-- > 0;) {
        const HistoryPlay& p = plays[i];
        if (p.started_at <= 0) continue;
        long long p_end = p.started_at + static_cast<long long>(p.listened_sec + 0.5);
        if (!in_session || sess_start - p_end > kSessionGapSec) {
            if (in_session) {
                ++sessions;
                session_sum += static_cast<double>(std::max<long long>(0, sess_end - sess_start));
            }
            in_session = true;
            sess_start = p.started_at;
            sess_end = p_end;
        } else {
            sess_start = std::min(sess_start, p.started_at);
            sess_end = std::max(sess_end, p_end);
        }
    }
    if (in_session) {
        ++sessions;
        session_sum += static_cast<double>(std::max<long long>(0, sess_end - sess_start));
    }
    s.sessions = sessions;
    s.avg_session_sec = sessions > 0 ? session_sum / sessions : 0.0;
    s.tracks_per_session = sessions > 0 ? static_cast<double>(s.plays) / sessions : 0.0;

    s.days = static_cast<int>(per_day.size());
    s.avg_day_sec = s.days > 0 ? s.total_sec / s.days : 0.0;
    std::string today = day_key(static_cast<long long>(std::time(nullptr)));
    auto it = per_day.find(today);
    if (it != per_day.end()) s.today_sec = it->second;
    return s;
}

// ---------------------------------------------------------------------------
// Formatting
// ---------------------------------------------------------------------------

std::string format_mmss(double sec) {
    if (sec < 0) sec = 0;
    long long t = static_cast<long long>(sec + 0.5);
    long long h = t / 3600, m = (t % 3600) / 60, s = t % 60;
    char buf[32];
    if (h > 0) std::snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld", h, m, s);
    else std::snprintf(buf, sizeof(buf), "%lld:%02lld", m, s);
    return buf;
}

std::string format_len(double sec) {
    if (sec < 0) sec = 0;
    long long t = static_cast<long long>(sec + 0.5);
    char buf[32];
    if (t < 60) {
        std::snprintf(buf, sizeof(buf), "%llds", t);
        return buf;
    }
    long long h = t / 3600, m = (t % 3600) / 60;
    if (h == 0) {
        std::snprintf(buf, sizeof(buf), "%lldm", m);
        return buf;
    }
    std::snprintf(buf, sizeof(buf), "%lldh %02lldm", h, m);
    return buf;
}

std::string format_when(long long unix_sec) {
    std::tm tm = local_tm(unix_sec);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d-%02d %02d:%02d", tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
    return buf;
}

} // namespace muisc
