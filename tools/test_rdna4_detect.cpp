// Checks the RDNA 4 predicate in core/gpu_detection.h against real cards.
//
// Why this exists: the id test it replaces accepted 0x7480..0x74DF, which is
// Navi 33 -- the RX 7600 -- so on that card the program set
// HSA_OVERRIDE_GFX_VERSION=12.0.1 and, by the note in the header, every module
// then failed to load. Nothing in the build or the test suite noticed, because
// the predicate had never been exercised anywhere except on the one GPU in the
// developer's machine. This is the smallest thing that would have caught it:
// every vendor id and device id below is a real one, taken from DeviceHunt's
// PCI database (vendor 1002), and the names are spelled the way DXGI reports
// them.
//
// Build (VS x64 Developer prompt, from the repository root):
//   cl /EHsc /std:c++17 /I. tools\test_rdna4_detect.cpp /Fe:%TEMP%\test_rdna4.exe
//   %TEMP%\test_rdna4.exe
//
// Exit code is the number of failed expectations, so it can be used as a gate.
#include "../core/gpu_detection.h"

#include <cstdio>
#include <string>

namespace {

struct Case {
    const wchar_t *name;
    UINT device_id;
    bool expect_rdna4;
    const wchar_t *why;
};

// Device ids for the discrete cards, all looked up in DeviceHunt (PCI 1002):
//   Navi 48 = 0x7550   RX 9070 XT / 9070 / 9070 GRE, Radeon AI PRO R9700
//   Navi 44 = 0x7590   RX 9060 XT / 9060
//   Navi 31 = 0x744C   RX 7900 XT / XTX / GRE / 7900M
//   Navi 32 = 0x747E   RX 7800 XT / 7700 XT
//   Navi 33 = 0x7480   RX 7600 / 7600 XT / 7600M XT / 7600S / 7700S / W7600
//   Navi 21 = 0x73BF   RX 6800 / 6800 XT / 6900 XT
//   Navi 22 = 0x73DF   RX 6700 / 6700 XT / 6750 XT / 6800M / 6850M XT
//   Navi 23 = 0x73FF   RX 6600 / 6600 XT / 6600M
//   Navi 24 = 0x743F   RX 6400 / 6500 XT / 6500M
//
// The iGPU ids are representative rather than looked up -- an APU's id is in the
// 0x15xx/0x16xx range, far from anything the predicate accepts, and what these
// cases assert is only that an iGPU whose name is 6xxM/7xxM/8xxM is not read as
// RDNA 4 by name. Asserting the exact id would claim a lookup that was not done.
const Case kCases[] = {
    // RDNA 4, must get the override.
    {L"amd radeon rx 9070 xt", 0x7550, true,  L"RDNA 4, Navi 48"},
    {L"amd radeon rx 9070", 0x7550, true,  L"RDNA 4, Navi 48"},
    {L"amd radeon rx 9070 gre", 0x7550, true,  L"RDNA 4, Navi 48"},
    {L"amd radeon rx 9060 xt", 0x7590, true,  L"RDNA 4, Navi 44"},
    {L"amd radeon rx 9060", 0x7590, true,  L"RDNA 4, Navi 44"},
    {L"amd radeon ai pro r9700", 0x1234, true, L"workstation RDNA 4, by name"},

    // RDNA 3. The first entry is the card the old range mis-flagged.
    {L"amd radeon rx 7600", 0x7480, false, L"Navi 33 -- the card the old 0x7480..0x74DF range broke"},
    {L"amd radeon rx 7600 xt", 0x7481, false, L"Navi 33"},
    {L"amd radeon rx 7650 gre", 0x7483, false, L"RX 7000 series, RDNA 3 -- also inside the old range"},
    {L"amd radeon rx 7400", 0x749f, false, L"RX 7000 series OEM card, RDNA 3 -- also inside the old range"},
    {L"amd radeon rx 7600m xt", 0x7483, false, L"Navi 33, mobile"},
    {L"amd radeon rx 7700s", 0x7489, false, L"Navi 33, mobile"},
    {L"amd radeon pro w7600", 0x748b, false, L"Navi 33, workstation"},
    {L"amd radeon rx 7900 xt", 0x744c, false, L"Navi 31"},
    {L"amd radeon rx 7900 xtx", 0x744c, false, L"Navi 31"},
    {L"amd radeon rx 7900 gre", 0x744c, false, L"Navi 31"},
    {L"amd radeon rx 7800 xt", 0x747e, false, L"Navi 32"},
    {L"amd radeon rx 7700 xt", 0x747e, false, L"Navi 32"},
    {L"amd radeon 780m", 0x15bf, false, L"Phoenix iGPU, RDNA 3"},

    // RDNA 2 and older: no matrix units, nothing here wants the override.
    {L"amd radeon rx 6900 xt", 0x73bf, false, L"Navi 21, RDNA 2"},
    {L"amd radeon rx 6800 xt", 0x73bf, false, L"Navi 21, RDNA 2"},
    {L"amd radeon rx 6750 xt", 0x73df, false, L"Navi 22, RDNA 2"},
    {L"amd radeon rx 6600 xt", 0x73ff, false, L"Navi 23, RDNA 2"},
    {L"amd radeon rx 6500 xt", 0x743f, false, L"Navi 24, RDNA 2"},
    {L"amd radeon 680m", 0x1681, false, L"Rembrandt iGPU, RDNA 2"},
    {L"amd radeon 610m", 0x1506, false, L"Mendocino iGPU, RDNA 2"},

    // RDNA 3.5 iGPUs: 8xxx names, native WMMA, and not RDNA 4 by name or id.
    {L"amd radeon 890m", 0x150e, false, L"Strix Point iGPU"},
    {L"amd radeon 8060s", 0x1586, false, L"Strix Halo iGPU"},
};

}  // namespace

int main() {
    int failed = 0;
    for (const Case &c : kCases) {
        const bool got = dlssnr::is_rdna4_gpu(c.name, c.device_id);
        const bool ok = (got == c.expect_rdna4);
        if (!ok) ++failed;
        wprintf(L"%s  %-26ls dev=%04X  expect=%s  got=%s   (%ls)\n",
                ok ? L"ok  " : L"FAIL",
                c.name, c.device_id,
                c.expect_rdna4 ? L"rdna4" : L"other",
                got ? L"rdna4" : L"other",
                c.why);
    }
    printf("\n%d cases, %d failed\n", (int)(sizeof(kCases) / sizeof(kCases[0])), failed);

    // The rule this replaced, kept as the exhibit. It is what made the RX 7600 a
    // failure case above, and it shows the two directions the new rule fixes:
    // a card it wrongly claimed, and one it never claimed.
    auto old_rule = [](const std::wstring &n, UINT id) {
        return (n.find(L"9070") != std::wstring::npos || n.find(L"9060") != std::wstring::npos ||
                n.find(L"9080") != std::wstring::npos || n.find(L"9090") != std::wstring::npos ||
                n.find(L"rx 9") != std::wstring::npos || n.find(L"radeon 9") != std::wstring::npos ||
                (id >= 0x7480 && id <= 0x74DF));
    };
    auto verdict = [](bool b) { return b ? "rdna4" : "other"; };
    printf("\nnegative control -- what the superseded rule did:\n");
    printf("  rx 7600        dev=7480   old=%s   new=%s   (RDNA 3 claimed as RDNA 4)\n",
           verdict(old_rule(L"amd radeon rx 7600", 0x7480)),
           verdict(dlssnr::is_rdna4_gpu(L"amd radeon rx 7600", 0x7480)));
    printf("  ai pro r9700   dev=7550   old=%s   new=%s   (RDNA 4 never claimed)\n",
           verdict(old_rule(L"amd radeon ai pro r9700", 0x7550)),
           verdict(dlssnr::is_rdna4_gpu(L"amd radeon ai pro r9700", 0x7550)));
    return failed;
}
