#include "ps2_vu1_impl.inl"
#include "runtime/gs/gs_frontend.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <intrin.h>
#include <dbghelp.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "dbghelp.lib")

void registerGeneratedVu1Programs();
void registerSotcVu1NativeKernels();
void printVu1VerifySummary();

namespace
{
    using A = VU1InterpreterAccess;

    struct Sampler
    {
        std::atomic<bool> running{false};
        std::atomic<bool> active{false};
        std::unordered_map<uint64_t, uint64_t> hits;
        uint64_t samples = 0;
        std::thread thread;

        std::vector<uint64_t> rips;

        void start(HANDLE target)
        {
            rips.reserve(1u << 22);
            running = true;
            thread = std::thread([this, target]()
                                 {
                                     timeBeginPeriod(1);
                                     while (running.load())
                                     {
                                         Sleep(1);
                                         if (!active.load())
                                             continue;
                                         if (SuspendThread(target) == static_cast<DWORD>(-1))
                                             continue;
                                         CONTEXT context{};
                                         context.ContextFlags = CONTEXT_CONTROL;
                                         const bool ok = GetThreadContext(target, &context) != 0;
                                         ResumeThread(target);
                                         if (ok && rips.size() < rips.capacity())
                                             rips.push_back(context.Rip);
                                     }
                                     timeEndPeriod(1); });
        }

        void stop()
        {
            running = false;
            thread.join();
            for (const uint64_t rip : rips)
                ++hits[rip];
            samples = rips.size();
        }

        void report()
        {
            HANDLE process = GetCurrentProcess();
            SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
            SymInitialize(process, nullptr, TRUE);
            std::map<std::string, uint64_t> functions;
            std::map<std::string, uint64_t> lines;
            for (const auto &[address, count] : hits)
            {
                alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 512];
                auto *symbol = reinterpret_cast<SYMBOL_INFO *>(storage);
                symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
                symbol->MaxNameLen = 511;
                DWORD64 displacement = 0;
                std::string name = "?";
                if (SymFromAddr(process, address, &displacement, symbol))
                    name = displacement > 0x10000u ? std::string("far+") + symbol->Name : std::string(symbol->Name);
                IMAGEHLP_LINE64 line{};
                line.SizeOfStruct = sizeof(line);
                DWORD lineDisplacement = 0;
                std::string where = name;
                if (SymGetLineFromAddr64(process, address, &lineDisplacement, &line))
                {
                    std::string file = line.FileName;
                    const size_t slash = file.find_last_of("\\/");
                    where += " (" + file.substr(slash == std::string::npos ? 0 : slash + 1) + ":" + std::to_string(line.LineNumber) + ")";
                }
                functions[name] += count;
                lines[where] += count;
                if (name == "_NLG_Return2" && count * 200u > samples)
                    std::printf("  raw %llx +%llx count %llu\n", static_cast<unsigned long long>(address),
                                static_cast<unsigned long long>(displacement), static_cast<unsigned long long>(count));
            }
            const auto top = [&](const char *title, const std::map<std::string, uint64_t> &table, size_t limit)
            {
                std::vector<std::pair<uint64_t, std::string>> sorted;
                for (const auto &[key, count] : table)
                    sorted.emplace_back(count, key);
                std::sort(sorted.rbegin(), sorted.rend());
                std::printf("%s (%llu samples):\n", title, static_cast<unsigned long long>(samples));
                for (size_t i = 0; i < std::min(limit, sorted.size()); ++i)
                    std::printf("  %6.2f%%  %s\n", 100.0 * sorted[i].first / std::max<uint64_t>(samples, 1u), sorted[i].second.c_str());
            };
            top("functions", functions, 25);
            top("lines", lines, 60);
        }
    };

    struct Record
    {
        uint32_t maxCycles = 0;
        uint64_t hash = 0;
        uint64_t tick = 0;
        A::TraceState state;
        size_t diff = 0;
    };

    std::vector<uint8_t> readFile(const std::string &path)
    {
        std::ifstream file(path, std::ios::binary);
        return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

    template <typename T>
    T readAt(const std::vector<uint8_t> &buffer, size_t &pos)
    {
        T value{};
        std::memcpy(&value, buffer.data() + pos, sizeof(T));
        pos += sizeof(T);
        return value;
    }

    void applyDiff(const std::vector<uint8_t> &buffer, size_t pos, uint8_t *mem)
    {
        for (;;)
        {
            const uint16_t index = readAt<uint16_t>(buffer, pos);
            const uint16_t count = readAt<uint16_t>(buffer, pos);
            if (index == 0xFFFFu && count == 0u)
                return;
            std::memcpy(mem + index * 16u, buffer.data() + pos, count * 16u);
            pos += count * 16u;
        }
    }

    // --compare: tolerance comparison of the compiled code + native kernels against the interpreter.
    // Differences are sorted into flag-only (MAC/status), value-level (same GS command stream, values
    // a few units apart) and control-flow (different branches, packet structure, ADC bits, VI).
    uint32_t ulpDistance(uint32_t a, uint32_t b)
    {
        const auto ordered = [](uint32_t bits) -> int64_t
        { return (bits & 0x80000000u) ? -static_cast<int64_t>(bits & 0x7FFFFFFFu) : static_cast<int64_t>(bits); };
        const int64_t d = ordered(a) - ordered(b);
        return static_cast<uint32_t>(std::min<int64_t>(d < 0 ? -d : d, 0xFFFFFFFFll));
    }

    struct CompareStats
    {
        uint64_t runs = 0, identical = 0, flagsOnly = 0, valueOnly = 0, control = 0;
        // value-level maxima and counts over all GS packets
        uint64_t xyWords = 0, zWords = 0, colorWords = 0, stWords = 0, fogWords = 0;
        uint32_t maxXy = 0, maxZ = 0, maxColor = 0, maxStUlp = 0, maxFog = 0;
        std::map<uint32_t, uint64_t> xyHist, zHist, stHist;
        uint64_t memWords = 0, vfLanes = 0;
        uint32_t maxMemUlp = 0, maxVfUlp = 0;
        uint64_t vertices = 0;
        std::map<std::string, uint64_t> controlReasons;
        int examples = 0;
    };

    struct PacketCompare
    {
        bool structural = false;
        bool valueDiff = false;
        std::string reason;
    };

    uint32_t word(const std::vector<uint8_t> &p, size_t offset)
    {
        uint32_t value = 0;
        std::memcpy(&value, p.data() + offset, 4u);
        return value;
    }

    void comparePacket(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b, CompareStats &stats, PacketCompare &out)
    {
        if (a.size() != b.size())
        {
            out.structural = true;
            out.reason = "packet size " + std::to_string(a.size()) + "/" + std::to_string(b.size());
            return;
        }
        const auto bucket = [](uint32_t d) { return d <= 4u ? d : d <= 16u ? 16u : d <= 256u ? 256u : 0xFFFFFFFFu; };
        size_t pos = 0;
        while (pos + 16u <= a.size())
        {
            if (std::memcmp(a.data() + pos, b.data() + pos, 16u) != 0)
            {
                out.structural = true;
                out.reason = "gif tag";
                return;
            }
            uint64_t lo = 0, hi = 0;
            std::memcpy(&lo, a.data() + pos, 8u);
            std::memcpy(&hi, a.data() + pos + 8u, 8u);
            pos += 16u;
            const uint32_t nloop = static_cast<uint32_t>(lo & 0x7FFFu);
            const bool eop = (lo >> 15) & 1u;
            const uint32_t flg = static_cast<uint32_t>((lo >> 58) & 3u);
            uint32_t nreg = static_cast<uint32_t>((lo >> 60) & 0xFu);
            if (nreg == 0u)
                nreg = 16u;
            if (flg == 0u)
            {
                for (uint32_t loop = 0; loop < nloop; ++loop)
                    for (uint32_t r = 0; r < nreg; ++r, pos += 16u)
                    {
                        if (pos + 16u > a.size())
                            return;
                        if (std::memcmp(a.data() + pos, b.data() + pos, 16u) == 0)
                        {
                            const uint32_t reg = static_cast<uint32_t>((hi >> (4u * r)) & 0xFu);
                            if (reg == 4u || reg == 5u)
                                ++stats.vertices;
                            continue;
                        }
                        const uint32_t reg = static_cast<uint32_t>((hi >> (4u * r)) & 0xFu);
                        uint32_t wa[4], wb[4];
                        for (uint32_t i = 0; i < 4u; ++i)
                        {
                            wa[i] = word(a, pos + 4u * i);
                            wb[i] = word(b, pos + 4u * i);
                        }
                        const auto absd = [](uint32_t x, uint32_t y) { return x > y ? x - y : y - x; };
                        out.valueDiff = true;
                        if (reg == 4u || reg == 5u)
                        {
                            ++stats.vertices;
                            if (((wa[3] ^ wb[3]) & 0x8000u) != 0u)
                            {
                                out.structural = true;
                                out.reason = "ADC (draw/no-draw) bit";
                                return;
                            }
                            const uint32_t dx = absd(wa[0] & 0xFFFFu, wb[0] & 0xFFFFu), dy = absd(wa[1] & 0xFFFFu, wb[1] & 0xFFFFu);
                            if (dx || dy)
                            {
                                ++stats.xyWords;
                                stats.maxXy = std::max({stats.maxXy, dx, dy});
                                ++stats.xyHist[bucket(std::max(dx, dy))];
                            }
                            const uint32_t dz = reg == 5u ? absd(wa[2], wb[2]) : absd((wa[2] >> 4) & 0xFFFFFFu, (wb[2] >> 4) & 0xFFFFFFu);
                            if (dz)
                            {
                                ++stats.zWords;
                                stats.maxZ = std::max(stats.maxZ, dz);
                                ++stats.zHist[bucket(dz)];
                            }
                            if (reg == 4u)
                            {
                                const uint32_t df = absd((wa[3] >> 4) & 0xFFu, (wb[3] >> 4) & 0xFFu);
                                if (df)
                                {
                                    ++stats.fogWords;
                                    stats.maxFog = std::max(stats.maxFog, df);
                                }
                            }
                            if (((wa[0] ^ wb[0]) & 0xFFFF0000u) || ((wa[1] ^ wb[1]) & 0xFFFF0000u) ||
                                ((wa[3] ^ wb[3]) & ~(reg == 4u ? 0xFF0u : 0u)))
                            {
                                out.structural = true;
                                out.reason = "XYZ reserved bits";
                                return;
                            }
                        }
                        else if (reg == 1u)
                        {
                            ++stats.colorWords;
                            for (uint32_t i = 0; i < 4u; ++i)
                                stats.maxColor = std::max(stats.maxColor, absd(wa[i] & 0xFFu, wb[i] & 0xFFu));
                        }
                        else if (reg == 2u)
                        {
                            ++stats.stWords;
                            uint32_t worst = 0;
                            for (uint32_t i = 0; i < 3u; ++i)
                                worst = std::max(worst, ulpDistance(wa[i], wb[i]));
                            stats.maxStUlp = std::max(stats.maxStUlp, worst);
                            ++stats.stHist[bucket(worst)];
                        }
                        else if (reg == 0xAu)
                        {
                            ++stats.fogWords;
                            stats.maxFog = std::max(stats.maxFog, absd((wa[3] >> 24) & 0xFFu, (wb[3] >> 24) & 0xFFu));
                        }
                        else
                        {
                            out.structural = true;
                            out.reason = "packed register " + std::to_string(reg);
                            return;
                        }
                    }
            }
            else
            {
                const size_t bytes = flg == 1u ? ((static_cast<size_t>(nloop) * nreg * 8u + 15u) & ~size_t{15u}) : static_cast<size_t>(nloop) * 16u;
                const size_t end = std::min(a.size(), pos + bytes);
                if (std::memcmp(a.data() + pos, b.data() + pos, end - pos) != 0)
                {
                    out.structural = true;
                    out.reason = flg == 1u ? "reglist data" : "image data";
                    return;
                }
                pos = end;
            }
            if (eop)
                break;
        }
    }
}

namespace
{
    std::string hex(uint32_t value)
    {
        char text[16];
        std::snprintf(text, sizeof(text), "%04x", value);
        return text;
    }

    void compareRun(VU1Interpreter &vu, std::vector<uint8_t> &image, std::vector<uint8_t> &work, GS &gs, uint32_t maxCycles,
                    A::CompiledProgram compiled, CompareStats &stats, uint64_t startCycle)
    {
        static std::unique_ptr<VU1Interpreter> shadow;
        static std::vector<uint8_t> shadowWork;
        if (!shadow)
            shadow = std::make_unique<VU1Interpreter>(VU1Interpreter::Unit::VU1);
        *shadow = vu;
        shadowWork = work;
        const uint32_t startPc = vu.state().pc;
        vu1_impl::Vu1PacketSink realSink, shadowSink;
        realSink.forward = shadowSink.forward = false;
        vu1_impl::packetSink = &realSink;
        vu1NormShadow.valid = false;
        const bool realEnded = A::runLoopWith(vu, image.data(), 0x4000u, work.data(), 0x4000u, gs, nullptr, maxCycles, compiled);
        vu1_impl::packetSink = &shadowSink;
        vu1NormShadow.valid = false;
        const bool shadowEnded = A::runLoopWith(*shadow, image.data(), 0x4000u, shadowWork.data(), 0x4000u, gs, nullptr, maxCycles, nullptr);
        vu1_impl::packetSink = nullptr;
        vu1NormShadow.valid = false;
        ++stats.runs;
        static const bool cycleLog = std::getenv("BENCH_CYCLE_LOG") != nullptr;
        if (cycleLog)
            std::fprintf(stderr, "[cyc] %04x %llu %llu\n", startPc, static_cast<unsigned long long>(vu.state().cycles - startCycle),
                         static_cast<unsigned long long>(shadow->state().cycles - startCycle));

        std::string control;
        const auto flagControl = [&](const std::string &why)
        {
            if (control.empty())
                control = why;
        };
        const VU1State a = A::effectiveState(vu);
        const VU1State b = A::effectiveState(*shadow);
        bool values = false;
        const bool flags = a.mac != b.mac || a.status != b.status;
        for (uint32_t reg = 0; reg < 16u; ++reg)
            if (a.vi[reg] != b.vi[reg])
                flagControl("vi" + std::to_string(reg));
        if (a.pc != b.pc || realEnded != shadowEnded)
            flagControl("pc");
        if (a.clip != b.clip)
            flagControl("clip flags");
        for (uint32_t reg = 0; reg < 32u; ++reg)
            for (uint32_t lane = 0; lane < 4u; ++lane)
            {
                uint32_t x, y;
                std::memcpy(&x, &a.vf[reg][lane], 4u);
                std::memcpy(&y, &b.vf[reg][lane], 4u);
                if (x != y)
                {
                    values = true;
                    ++stats.vfLanes;
                    stats.maxVfUlp = std::max(stats.maxVfUlp, ulpDistance(x, y));
                }
            }
        if (std::memcmp(a.acc, b.acc, sizeof(a.acc)) != 0 || std::memcmp(&a.q, &b.q, 4u) != 0)
            values = true;
        for (uint32_t offset = 0; offset < 0x4000u; offset += 4u)
        {
            uint32_t x, y;
            std::memcpy(&x, work.data() + offset, 4u);
            std::memcpy(&y, shadowWork.data() + offset, 4u);
            if (x != y)
            {
                values = true;
                ++stats.memWords;
                stats.maxMemUlp = std::max(stats.maxMemUlp, ulpDistance(x, y));
            }
        }
        if (realSink.packets.size() != shadowSink.packets.size())
            flagControl("packet count");
        else
            for (size_t i = 0; i < realSink.packets.size(); ++i)
            {
                PacketCompare pc;
                comparePacket(realSink.packets[i], shadowSink.packets[i], stats, pc);
                if (pc.structural)
                    flagControl(pc.reason + " (packet " + std::to_string(i + 1) + " of " + std::to_string(realSink.packets.size()) +
                                ", start pc " + hex(startPc) + ")");
                values |= pc.valueDiff;
            }
        if (!control.empty())
        {
            ++stats.control;
            ++stats.controlReasons[control];
            if (stats.examples++ < 10)
                std::printf("  control-flow difference: run %llu, end pc %04x: %s\n", static_cast<unsigned long long>(stats.runs), b.pc,
                            control.c_str());
        }
        else if (values)
            ++stats.valueOnly;
        else if (flags)
            ++stats.flagsOnly;
        else
            ++stats.identical;
    }

    void printCompare(const CompareStats &s)
    {
        std::printf("compare: %llu runs: %llu identical, %llu MAC/status only, %llu value-level, %llu control-flow\n",
                    static_cast<unsigned long long>(s.runs), static_cast<unsigned long long>(s.identical),
                    static_cast<unsigned long long>(s.flagsOnly), static_cast<unsigned long long>(s.valueOnly),
                    static_cast<unsigned long long>(s.control));
        for (const auto &[why, count] : s.controlReasons)
            std::printf("  control-flow: %s x%llu\n", why.c_str(), static_cast<unsigned long long>(count));
        std::printf("GS vertices checked: %llu\n", static_cast<unsigned long long>(s.vertices));
        const auto hist = [](const char *name, const std::map<uint32_t, uint64_t> &h)
        {
            std::printf("  %s:", name);
            for (const auto &[bucket, count] : h)
                std::printf(" <=%u:%llu", bucket, static_cast<unsigned long long>(count));
            std::printf("\n");
        };
        std::printf("  XY differs in %llu vertices, max %u/16 px\n", static_cast<unsigned long long>(s.xyWords), s.maxXy);
        hist("XY delta (1/16 px)", s.xyHist);
        std::printf("  Z differs in %llu vertices, max %u units\n", static_cast<unsigned long long>(s.zWords), s.maxZ);
        hist("Z delta", s.zHist);
        std::printf("  colour differs in %llu, max %u/255; fog differs in %llu, max %u/255\n", static_cast<unsigned long long>(s.colorWords),
                    s.maxColor, static_cast<unsigned long long>(s.fogWords), s.maxFog);
        std::printf("  STQ differs in %llu, max %u ulp\n", static_cast<unsigned long long>(s.stWords), s.maxStUlp);
        hist("STQ delta (ulp)", s.stHist);
        std::printf("  VU memory words differing %llu (max %u ulp), VF lanes differing %llu (max %u ulp)\n",
                    static_cast<unsigned long long>(s.memWords), s.maxMemUlp, static_cast<unsigned long long>(s.vfLanes), s.maxVfUlp);
    }
}

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: sotc_vu1_bench <trace.bin> <image-dir> [--verify] [--compare] [--interp] [--repeat N] [--tsc] [--only PC] [--profile]\n"
                             "  --compare: tolerance comparison against the interpreter (flags / GS values / control flow)\n"
                             "  PS2X_VU1_EXACT=1: exact native kernels instead of relaxed; BENCH_NO_NATIVE=1: no native kernels\n");
        return 2;
    }
    const std::string tracePath = argv[1];
    const std::string imageDir = argv[2];
    bool verify = false;
    bool compare = false;
    bool interp = false;
    bool profile = false;
    bool cpuCycles = false;
    bool tsc = false; // time each run with RDTSC (QueryThreadCycleTime is a kernel call that dwarfs short runs)
    uint32_t onlyPc = ~0u;
    int repeat = 1;
    for (int i = 3; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--verify")
            verify = true;
        else if (arg == "--compare")
            compare = true;
        else if (arg == "--interp")
            interp = true;
        else if (arg == "--profile")
            profile = true;
        else if (arg == "--cpu-cycles")
            cpuCycles = true;
        else if (arg == "--tsc")
            tsc = true;
        else if (arg == "--only" && i + 1 < argc)
            onlyPc = static_cast<uint32_t>(std::stoul(argv[++i], nullptr, 16));
        else if (arg == "--repeat" && i + 1 < argc)
            repeat = std::atoi(argv[++i]);
    }
    registerGeneratedVu1Programs();
    if (!std::getenv("BENCH_NO_NATIVE"))
        registerSotcVu1NativeKernels();
    if (std::getenv("BENCH_RZ"))
        _mm_setcsr(_mm_getcsr() | 0x6000u);

    const std::vector<uint8_t> buffer = readFile(tracePath);
    std::vector<Record> records;
    for (size_t pos = 0; pos + 24u <= buffer.size();)
    {
        if (readAt<uint32_t>(buffer, pos) != 0x52543156u)
        {
            std::fprintf(stderr, "bad record at %zu\n", pos - 4u);
            return 1;
        }
        Record record;
        record.maxCycles = readAt<uint32_t>(buffer, pos);
        record.hash = readAt<uint64_t>(buffer, pos);
        record.tick = readAt<uint64_t>(buffer, pos);
        record.state = readAt<A::TraceState>(buffer, pos);
        record.diff = pos;
        for (;;)
        {
            const uint16_t index = readAt<uint16_t>(buffer, pos);
            const uint16_t count = readAt<uint16_t>(buffer, pos);
            if (index == 0xFFFFu && count == 0u)
                break;
            pos += count * 16u;
        }
        records.push_back(record);
    }
    if (records.empty())
    {
        std::fprintf(stderr, "no records\n");
        return 1;
    }

    std::map<uint64_t, std::vector<uint8_t>> images;
    std::map<uint64_t, A::CompiledProgram> programs;
    for (const Record &record : records)
    {
        if (images.count(record.hash))
            continue;
        char name[64];
        std::snprintf(name, sizeof(name), "/vu1_%016" PRIx64 ".bin", record.hash);
        images[record.hash] = readFile(imageDir + name);
        const std::vector<uint8_t> &image = images[record.hash];
        programs[record.hash] = image.size() == 0x4000u
                                    ? findCompiledVu1ProgramForImage(image.data(), static_cast<uint32_t>(image.size()))
                                    : findCompiledVu1ProgramByHash(record.hash);
        std::printf("image %016" PRIx64 ": %s, %s\n", record.hash, images[record.hash].size() == 0x4000u ? "loaded" : "MISSING",
                    programs[record.hash] ? "compiled" : "not compiled");
    }

    auto vu = std::make_unique<VU1Interpreter>(VU1Interpreter::Unit::VU1);
    GS gs;
    std::vector<uint8_t> current(0x4000u, 0u);
    std::vector<uint8_t> work(0x4000u, 0u);
    vu1_impl::Vu1PacketSink sink;
    sink.forward = false;
    std::map<uint64_t, double> imageSeconds;
    std::map<std::pair<uint64_t, uint32_t>, std::pair<double, uint64_t>> entryStats;
    std::map<std::pair<uint64_t, uint32_t>, uint64_t> entryRuns;
    CompareStats cmp;
    double total = 0.0;
    uint64_t mismatches = 0;
    uint64_t runs = 0;
    uint64_t vuCycles = 0;
    uint64_t threadCycles = 0;
    uint64_t tscCycles = 0;
    VU1State lastRegs{};
    bool haveLast = false;
    Sampler sampler;
    HANDLE self = nullptr;
    if (profile)
    {
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &self, 0, FALSE, DUPLICATE_SAME_ACCESS);
        sampler.start(self);
    }
    for (int pass = 0; pass < repeat; ++pass)
    {
        std::fill(current.begin(), current.end(), 0u);
        for (const Record &record : records)
        {
            applyDiff(buffer, record.diff, current.data());
            std::vector<uint8_t> &image = images[record.hash];
            if (image.size() != 0x4000u)
                continue;
            if (onlyPc != ~0u && record.state.state.pc != onlyPc)
                continue;
            std::memcpy(work.data(), current.data(), work.size());
            A::traceRestore(*vu, record.state);
            if (verify || !haveLast || std::memcmp(lastRegs.vf, record.state.state.vf, sizeof(lastRegs.vf)) != 0 ||
                std::memcmp(lastRegs.acc, record.state.state.acc, sizeof(lastRegs.acc)) != 0)
                vu1NormShadow.valid = false;
            const A::CompiledProgram compiled = interp ? nullptr : programs[record.hash];
            ++runs;
            if (compare)
            {
                compareRun(*vu, image, work, gs, record.maxCycles, compiled, cmp, record.state.cycle);
                continue;
            }
            if (verify)
            {
                if (!A::runVerifiedWith(*vu, image.data(), 0x4000u, work.data(), 0x4000u, gs, nullptr, record.maxCycles, compiled, false))
                    ++mismatches;
                continue;
            }
            vu1_impl::packetSink = &sink;
            const uint64_t cycleBefore = record.state.cycle;
            sampler.active = true;
            ULONG64 threadBefore = 0;
            if (cpuCycles && !QueryThreadCycleTime(GetCurrentThread(), &threadBefore))
                return 1;
            const uint64_t tscBefore = tsc ? __rdtsc() : 0u;
            const auto start = std::chrono::steady_clock::now();
            A::runLoopWith(*vu, image.data(), 0x4000u, work.data(), 0x4000u, gs, nullptr, record.maxCycles, compiled);
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            if (tsc)
                tscCycles += __rdtsc() - tscBefore;
            ULONG64 threadAfter = 0;
            if (cpuCycles && !QueryThreadCycleTime(GetCurrentThread(), &threadAfter))
                return 1;
            threadCycles += threadAfter - threadBefore;
            sampler.active = false;
            vu1_impl::packetSink = nullptr;
            sink.packets.clear();
            total += seconds;
            imageSeconds[record.hash] += seconds;
            vuCycles += vu->state().cycles - cycleBefore;
            lastRegs = vu->state();
            haveLast = true;
            auto &entry = entryStats[{record.hash, record.state.state.pc}];
            entry.first += seconds;
            entry.second += vu->state().cycles - cycleBefore;
            ++entryRuns[{record.hash, record.state.state.pc}];
        }
    }
    if (profile)
    {
        sampler.stop();
        sampler.report();
    }
    const double fields = static_cast<double>(records.back().tick - records.front().tick + 1u);
    std::printf("%zu records over %.0f fields, %llu runs\n", records.size(), fields, static_cast<unsigned long long>(runs));
    if (compare)
    {
        printCompare(cmp);
        return 0;
    }
    if (verify)
    {
        std::printf("verify: %llu mismatches\n", static_cast<unsigned long long>(mismatches));
        printVu1VerifySummary();
        return mismatches == 0u ? 0 : 1;
    }
    std::printf("total %.3f ms, %.3f ms per field, %.2f M VU cycles per field, %.2f ns per VU cycle\n", total * 1000.0 / repeat,
                total * 1000.0 / repeat / fields, vuCycles / 1e6 / repeat / fields, total * 1e9 / std::max<uint64_t>(vuCycles, 1u));
    if (cpuCycles)
        std::printf("CPU %.3f M thread cycles per field\n", threadCycles / 1e6 / repeat / fields);
    if (tsc)
        std::printf("TSC %.3f M cycles per field, %.0f per run\n", tscCycles / 1e6 / repeat / fields,
                    static_cast<double>(tscCycles) / static_cast<double>(std::max<uint64_t>(runs, 1u)));
    for (const auto &[hash, seconds] : imageSeconds)
        std::printf("  %016" PRIx64 " %.3f ms per field\n", hash, seconds * 1000.0 / repeat / fields);
    std::vector<std::pair<double, std::pair<uint64_t, uint32_t>>> entries;
    for (const auto &[key, value] : entryStats)
        entries.emplace_back(value.first, key);
    std::sort(entries.rbegin(), entries.rend());
    for (size_t i = 0; i < std::min<size_t>(entries.size(), 30u); ++i)
    {
        const auto &key = entries[i].second;
        const auto &value = entryStats[key];
        std::printf("  entry %016" PRIx64 ":%04x %.3f ms/field, %.2f M cycles/field, %llu runs/field, %.2f ns/cycle\n", key.first, key.second,
                    value.first * 1000.0 / repeat / fields, value.second / 1e6 / repeat / fields,
                    static_cast<unsigned long long>(entryRuns[key] / repeat / static_cast<uint64_t>(fields)),
                    value.first * 1e9 / std::max<uint64_t>(value.second, 1u));
    }
    return 0;
}
