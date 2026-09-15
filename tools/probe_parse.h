#pragma once
// Parsing for the ffprobe csv output probe_video() consumes:
//
//   ffprobe -v error -select_streams v:0 \
//     -show_entries stream=width,height,r_frame_rate,avg_frame_rate:format=duration \
//     -of csv=p=0 input
//
// prints a stream line like "1920,1080,30000/1001,30000/1001" followed by the
// container duration on its own line ("12.34").
//
// avg_frame_rate is preferred over r_frame_rate: on a variable-frame-rate
// source the latter is the nominal rate while the average is what the timeline
// actually adds up to, and every frame-to-time conversion built on the parsed
// fps (seek offsets, the encoder's -r, duration arithmetic) should follow the
// timeline. Taking the nominal rate makes the stream and the container
// disagree, which shows up as drift on VFR sources.
//
// The stream line is split by hand because sscanf's %s conversion only stops
// at whitespace, never at a comma: a format like "%u,%u,%63s,%63s" hands the
// whole "r_frame_rate,avg_frame_rate" pair to the first %63s, the literal
// comma then fails to match the newline, and the avg branch never ran -- the
// VFR fix this feeds was silently dead code until the split was made
// explicit. tests/probe_parse.cpp locks that behaviour down.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

struct ProbeVideoParams {
    unsigned width = 0;
    unsigned height = 0;
    double fps = 0.0;
    double duration = 0.0;
};

// "30000/1001" -> 29.97; "25" -> 25; "0/0" or garbage leaves `out` untouched.
inline void probe_rate_of(const char *value, double &out) {
    unsigned num = 0, den = 1;
    if (sscanf(value, "%u/%u", &num, &den) == 2 && num && den) out = (double)num / den;
    else if (sscanf(value, "%u", &num) == 1 && num) out = (double)num;
}

// Parse the two ffprobe lines described above into `params`. On failure
// returns false and fills `error` with the offending text.
inline bool parse_probe_csv(const std::string &text, ProbeVideoParams &params,
                            std::string &error) {
    std::string stream_line = text;
    std::string rest;
    if (const size_t nl = stream_line.find('\n'); nl != std::string::npos) {
        rest = stream_line.substr(nl + 1);
        stream_line.resize(nl);
    }
    while (!stream_line.empty() &&
           (stream_line.back() == '\r' || stream_line.back() == '\n' ||
            stream_line.back() == ' '))
        stream_line.pop_back();
    std::vector<std::string> fields;
    for (size_t start = 0; start <= stream_line.size();) {
        const size_t comma = stream_line.find(',', start);
        if (comma == std::string::npos) {
            fields.push_back(stream_line.substr(start));
            break;
        }
        fields.push_back(stream_line.substr(start, comma - start));
        start = comma + 1;
    }
    char rate[64] = {}, rate_avg[64] = {};
    auto copy_field = [&](size_t i, char out[64]) {
        out[0] = 0;
        if (i < fields.size()) {
            const size_t n = std::min(fields[i].size(), (size_t)63);
            std::memcpy(out, fields[i].c_str(), n);
            out[n] = 0;
        }
    };
    unsigned w = 0, h = 0;
    if (fields.size() >= 3) {
        w = (unsigned)strtoul(fields[0].c_str(), nullptr, 10);
        h = (unsigned)strtoul(fields[1].c_str(), nullptr, 10);
        copy_field(2, rate);
        copy_field(3, rate_avg);  // stays empty on an ffprobe too old to report it
    }
    if (fields.size() < 3 || !w || !h) {
        error = "could not parse ffprobe output: " + text;
        return false;
    }
    params.width = w;
    params.height = h;
    // format=duration lives on its own line after the stream line; 0 if absent.
    params.duration = strtod(rest.c_str(), nullptr);
    if (rate_avg[0]) probe_rate_of(rate_avg, params.fps);
    if (params.fps <= 0) probe_rate_of(rate, params.fps);
    return true;
}
