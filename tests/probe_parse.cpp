// Assertions for the ffprobe csv parser in tools/probe_parse.h.
//
// The parser used to be an sscanf format string whose %s conversion cannot
// stop at a comma, so the avg_frame_rate branch was dead code and VFR sources
// kept the nominal rate. These cases lock the intended behaviour down; the
// VFR case is the one that used to fail.
//
// Usage: probe_parse          (no arguments; exit code 0 = all passed)

#include "../tools/probe_parse.h"

#include <cstdio>

static int failures = 0;

static void expect(const char *name, const std::string &text, unsigned w, unsigned h,
                   double fps, double dur) {
    ProbeVideoParams p;
    std::string err;
    if (!parse_probe_csv(text, p, err)) {
        printf("%-42s -> FAIL (rejected: %s)\n", name, err.c_str());
        ++failures;
        return;
    }
    const bool ok = p.width == w && p.height == h &&
                    (fps == 0.0 ? p.fps == 0.0
                                : (p.fps - fps) < 1e-6 && (fps - p.fps) < 1e-6) &&
                    (p.duration - dur) < 1e-6 && (dur - p.duration) < 1e-6;
    printf("%-42s -> %ux%u fps=%.6f dur=%.3f  %s\n", name, p.width, p.height, p.fps,
           p.duration, ok ? "OK" : "FAIL");
    if (!ok) ++failures;
}

static void expect_rejected(const char *name, const std::string &text) {
    ProbeVideoParams p;
    std::string err;
    if (parse_probe_csv(text, p, err)) {
        printf("%-42s -> FAIL (accepted garbage)\n", name);
        ++failures;
    } else {
        printf("%-42s -> rejected  OK\n", name);
    }
}

int main() {
    // The everyday constant-frame-rate line: avg equals the nominal rate.
    expect("CFR 29.97 (avg == nominal)",
           "1920,1080,30000/1001,30000/1001\n12.34", 1920, 1080, 30000.0 / 1001, 12.34);

    // The case the sscanf version got wrong: the average differs from the
    // nominal rate, and the timeline (not the container's label) must win.
    expect("VFR (avg differs from nominal)",
           "1920,1080,30000/1001,2997/125\n60.5", 1920, 1080, 2997.0 / 125, 60.5);

    // An ffprobe too old to report avg_frame_rate: the nominal rate is all
    // there is.
    expect("old ffprobe (no avg field)",
           "1920,1080,30000/1001\n12.34", 1920, 1080, 30000.0 / 1001, 12.34);

    // ffprobe prints 0/0 when it cannot know the average: fall back to the
    // nominal rate rather than to zero.
    expect("avg=0/0 falls back to nominal",
           "1280,720,24000/1001,0/0\n5.5", 1280, 720, 24000.0 / 1001, 5.5);

    // Windows line endings and integer rates.
    expect("CRLF + integer rates",
           "1920,1080,25,25\r\n90.0", 1920, 1080, 25.0, 90.0);

    // Nonsense must be refused, not guessed at.
    expect_rejected("garbage rejected", "garbage");
    expect_rejected("empty rejected", "");

    if (failures) {
        printf("\n%d FAILURE(S)\n", failures);
        return 1;
    }
    printf("\nALL PASSED\n");
    return 0;
}
