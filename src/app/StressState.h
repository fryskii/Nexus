#pragma once
// State of a --stress run (see Stress.cpp). Separate header so App's destructor sees a complete type.
#include "app/App.h"

#include <map>
#include <random>
#include <string>
#include <vector>

namespace nx {

struct StressSample {
    double t = 0;
    uint64_t privateBytes = 0, workingSet = 0;
    DWORD handles = 0, gdi = 0, user = 0, threads = 0;
    double cpuSec = 0;
    uint64_t frames = 0;
};

struct App::StressState {
    int seconds = 60;
    std::wstring csvPath;
    std::wstring only; // ",navigate,hover," from NEXUS_STRESS_ONLY; empty = full mix
    std::mt19937 rng;
    uint64_t start = 0;
    std::vector<StressSample> samples;
    std::vector<float> paintMs;
    std::map<int, std::vector<float>> paintMsByPage; // PageId -> frame times
    std::map<std::string, uint64_t> actions;
    std::vector<HWND> inspectors;
    std::vector<float> snapshotMs;          // time spent in App::OnSnapshot
    std::map<UINT_PTR, uint64_t> timerFires; // WM_TIMER deliveries by id
    std::map<std::pair<UINT, std::wstring>, uint64_t> messages; // main-loop messages by (id, window class)
    std::string log; // "seconds action detail" per line, for reproducing a run
    bool finished = false;
};

} // namespace nx
