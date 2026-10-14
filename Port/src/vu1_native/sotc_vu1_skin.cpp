// Native implementation of SotC VU1 program 05's skinned strip routine (0x10E0).
//
// Program 05 (0xEEC74A44BC46A973, 0x3BE8 bytes) draws skinned character meshes. Routine 0x10E0
// transforms one strip: per vertex five input quadwords (position, normal (itof12), bone word,
// ST (itof12), colour (itof0)). The bone word packs per-bone "index + weight" floats in xyz and the
// bone count as an integer in w; each bone's 4x4 matrix lives at VU address index*4. Position and
// normal are blended over the bones (vf23/vf24), written back into the input slots (the clipping
// path reads them), transformed to the screen through vf01-vf04 with a perspective divide, lit
// (normal through vf13-vf16, clamped, light colours vf09-vf12 from 988-991, times the vertex
// colour, clamped to 255), fogged through vf17, clip-tested through vf05-vf08 and classified per
// triangle like program 00's strip kernel.
//
// The routine is one software pipeline: the bone loop (0x1208-0x1260) and the vertex loop share
// the entry 0x1208, and each pass preloads the next vertex. This kernel transcribes it pair by pair
// in program order. Within a pair both halves read their sources before either writes; across
// pairs the VU interlocks on VF/VI hazards, every Q read follows WAITQ, and FCAND/FCOR come at least
// 4 cycles after the CLIPW they observe, so program order gives the architectural results.
//
// Arithmetic is relaxed: plain SSE under round toward zero with FTZ/DAZ (see sotc_vu1_native.cpp).
// MAC/status are rebuilt from the last FMAC; no flag is read before the hand-off and the clipping
// path computes its own before FSAND. With PS2X_VU1_EXACT=1 the kernel is not registered.
//
// Hand-off: when no triangle needs clipping, the kernel runs through 0x14E0 and resumes the
// compiled code at 0x14E8 (reload, XGKICK, return); otherwise it resumes at 0x14C8 and the compiled
// code runs the clipping path. Verification: sotc_vu1_bench --compare.

#include "ps2_vu1_impl.inl"

#include <cstdint>
#include <cstring>

using VuNativeProgram = VU1InterpreterAccess::CompiledProgram;
void registerVu1NativeEntry(uint64_t programHash, uint32_t pc, VuNativeProgram native);

namespace
{
    constexpr uint64_t kSkinStripKey = 0x50C0'4E41'5449'10E0ull;
    constexpr uint64_t kProgram05 = 0xEEC74A44BC46A973ull;
    constexpr unsigned kRelaxedCsrBits = 0x6000u | 0x8000u | 0x0040u;

    struct RelaxedCsr
    {
        unsigned saved;
        RelaxedCsr() : saved(_mm_getcsr())
        {
            if ((saved & kRelaxedCsrBits) != kRelaxedCsrBits)
                _mm_setcsr(saved | kRelaxedCsrBits);
        }
        ~RelaxedCsr()
        {
            if ((saved & kRelaxedCsrBits) != kRelaxedCsrBits)
                _mm_setcsr(saved);
        }
    };

    template <uint32_t Lane>
    VU1_FORCE_INLINE __m128 bc(__m128 value)
    {
        return _mm_shuffle_ps(value, value, _MM_SHUFFLE(Lane, Lane, Lane, Lane));
    }

    // mr32: (x, y, z, w) <- (y, z, w, x)
    VU1_FORCE_INLINE __m128 rot(__m128 value)
    {
        return _mm_shuffle_ps(value, value, _MM_SHUFFLE(0, 3, 2, 1));
    }

    VU1_FORCE_INLINE int32_t i16(int32_t value)
    {
        return static_cast<int16_t>(value);
    }

    VU1_FORCE_INLINE int32_t laneX16(__m128 value)
    {
        return i16(static_cast<int32_t>(static_cast<uint32_t>(_mm_cvtsi128_si32(_mm_castps_si128(value))) & 0xFFFFu));
    }

    VU1_FORCE_INLINE int32_t laneW16(__m128 value)
    {
        return i16(static_cast<int32_t>(static_cast<uint32_t>(_mm_extract_epi32(_mm_castps_si128(value), 3)) & 0xFFFFu));
    }
}

template <>
struct VU1InterpreterAccess::Native<kSkinStripKey>
{
    static StepResult run(VU1Interpreter &vu, const Vu1RunArgs &args);
};

VU1InterpreterAccess::StepResult VU1InterpreterAccess::Native<kSkinStripKey>::run(VU1Interpreter &vu, const Vu1RunArgs &args)
{
    // Entries: 0x0598 "bal vi14, 0x10e0 ; nop", or 0x0920 / 0x09B8 "iaddiu vi09, 0x3f4 ; lq vf26, 1008 ;
    // lq vf17, 976 ; xgkick vi09 ; bal vi14, 0x10e0 ; sq vf26, 928" (delay slot).
    if (vu.m_state.branchPending || vu.m_state.ebit || vu.m_state.haltAfterDelaySlot || vu.m_stopRequested ||
        args.dataSize != kFastMemSize || !args.vuData)
        return StepResult::Fallback;
    if ((vu.m_flagValid | vu.m_vfWriteValid | vu.m_viWriteValid | vu.m_accWriteValid | vu.m_storeValid) != 0u ||
        vu.m_xgkick.active || vu.m_fdiv.valid || vu.m_efu[0].valid || vu.m_efu[1].valid)
        return StepResult::Fallback;

    uint8_t *const mem = args.vuData;
    const uint32_t entryPc = vu.m_state.pc;
    const bool kickEntry = entryPc != 0x0598u;
    const uint32_t balPc = kickEntry ? entryPc + 0x20u : entryPc;
    const uint64_t entryCycle = vu.m_cycle;

    // Pre-scan the strip: vertex count and bone counts (the bone loop needs 1-4 bones).
    const int32_t top = static_cast<int16_t>(vu.m_state.top & 0x3FFu);
    const uint32_t count = ldw(mem, qaddr(top, 0)) & 0x7FFFu;
    if (count == 0u || count > 200u)
        return StepResult::Fallback;
    uint64_t boneIterations = 0u;
    for (uint32_t v = 0; v < count; ++v)
    {
        const int32_t bones = i16(static_cast<int32_t>(ldw(mem, qaddr(top, static_cast<int32_t>(1u + 5u * v + 4u)) + 12u) & 0xFFFFu));
        if (bones < 1 || bones > 4)
            return StepResult::Fallback;
        boneIterations += static_cast<uint64_t>(bones);
    }
    // Cycle model of the interpreter's pipeline timing, fitted over 808 gameplay runs (total error
    // 0.2%). The trace has no FCOR plane tests or clipped triangles; those two use program 00's costs
    // for the same instruction sequence.
    constexpr uint64_t kCyclesFixed = 53u;
    constexpr uint64_t kCyclesVertex = 55u;
    constexpr uint64_t kCyclesBone = 12u;
    constexpr uint64_t kCyclesCheckedVertex = 5u;
    constexpr uint64_t kCyclesPlaneTest = 3u;
    constexpr uint64_t kCyclesNeedsClip = 2u;
    constexpr uint64_t kCyclesHandOff = 4u;
    const uint64_t worst = kCyclesFixed + (kCyclesVertex + kCyclesCheckedVertex + 5u * kCyclesPlaneTest + kCyclesNeedsClip) * count +
                           kCyclesBone * boneIterations + 64u;
    if (vu.m_cycle + worst > args.budgetEnd)
        return StepResult::Fallback;

    [[maybe_unused]] RelaxedCsr csr;
    __m128 vf[32];
    for (uint32_t reg = 0; reg < 32u; ++reg)
        vf[reg] = _mm_loadu_ps(vu.m_state.vf[reg]);
    vf[0] = kVf0();
    __m128 acc = _mm_loadu_ps(vu.m_state.acc);
    int32_t vi[16];
    for (uint32_t reg = 0; reg < 16u; ++reg)
        vi[reg] = vu.m_state.vi[reg];
    vi[0] = 0;
    float q = vu.m_state.q;
    float iReg = vu.m_state.i;
    uint32_t clip = vu.m_workingClip & 0xFFFFFFu;
    uint32_t lastDi = 0u, orDi = 0u;
    uint64_t checkedVertices = 0u, planeTests = 0u, needsClip = 0u;

    const auto lq = [&](int32_t base, int32_t offset) [[msvc::forceinline]] { return ldq(mem, qaddr(base, offset)); };
    const auto sq = [&](__m128 value, int32_t base, int32_t offset) [[msvc::forceinline]]
    { _mm_storeu_ps(reinterpret_cast<float *>(mem + qaddr(base, offset)), value); };
    const auto iswW = [&](int32_t value, int32_t base, int32_t offset) [[msvc::forceinline]]
    {
        const uint32_t word = static_cast<uint32_t>(static_cast<uint16_t>(value & 0xFFFF));
        std::memcpy(mem + qaddr(base, offset) + 12u, &word, 4u);
    };
    const auto blend = [](__m128 old, __m128 value, uint32_t dest) [[msvc::forceinline]] { return blendDest(old, value, dest); };
    const auto fma = [](__m128 a, __m128 s, __m128 t) [[msvc::forceinline]] { return _mm_add_ps(a, _mm_mul_ps(s, t)); };
    const auto divide = [&](float den) [[msvc::forceinline]]
    {
        // div q, vf00w, den: 1 / den; a (DAZ) zero divisor saturates and sets D.
        if (den == 0.0f) [[unlikely]]
        {
            lastDi = 0x20u;
            q = std::signbit(den) ? -std::numeric_limits<float>::max() : std::numeric_limits<float>::max();
        }
        else
        {
            lastDi = 0u;
            q = 1.0f / den;
        }
        orDi |= lastDi;
    };
    const auto clipw = [&](__m128 value) [[msvc::forceinline]]
    {
        const uint32_t wBits = static_cast<uint32_t>(_mm_extract_ps(value, 3));
        const int32_t limit = (wBits & 0x7F800000u) != 0u ? static_cast<int32_t>(wBits & 0x7FFFFFFFu) : 0x007FFFFF;
        const __m128i bits = _mm_castps_si128(value);
        const __m128i bounds = _mm_set1_epi32(limit);
        const uint32_t positive = static_cast<uint32_t>(_mm_movemask_ps(_mm_castsi128_ps(_mm_cmpgt_epi32(bits, bounds)))) & 7u;
        const uint32_t negative = static_cast<uint32_t>(_mm_movemask_ps(_mm_castsi128_ps(
                                      _mm_cmpgt_epi32(_mm_xor_si128(bits, _mm_set1_epi32(static_cast<int32_t>(0x80000000u))), bounds)))) & 7u;
        const uint32_t plus = (positive & 1u) | ((positive & 2u) << 1u) | ((positive & 4u) << 2u);
        const uint32_t minus = ((negative & 1u) << 1u) | ((negative & 2u) << 2u) | ((negative & 4u) << 3u);
        clip = ((clip << 6) | plus | minus) & 0xFFFFFFu;
    };
    const auto xgkick = [&](int32_t address)
    {
        constexpr uint64_t kAll = std::numeric_limits<uint64_t>::max() / 2u;
        vu.startXgkick(static_cast<uint32_t>(static_cast<uint16_t>(address)));
        xgkickAdvance(vu, kAll);
    };
    __m128 lastFmac = _mm_setzero_ps(); // for MAC/status at the hand-off (13D8 maddw vf31)

    // --- entry ---
    if (kickEntry)
    {
        vi[9] = 0x3F4;                // iaddiu vi09, vi00, 0x3f4
        vf[26] = lq(0, 1008);         // lq vf26, 1008(vi00)
        vf[17] = lq(0, 976);          // lq vf17, 976(vi00)
        xgkick(vi[9]);                // xgkick vi09
    }
    vi[14] = static_cast<int32_t>((balPc + 16u) / 8u); // bal vi14, 0x10e0
    if (kickEntry)
        sq(vf[26], 0, 928);           // delay slot: sq vf26, 928(vi00)

    // --- 10E0-1200: prologue (first vertex, first bone) ---
    vi[4] = top;                                                         // xtop vi04
    vf[13] = blend(vf[13], _mm_mul_ps(vf[0], bc<0>(vf[0])), 0x1u);        // mulx.w vf13, vf00, vf00x | iaddiu vi10, 0x7fff
    vi[10] = 0x7FFF;
    vf[14] = blend(vf[14], _mm_mul_ps(vf[0], bc<0>(vf[0])), 0x1u);        // mulx.w vf14 | iaddiu vi11, vi04, 0
    vi[11] = vi[4];
    vf[15] = blend(vf[15], _mm_mul_ps(vf[0], bc<0>(vf[0])), 0x1u);        // mulx.w vf15 | ilwr.x vi03, (vi04)
    vi[3] = i16(static_cast<int32_t>(ldw(mem, qaddr(vi[4], 0)) & 0xFFFFu));
    vf[16] = blend(vf[16], _mm_add_ps(vf[0], bc<0>(vf[0])), 0x1u);        // addx.w vf16 | ilwr.w vi06, (vi04)
    vi[6] = i16(static_cast<int32_t>(ldw(mem, qaddr(vi[4], 0) + 12u) & 0xFFFFu));
    vi[2] = i16(vi[10] + 1);                                              // iaddiu vi02, vi10, 1
    vf[26] = lq(vi[4], 0);                                                // lqi vf26, (vi04++)
    vi[4] = i16(vi[4] + 1);
    vi[3] = i16(vi[10] & vi[3]);                                          // iand vi03, vi10, vi03
    vi[5] = vi[6];                                                        // iaddiu vi05, vi06, 0
    vi[8] = 0;                                                            // iaddiu vi08, vi00, 0
    vi[10] = 2;                                                           // iaddi vi10, vi00, 2
    vf[29] = blend(vf[29], rot(vf[17]), 0x1u);                            // mr32.w vf29, vf17
    sq(vf[26], vi[5], 0);                                                 // sqi vf26, (vi05++)
    vi[5] = i16(vi[5] + 1);
    vf[22] = lq(vi[4], 4);                                                // lq vf22, 4(vi04)
    vf[19] = lq(vi[4], 1);                                                // lq vf19, 1(vi04)
    vf[18] = lq(vi[4], 0);                                                // lq vf18, 0(vi04)
    vf[21] = lq(vi[4], 2);                                                // lq vf21, 2(vi04)
    vf[26] = ftoi<0u>(vf[22]);                                            // ftoi0 vf26, vf22 | lq vf20, 3(vi04)
    vf[20] = lq(vi[4], 3);
    vf[19] = itof<12u>(vf[19]);                                           // itof12 vf19, vf19
    vf[23] = vf[0];                                                       // move vf23, vf00
    vf[24] = vf[0];                                                       // move vf24, vf00
    vf[27] = itof<0u>(vf[26]);                                            // itof0 vf27, vf26 | mtir vi09, vf26x
    vi[9] = laneX16(vf[26]);
    vf[20] = itof<0u>(vf[20]);                                            // itof0 vf20, vf20 | mr32 vf26, vf26
    vf[26] = rot(vf[26]);
    vi[12] = laneW16(vf[22]);                                             // mtir vi12, vf22w
    vf[22] = _mm_sub_ps(vf[22], vf[27]);                                  // sub vf22, vf22, vf27 | iadd vi09 x2
    vi[9] = i16(vi[9] + vi[9]);
    vi[9] = i16(vi[9] + vi[9]);                                           // iadd vi09, vi09, vi09
    vf[27] = lq(vi[9], 0);                                                // lq vf27-vf30, 0-3(vi09)
    vf[28] = lq(vi[9], 1);
    vf[29] = lq(vi[9], 2);
    vf[30] = lq(vi[9], 3);
    vi[9] = laneX16(vf[26]);                                              // mtir vi09, vf26x
    vf[26] = rot(vf[26]);                                                 // mr32 vf26, vf26
    acc = _mm_mul_ps(vf[27], bc<0>(vf[18]));                              // mulax acc, vf27, vf18x
    acc = fma(acc, vf[28], bc<1>(vf[18]));                                // madday
    acc = fma(acc, vf[29], bc<2>(vf[18]));                                // maddaz | iadd vi09 x2
    vi[9] = i16(vi[9] + vi[9]);
    vf[31] = fma(acc, vf[30], bc<3>(vf[0]));                              // maddw vf31, vf30, vf00w | iadd
    vi[9] = i16(vi[9] + vi[9]);

    for (;;)
    {
        // --- 1208-1260: one bone ---
        for (;;)
        {
            acc = _mm_mul_ps(vf[27], bc<0>(vf[19]));                      // 1208 mulax acc, vf27, vf19x | lq vf30, 3(vi09)
            vf[30] = lq(vi[9], 3);
            acc = fma(acc, vf[28], bc<1>(vf[19]));                        // 1210 madday | lq vf27, 0(vi09)
            vf[27] = lq(vi[9], 0);
            acc = fma(acc, vf[29], bc<2>(vf[19]));                        // 1218 maddaz | lq vf28, 1(vi09)
            vf[28] = lq(vi[9], 1);
            vf[25] = fma(acc, vf[0], bc<3>(vf[19]));                      // 1220 maddw vf25, vf00, vf19w | lq vf29, 2(vi09)
            vf[29] = lq(vi[9], 2);
            vf[31] = blend(vf[31], _mm_mul_ps(vf[31], bc<0>(vf[22])), 0xEu); // 1228 mulx.xyz vf31, vf31, vf22x | mtir vi09, vf26x
            vi[9] = laneX16(vf[26]);
            acc = _mm_mul_ps(vf[27], bc<0>(vf[18]));                      // 1230 mulax acc, vf27, vf18x
            acc = fma(acc, vf[28], bc<1>(vf[18]));                        // 1238 madday | mr32 vf26, vf26
            vf[26] = rot(vf[26]);
            vf[25] = blend(vf[25], _mm_mul_ps(vf[25], bc<0>(vf[22])), 0xEu); // 1240 mulx.xyz vf25, vf25, vf22x | iaddi vi12, -1
            vi[12] = i16(vi[12] - 1);
            vf[23] = _mm_add_ps(vf[23], vf[31]);                          // 1248 add vf23, vf23, vf31 | mr32 vf22, vf22
            vf[22] = rot(vf[22]);
            acc = fma(acc, vf[29], bc<2>(vf[18]));                        // 1250 maddaz | iadd vi09
            vi[9] = i16(vi[9] + vi[9]);
            vf[31] = fma(acc, vf[30], bc<3>(vf[0]));                      // 1258 maddw vf31, vf30, vf00w | ibne vi12, 0x1208
            const bool moreBones = vi[12] != 0;
            vf[24] = blend(vf[24], _mm_add_ps(vf[24], vf[25]), 0xEu);     // 1260 add.xyz vf24, vf24, vf25 | iadd vi09 (delay slot)
            vi[9] = i16(vi[9] + vi[9]);
            if (!moreBones)
                break;
        }

        // --- 1268-13D8: last bone, output, lighting, next vertex's first bone ---
        acc = _mm_mul_ps(vf[27], bc<0>(vf[19]));                          // 1268 mulax | move vf30, vf22
        vf[30] = vf[22];
        acc = fma(acc, vf[28], bc<1>(vf[19]));                            // 1270 madday | lq vf22, 9(vi04)
        vf[22] = lq(vi[4], 9);
        acc = fma(acc, vf[29], bc<2>(vf[19]));                            // 1278 maddaz | iaddiu vi04, 5
        vi[4] = i16(vi[4] + 5);
        vf[25] = fma(acc, vf[0], bc<3>(vf[19]));                          // 1280 maddw vf25, vf00, vf19w | iaddi vi03, -1
        vi[3] = i16(vi[3] - 1);
        vf[31] = blend(vf[31], _mm_mul_ps(vf[31], bc<0>(vf[30])), 0xEu);  // 1288 mulx.xyz vf31, vf31, vf30x | iaddiu vi05, 3
        vi[5] = i16(vi[5] + 3);
        vf[26] = ftoi<0u>(vf[22]);                                        // 1290 ftoi0 vf26, vf22 | lq vf09, 988
        vf[9] = lq(0, 988);
        vf[10] = lq(0, 989);                                              // 1298 lq vf10
        vf[25] = blend(vf[25], _mm_mul_ps(vf[25], bc<0>(vf[30])), 0xEu);  // 12A0 mulx.xyz vf25 | lq vf11
        vf[11] = lq(0, 990);
        vf[23] = _mm_add_ps(vf[23], vf[31]);                              // 12A8 add vf23 | lq vf12
        vf[12] = lq(0, 991);
        vf[30] = itof<0u>(vf[26]);                                        // 12B0 itof0 vf30, vf26 | mtir vi09, vf26x
        vi[9] = laneX16(vf[26]);
        vf[21] = itof<12u>(vf[21]);                                       // 12B8 itof12 vf21 | mr32 vf26
        vf[26] = rot(vf[26]);
        vf[24] = blend(vf[24], _mm_add_ps(vf[24], vf[25]), 0xEu);         // 12C0 add.xyz vf24 | mr32.w vf29, vf17
        vf[29] = blend(vf[29], rot(vf[17]), 0x1u);
        acc = _mm_mul_ps(vf[1], bc<0>(vf[23]));                           // 12C8 mulax acc, vf01, vf23x | iaddi vi10, -1
        vi[10] = i16(vi[10] - 1);
        acc = fma(acc, vf[2], bc<1>(vf[23]));                             // 12D0 madday | sq vf23, -5(vi04)
        sq(vf[23], vi[4], -5);
        acc = fma(acc, vf[3], bc<2>(vf[23]));                             // 12D8 maddaz | move vf25, vf21
        vf[25] = vf[21];
        vf[18] = fma(acc, vf[4], bc<3>(vf[0]));                           // 12E0 maddw vf18, vf04, vf00w | sq vf24, -4(vi04)
        sq(vf[24], vi[4], -4);
        acc = _mm_mul_ps(vf[13], bc<0>(vf[24]));                          // 12E8 mulax acc, vf13, vf24x | lq vf21, 2(vi04)
        vf[21] = lq(vi[4], 2);
        acc = fma(acc, vf[14], bc<1>(vf[24]));                            // 12F0 madday [I] | loi 255
        iReg = std::bit_cast<float>(0x437F0000u);
        acc = fma(acc, vf[15], bc<2>(vf[24]));                            // 12F8 maddaz | sq vf25, -3(vi04)
        sq(vf[25], vi[4], -3);
        {
            const __m128 v24 = fma(acc, vf[16], bc<3>(vf[24]));           // 1300 maddw vf24, vf16, vf24w | div q, vf00w, vf18w
            divide(_mm_cvtss_f32(bc<3>(vf[18])));
            vf[24] = v24;
        }
        acc = _mm_mul_ps(vf[5], bc<0>(vf[23]));                           // 1308 mulax acc, vf05, vf23x | mr32.zw vf25, vf00
        vf[25] = blend(vf[25], rot(vf[0]), 0x3u);
        acc = fma(acc, vf[6], bc<1>(vf[23]));                             // 1310 madday | iadd vi09
        vi[9] = i16(vi[9] + vi[9]);
        acc = fma(acc, vf[7], bc<2>(vf[23]));                             // 1318 maddaz | mtir vi12, vf22w
        vi[12] = laneW16(vf[22]);
        vf[28] = fma(acc, vf[8], bc<3>(vf[0]));                           // 1320 maddw vf28, vf08, vf00w | lq vf19, 1(vi04)
        vf[19] = lq(vi[4], 1);
        vf[24] = _mm_max_ps(vf[24], bc<0>(vf[0]));                        // 1328 maxx vf24, vf24, vf00x | iadd vi09
        vi[9] = i16(vi[9] + vi[9]);
        acc = blend(acc, _mm_mul_ps(vf[17], vf[0]), 0x1u);                // 1330 mula.w acc, vf17, vf00 | mr32 vf26
        vf[26] = rot(vf[26]);
        const __m128 qv = _mm_set1_ps(q);
        vf[18] = blend(vf[18], _mm_mul_ps(vf[18], qv), 0xEu);             // 1338 mulq.xyz vf18, vf18, q | waitq
        clipw(vf[28]);                                                    // 1340 clipw.xyz vf28, vf28w | lq vf27, 0(vi09)
        vf[27] = lq(vi[9], 0);
        vf[18] = blend(vf[18], fma(acc, vf[29], qv), 0x1u);               // 1348 maddq.w vf18, vf29, q | lq vf28, 1(vi09)
        vf[28] = lq(vi[9], 1);
        acc = _mm_mul_ps(vf[9], bc<0>(vf[24]));                           // 1350 mulax acc, vf09, vf24x | lq vf29, 2(vi09)
        vf[29] = lq(vi[9], 2);
        acc = fma(acc, vf[10], bc<1>(vf[24]));                            // 1358 madday | move vf23, vf00
        vf[23] = vf[0];
        acc = fma(acc, vf[11], bc<2>(vf[24]));                            // 1360 maddaz
        vf[24] = fma(acc, vf[12], bc<3>(vf[24]));                         // 1368 maddw vf24, vf12, vf24w
        const __m128 iv = _mm_set1_ps(iReg);
        vf[18] = blend(vf[18], _mm_min_ps(vf[18], iv), 0x1u);             // 1370 minii.w vf18, vf18, i
        vf[25] = blend(vf[25], _mm_mul_ps(vf[25], qv), 0xEu);             // 1378 mulq.xyz vf25, vf25, q | lq vf11, 3(vi04)
        vf[11] = lq(vi[4], 3);
        vf[22] = _mm_sub_ps(vf[22], vf[30]);                              // 1380 sub vf22, vf22, vf30 | lq vf12, 0(vi04)
        vf[12] = lq(vi[4], 0);
        vf[20] = blend(vf[20], _mm_mul_ps(vf[24], vf[20]), 0xEu);         // 1388 mul.xyz vf20, vf24, vf20 | lq vf30, 3(vi09)
        vf[30] = lq(vi[9], 3);
        vf[18] = blend(vf[18], _mm_max_ps(vf[18], bc<0>(vf[0])), 0x1u);   // 1390 maxx.w vf18, vf18, vf00x | mtir vi09, vf26w
        vi[9] = laneW16(vf[26]);
        vf[19] = itof<12u>(vf[19]);                                       // 1398 itof12 vf19 | sq vf25, -3(vi05)
        sq(vf[25], vi[5], -3);
        acc = _mm_mul_ps(vf[27], bc<0>(vf[12]));                          // 13A0 mulax acc, vf27, vf12x | fcand vi01, 0x3ffff
        vi[1] = (clip & 0x3FFFFu) != 0u ? 1 : 0;
        vf[9] = _mm_min_ps(vf[20], iv);                                   // 13A8 minii vf09, vf20, i | move vf24, vf00
        vf[24] = vf[0];
        vf[10] = ftoi<4u>(vf[18]);                                        // 13B0 ftoi4 vf10, vf18 | iadd vi09
        vi[9] = i16(vi[9] + vi[9]);
        vf[20] = itof<0u>(vf[11]);                                        // 13B8 itof0 vf20, vf11 | lq vf18, 0(vi04)
        vf[18] = lq(vi[4], 0);
        acc = fma(acc, vf[28], bc<1>(vf[12]));                            // 13C0 madday | iadd vi09
        vi[9] = i16(vi[9] + vi[9]);
        vf[11] = ftoi<0u>(vf[9]);                                         // 13C8 ftoi0 vf11, vf09 | sq vf09, -2(vi04)
        sq(vf[9], vi[4], -2);
        acc = fma(acc, vf[29], bc<2>(vf[12]));                            // 13D0 maddaz | ibne vi01, 0x1400
        const bool check = vi[1] != 0;
        vf[31] = fma(acc, vf[30], bc<3>(vf[0]));                          // 13D8 maddw vf31, vf30, vf00w | sq vf10, -1(vi05)
        lastFmac = vf[31];
        sq(vf[10], vi[5], -1);
        if (!check)
        {
            const bool more = vi[3] != 0;                                 // 13E0 ibne vi03, 0x1208
            sq(vf[11], vi[5], -2);                                        // 13E8 sq vf11, -2(vi05)
            if (more)
                continue;
            break;                                                        // 13F0 b 0x14c8
        }
        // --- 1400-14C0: a vertex of this triangle is outside the clip volume ---
        ++checkedVertices;
        vi[13] = laneW16(vf[10]);                                         // 1400 mtir vi13, vf10w
        vi[1] = (clip & 0x020820u) != 0u ? 1 : 0;                         // 1408 fcand vi01, 0x020820
        sq(vf[11], vi[5], -2);                                            // 1410 sq vf11, -2(vi05)
        iswW(vi[1], vi[4], -5);                                           // 1418 isw.w vi01, -5(vi04)
        const bool early = vi[10] >= 0;                                   // 1420 ibgez vi10, 0x14b8
        vi[13] = i16(vi[13] | vi[2]);                                     // 1428 ior vi13, vi13, vi02 (delay slot)
        if (!early)
        {
            static constexpr uint32_t kPlanes[5] = {0xFDF7DFu, 0xFF7DF7u, 0xFFBEFBu, 0xFFDF7Du, 0xFFEFBEu};
            bool rejected = false;
            for (const uint32_t plane : kPlanes)
            {
                ++planeTests;
                vi[1] = (clip | plane) == 0xFFFFFFu ? 1 : 0;              // fcor vi01, plane ; ibne vi01, 0x14b8
                if (vi[1] != 0)
                {
                    rejected = true;
                    break;
                }
            }
            if (!rejected)
            {
                ++needsClip;
                iswW(vi[2], vi[5], -3);                                   // 14A8 isw.w vi02, -3(vi05)
                vi[8] = i16(vi[8] + 1);                                   // 14B0 iaddiu vi08, vi08, 1
            }
        }
        const bool more = vi[3] != 0;                                     // 14B8 ibne vi03, 0x1208
        iswW(vi[13], vi[5], -1);                                          // 14C0 isw.w vi13, -1(vi05)
        if (!more)
            break;
    }

    // Write back.
    for (uint32_t reg = 1; reg < 32u; ++reg)
        _mm_storeu_ps(vu.m_state.vf[reg], vf[reg]);
    _mm_storeu_ps(vu.m_state.acc, acc);
    for (uint32_t reg = 1; reg < 16u; ++reg)
        vu.m_state.vi[reg] = vi[reg];
    vu.m_state.q = q;
    vu.m_state.i = iReg;
    {
        static constexpr uint8_t rev[16] = {0x0u, 0x8u, 0x4u, 0xCu, 0x2u, 0xAu, 0x6u, 0xEu, 0x1u, 0x9u, 0x5u, 0xDu, 0x3u, 0xBu, 0x7u, 0xFu};
        const uint32_t zero = rev[_mm_movemask_ps(_mm_cmpeq_ps(lastFmac, _mm_setzero_ps()))];
        const uint32_t sign = rev[_mm_movemask_ps(lastFmac)];
        const uint32_t mac = zero | (sign << 4);
        const uint32_t current = (zero != 0u ? 1u : 0u) | (sign != 0u ? 2u : 0u);
        uint32_t status = vu.m_state.status;
        status = (status & 0xFF0u) | current | (current << 6);
        status = (status & 0xFCFu) | lastDi | (orDi << 6);
        vu.m_state.status = status;
        vu.m_state.mac = mac;
    }
    vu.m_state.clip = clip;
    vu.m_workingClip = clip;

    vu.m_cycle += kCyclesFixed + kCyclesVertex * count + kCyclesBone * boneIterations + kCyclesCheckedVertex * checkedVertices +
                  kCyclesPlaneTest * planeTests + kCyclesNeedsClip * needsClip;
    if (vi[8] == 0)
    {
        // 14C8-14E0: isw.y vi06 / isw.z vi08 / ibne vi08 / isw.w vi14 (delay slot), all to 1017.
        const auto storeLane = [&](int32_t value, uint32_t lane)
        {
            const uint32_t word = static_cast<uint32_t>(static_cast<uint16_t>(value & 0xFFFF));
            std::memcpy(mem + qaddr(0, 1017) + lane * 4u, &word, 4u);
        };
        storeLane(vi[6], 1u);
        storeLane(vi[8], 2u);
        storeLane(vi[14], 3u);
        vu.m_cycle += kCyclesHandOff;
        vu.m_state.pc = 0x14E8u;
    }
    else
        vu.m_state.pc = 0x14C8u; // the compiled code stores and runs the clipping path
    vu.m_state.cycles = vu.m_cycle;
    if (vu.m_cycle - entryCycle < 64u)
    {
        for (auto &lanes : vu.m_vfReady)
            for (uint64_t &ready : lanes)
                ready = std::min(ready, vu.m_cycle);
        for (uint64_t &ready : vu.m_viReady)
            ready = std::min(ready, vu.m_cycle);
        for (uint64_t &ready : vu.m_accReady)
            ready = std::min(ready, vu.m_cycle);
    }
    vu.m_viBranchBackupValid = false;
    vu1NormShadow.valid = false;
    static const bool fitLog = std::getenv("SOTC_VU1_SKIN_FIT") != nullptr; // per-run features for refitting the cycle model
    if (fitLog) [[unlikely]]
        std::fprintf(stderr, "[skin-fit] %u %llu %llu %llu %llu %llu\n", count, static_cast<unsigned long long>(boneIterations),
                     static_cast<unsigned long long>(checkedVertices), static_cast<unsigned long long>(planeTests),
                     static_cast<unsigned long long>(needsClip), static_cast<unsigned long long>(vi[8] == 0 ? 1u : 0u));
    return StepResult::Continue;
}

void registerSotcVu1SkinKernels()
{
    const auto native = &VU1InterpreterAccess::Native<kSkinStripKey>::run;
    registerVu1NativeEntry(kProgram05, 0x0598u, native);
    registerVu1NativeEntry(kProgram05, 0x0920u, native);
    registerVu1NativeEntry(kProgram05, 0x09B8u, native);
}
