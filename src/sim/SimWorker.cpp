#include "SimWorker.h"

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

// Minimal declarations of the libngspice shared API (sharedspice.h).
struct ngcomplex {
    double cx_real;
    double cx_imag;
};
struct vecvalues {
    char *name;
    double creal;
    double cimag;
    bool is_scale;
    bool is_complex;
};
struct vecvaluesall {
    int veccount;
    int vecindex;
    vecvalues **vecsa;
};
struct vecinfo {
    int number;
    char *vecname;
    bool is_real;
    void *pdvec;
    void *pdvecscale;
};
struct vecinfoall {
    char *name;
    char *title;
    char *date;
    char *type;
    int veccount;
    vecinfo **vecs;
};

using SendChar = int(char *, int, void *);
using SendStat = int(char *, int, void *);
using ControlledExit = int(int, bool, bool, int, void *);
using SendData = int(vecvaluesall *, int, int, void *);
using SendInitData = int(vecinfoall *, int, void *);
using BGThreadRunning = int(bool, int, void *);

struct vector_info {
    char *v_name;
    int v_type;
    short v_flags;
    double *v_realdata;
    ngcomplex *v_compdata;
    int v_length;
};
using FnVecInfo = vector_info *(*)(char *);
using FnAllVecs = char **(*)(char *);

using FnInit = int (*)(SendChar *, SendStat *, ControlledExit *, SendData *, SendInitData *, BGThreadRunning *, void *);
using FnCommand = int (*)(const char *);
using FnCirc = int (*)(char **);
using FnRunning = bool (*)();
using FnCurPlot = char *(*)();
using FnAllPlots = char **(*)();

std::mutex g_outMutex;
std::atomic<bool> g_bgRunning{false};
std::atomic<bool> g_exited{false};
std::atomic<int> g_exitStatus{0};
std::atomic<bool> g_errorSeen{false};
FILE *g_live = nullptr;
std::mutex g_liveMutex;

void emit(const char *tag, const std::string &text)
{
    std::lock_guard<std::mutex> lock(g_outMutex);
    std::string t = text;
    for (char &c : t)
        if (c == '\n' || c == '\r')
            c = ' ';
    std::fprintf(stdout, "%s %s\n", tag, t.c_str());
    std::fflush(stdout);
}

int cbChar(char *s, int, void *)
{
    std::string line = s ? s : "";
    // ngspice prefixes stdout/stderr lines.
    if (line.rfind("stdout ", 0) == 0)
        line = line.substr(7);
    else if (line.rfind("stderr ", 0) == 0) {
        line = line.substr(7);
        if (line.find("Error") != std::string::npos || line.find("error") != std::string::npos)
            g_errorSeen = true;
    }
    emit("LOG", line);
    return 0;
}

int cbStat(char *s, int, void *)
{
    std::string st = s ? s : "";
    emit("STAT", st);
    const auto colon = st.find(':');
    if (colon != std::string::npos) {
        const double pct = std::atof(st.c_str() + colon + 1);
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.1f", pct);
        emit("PROGRESS", buf);
    }
    return 0;
}

int cbExit(int status, bool, bool, int, void *)
{
    g_exitStatus = status;
    g_exited = true;
    return 0;
}

// Live data stream: a simple binary format the GUI can tail while the
// simulation runs. Header: "LSLV" int32 count, then names (NUL terminated),
// then flags byte per vector (1=complex). Each point: int32 marker 0x50544E54
// followed by count doubles (or 2*count for complex vectors).
std::vector<bool> g_liveComplex;

int cbInitData(vecinfoall *info, int, void *)
{
    std::lock_guard<std::mutex> lock(g_liveMutex);
    if (!g_live || !info)
        return 0;
    const int32_t n = info->veccount;
    std::fwrite("LSLV", 1, 4, g_live);
    std::fwrite(&n, 4, 1, g_live);
    g_liveComplex.assign(n, false);
    for (int i = 0; i < n; ++i) {
        const char *nm = info->vecs[i]->vecname;
        std::fwrite(nm, 1, std::strlen(nm) + 1, g_live);
    }
    for (int i = 0; i < n; ++i) {
        const char flag = info->vecs[i]->is_real ? 0 : 1;
        g_liveComplex[i] = flag;
        std::fwrite(&flag, 1, 1, g_live);
    }
    std::fflush(g_live);
    return 0;
}

int cbData(vecvaluesall *vals, int, int, void *)
{
    std::lock_guard<std::mutex> lock(g_liveMutex);
    if (!g_live || !vals)
        return 0;
    const int32_t marker = 0x50544E54;
    std::fwrite(&marker, 4, 1, g_live);
    for (int i = 0; i < vals->veccount; ++i) {
        const vecvalues *v = vals->vecsa[i];
        std::fwrite(&v->creal, 8, 1, g_live);
        if (i < int(g_liveComplex.size()) && g_liveComplex[i])
            std::fwrite(&v->cimag, 8, 1, g_live);
    }
    static auto last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - last > std::chrono::milliseconds(100)) {
        std::fflush(g_live);
        last = now;
    }
    return 0;
}

int cbBg(bool notRunning, int, void *)
{
    g_bgRunning = !notRunning;
    return 0;
}

void *openNgspice(std::string *err)
{
    std::vector<std::string> candidates;
    if (const char *env = std::getenv("LINSPICE_NGSPICE_LIB"))
        candidates.emplace_back(env);
#ifdef LINSPICE_NGSPICE_BUILD_PATH
    candidates.emplace_back(LINSPICE_NGSPICE_BUILD_PATH); // found when LinSpice was built
#endif
    candidates.emplace_back("libngspice.so.0");
    candidates.emplace_back("libngspice.so");
    candidates.emplace_back("/usr/lib/libngspice.so.0");
    candidates.emplace_back("/usr/lib/x86_64-linux-gnu/libngspice.so.0");
    candidates.emplace_back("/usr/local/lib/libngspice.so.0");
    candidates.emplace_back("/run/current-system/sw/lib/libngspice.so.0"); // NixOS system profile
    std::string errors;
    for (const auto &c : candidates) {
        if (c.empty())
            continue;
        void *h = dlopen(c.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (h)
            return h;
        errors += dlerror();
        errors += "; ";
    }
    if (err)
        *err = errors;
    return nullptr;
}

} // namespace

int runSimWorker(int argc, char **argv)
{
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s --sim-worker <netlist> <raw> [--live <file>]\n", argv[0]);
        return 2;
    }
    const std::string netlistPath = argv[2];
    const std::string rawPath = argv[3];
    std::string livePath;
    std::string seed;
    for (int i = 4; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--live") == 0)
            livePath = argv[i + 1];
        else if (std::strcmp(argv[i], "--seed") == 0)
            seed = argv[i + 1];
    }

    std::string err;
    void *lib = openNgspice(&err);
    if (!lib) {
        emit("LOG", "ERROR: could not load libngspice: " + err);
        emit("DONE", "3");
        return 3;
    }
    auto init = reinterpret_cast<FnInit>(dlsym(lib, "ngSpice_Init"));
    auto command = reinterpret_cast<FnCommand>(dlsym(lib, "ngSpice_Command"));
    auto circ = reinterpret_cast<FnCirc>(dlsym(lib, "ngSpice_Circ"));
    auto running = reinterpret_cast<FnRunning>(dlsym(lib, "ngSpice_running"));
    auto curPlot = reinterpret_cast<FnCurPlot>(dlsym(lib, "ngSpice_CurPlot"));
    auto allPlots = reinterpret_cast<FnAllPlots>(dlsym(lib, "ngSpice_AllPlots"));
    if (!init || !command || !circ || !running || !curPlot || !allPlots) {
        emit("LOG", "ERROR: libngspice is missing required symbols");
        emit("DONE", "3");
        return 3;
    }

    if (!livePath.empty())
        g_live = std::fopen(livePath.c_str(), "wb");

    init(cbChar, cbStat, cbExit, g_live ? cbData : nullptr, g_live ? cbInitData : nullptr, cbBg, nullptr);

    // Load the XSPICE code models (POLY sources, digital, ideal diodes, ...).
    // In shared-library mode spinit is not always found, so load them explicitly
    // from the "ngspice" directory next to the library.
    {
        Dl_info info{};
        if (dladdr(reinterpret_cast<void *>(init), &info) && info.dli_fname) {
            std::string dir = info.dli_fname;
            const auto slash = dir.rfind('/');
            dir = slash == std::string::npos ? std::string(".") : dir.substr(0, slash);
            const char *models[] = {"spice2poly.cm", "analog.cm", "digital.cm", "xtradev.cm", "xtraevt.cm", "table.cm"};
            for (const char *m : models) {
                const std::string path = dir + "/ngspice/" + m;
                if (FILE *f = std::fopen(path.c_str(), "rb")) {
                    std::fclose(f);
                    command(("codemodel " + path).c_str());
                }
            }
        }
    }
    command("set ngbehavior=ltpsa");
    command("set filetype=binary");
    command("set noaskquit");
    command("set nomoremode");
    if (!seed.empty()) {
        // "setseed" re-seeds the generator; setting the variable alone only takes effect at startup.
        command(("set rndseed=" + seed).c_str());
        command(("setseed " + seed).c_str());
    }

    // Read the netlist and hand it to ngspice line by line.
    std::ifstream in(netlistPath, std::ios::binary);
    if (!in) {
        emit("LOG", "ERROR: cannot open netlist " + netlistPath);
        emit("DONE", "4");
        return 4;
    }
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        lines.push_back(line);
    }
    std::vector<char *> ptrs;
    for (auto &l : lines)
        ptrs.push_back(l.data());
    ptrs.push_back(nullptr);
    circ(ptrs.data());

    // Fail fast if the circuit did not parse.
    if (g_exited) {
        emit("DONE", "5");
        return 5;
    }
    const char *beforePlot = curPlot();
    const std::string plotBefore = beforePlot ? beforePlot : "";

    command("bg_run");
    // Wait for the background thread to start, then for it to finish.
    for (int i = 0; i < 200 && !running(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

    bool halted = false;
    bool stdinOpen = true;
    while (running()) {
        struct pollfd pfd = {0, POLLIN, 0};
        if (!stdinOpen) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        } else if (poll(&pfd, 1, 50) > 0 && (pfd.revents & (POLLIN | POLLHUP))) {
            char buf[256];
            const ssize_t n = read(0, buf, sizeof buf - 1);
            if (n > 0) {
                buf[n] = 0;
                if (std::strstr(buf, "halt") && !halted) {
                    command("bg_halt");
                    halted = true;
                    emit("LOG", "Simulation halted by user.");
                }
            } else {
                stdinOpen = false; // EOF: keep running without control input
            }
        }
        if (g_exited)
            break;
    }

    {
        std::lock_guard<std::mutex> lock(g_liveMutex);
        if (g_live) {
            std::fclose(g_live);
            g_live = nullptr;
        }
    }

    // Write every plot produced by this run into the raw file (oldest first).
    // The vectors are read through the shared-library API so that empty or
    // unavailable vectors (e.g. some device currents in AC analysis) are skipped
    // instead of aborting the whole write.
    auto vecInfo = reinterpret_cast<FnVecInfo>(dlsym(lib, "ngGet_Vec_Info"));
    auto allVecs = reinterpret_cast<FnAllVecs>(dlsym(lib, "ngSpice_AllVecs"));
    std::vector<std::string> plots;
    if (char **all = allPlots()) {
        for (int i = 0; all[i]; ++i) {
            const std::string p = all[i];
            if (p != "const")
                plots.push_back(p);
        }
    }
    int written = 0;
    FILE *raw = std::fopen(rawPath.c_str(), "wb");
    if (raw && vecInfo && allVecs) {
        for (auto it = plots.rbegin(); it != plots.rend(); ++it) {
            const std::string &plot = *it;
            command(("setplot " + plot).c_str());
            std::vector<std::string> names;
            if (char **vs = allVecs(const_cast<char *>(plot.c_str())))
                for (int i = 0; vs[i]; ++i)
                    names.push_back(vs[i]);
            // ngGet_Vec_Info returns a pointer to a static struct that is
            // overwritten by the next call, so copy what we need right away.
            struct Vec {
                std::string name;
                vector_info info;
            };
            std::vector<Vec> vecs;
            int scaleIdx = -1;
            for (const auto &n : names) {
                vector_info *vi = vecInfo(const_cast<char *>(n.c_str()));
                if (!vi || vi->v_length <= 0 || (!vi->v_realdata && !vi->v_compdata))
                    continue;
                if (scaleIdx < 0 && (n == "time" || n == "frequency" || (n.size() > 6 && n.compare(n.size() - 6, 6, "-sweep") == 0)))
                    scaleIdx = int(vecs.size());
                vecs.push_back({n, *vi});
            }
            if (vecs.empty())
                continue;
            if (scaleIdx > 0)
                std::swap(vecs[0], vecs[scaleIdx]);
            // Keep only vectors as long as the scale.
            const int npts = vecs[0].info.v_length;
            std::vector<Vec> keep;
            for (const auto &v : vecs)
                if (v.info.v_length == npts)
                    keep.push_back(v);
            bool complex = false;
            for (const auto &v : keep)
                if (v.info.v_compdata && !v.info.v_realdata)
                    complex = true;
            std::string plotName = "Unknown";
            const std::string prefix = plot.substr(0, plot.find_first_of("0123456789"));
            if (prefix == "tran") plotName = "Transient Analysis";
            else if (prefix == "ac") plotName = "AC Analysis";
            else if (prefix == "dc") plotName = "DC transfer characteristic";
            else if (prefix == "op") plotName = "Operating Point";
            else if (prefix == "noise") plotName = scaleIdx >= 0 ? "Noise Spectral Density Curves" : "Integrated Noise"; // only the spectrum has a frequency scale
            else if (prefix == "tf") plotName = "Transfer Function";
            else if (prefix == "sp") plotName = "S-Parameter Analysis";
            else if (prefix == "pz") plotName = "Pole-Zero Analysis";
            static const char *typeNames[] = {"notype", "time", "frequency", "voltage", "current", "voltage-density",
                                              "current-density", "sqr-voltage-density", "sqr-current-density", "sqr-voltage",
                                              "sqr-current", "pole", "zero", "s-param", "temp-sweep", "res-sweep",
                                              "impedance", "admittance", "power", "phase", "decibel", "capacitance", "charge"};
            // LTspice's layout: UTF-16LE header; real data stores the scale as double and every
            // other vector as float32 (half the size of all-double files).
            std::string hdr;
            auto add = [&hdr](const char *fmt, auto... args) {
                char line[4096];
                std::snprintf(line, sizeof line, fmt, args...);
                hdr += line;
            };
            add("Title: %s\nDate: \nPlotname: %s\nFlags: %s\nNo. Variables: %d\nNo. Points: %d\nVariables:\n",
                plot.c_str(), plotName.c_str(), complex ? "complex" : "real", int(keep.size()), npts);
            for (size_t i = 0; i < keep.size(); ++i) {
                const int t = keep[i].info.v_type;
                const char *tn = (t >= 0 && t < int(sizeof typeNames / sizeof *typeNames)) ? typeNames[t] : "notype";
                // Name vectors the way ngspice's raw writer does: v(node), i(branch).
                std::string name = keep[i].name;
                const bool isScale = i == 0 && scaleIdx >= 0;
                if (!isScale) {
                    const auto hash = name.find("#branch");
                    if (hash != std::string::npos && name.find('(') == std::string::npos)
                        name = "i(" + name.substr(0, hash) + ")";
                    else if (name.find_first_of("(@#") == std::string::npos && prefix != "noise" && prefix != "tf")
                        name = "v(" + name + ")";
                }
                add("\t%d\t%s\t%s\n", int(i), name.c_str(), tn);
            }
            hdr += "Binary:\n";
            for (unsigned char c : hdr) {
                const char u16[2] = {char(c), 0};
                std::fwrite(u16, 1, 2, raw);
            }
            std::vector<char> row;
            for (int p = 0; p < npts; ++p) {
                row.clear();
                for (size_t i = 0; i < keep.size(); ++i) {
                    const auto &v = keep[i];
                    const double re = v.info.v_realdata ? v.info.v_realdata[p] : v.info.v_compdata[p].cx_real;
                    const double im = v.info.v_realdata ? 0.0 : v.info.v_compdata[p].cx_imag;
                    auto put = [&row](const void *d, size_t n) { row.insert(row.end(), (const char *)d, (const char *)d + n); };
                    if (complex) {
                        put(&re, 8);
                        put(&im, 8);
                    } else if (i == 0) {
                        put(&re, 8);
                    } else {
                        const float f = float(re);
                        put(&f, 4);
                    }
                }
                std::fwrite(row.data(), 1, row.size(), raw);
            }
            ++written;
            emit("PLOT", plot);
        }
    }
    if (raw)
        std::fclose(raw);
    const int rc = g_exited ? int(g_exitStatus) : (written ? 0 : 6);
    char buf[16];
    std::snprintf(buf, sizeof buf, "%d", rc);
    emit("DONE", buf);
    std::fflush(stdout);
    // Do not dlclose: ngspice background threads may still be tearing down.
    std::_Exit(rc);
}
