// Native implementations of hot SotC VU1 microcode routines.
//
// The generated VU1 code models the VU pipeline instruction by instruction: per-register ready
// cycles, stall checks, budget checks, flag-pipeline records and branch bookkeeping. For the hot
// vertex loops that bookkeeping costs far more than the arithmetic. These kernels execute the same
// instructions in program order with the same arithmetic helpers (normalised operands, round toward
// zero, the exact FMAC fallback near overflow/underflow) and reproduce every architectural result:
// VF/VI/ACC/Q/I registers, MAC/status/clip flags and all VU memory stores. Program order is exact
// here because the VU interlocks on VF/VI data hazards, every Q read follows a WAITQ or the 7-cycle
// divide latency, and no branch reads a VI register written by the immediately preceding pair.
// The clip flag reads (FCAND/FCOR) see every earlier CLIPW: each is at least 4 cycles after the
// previous CLIPW, the clip flag latency.
//
// A kernel runs from a registered entry PC and hands back to the compiled code at a PC where all
// pipelines are quiescent; the compiled code then runs the rest of the program (XGKICK, return,
// end, and the rare polygon-clipping path). The cycle count is a per-path static estimate.
// Ready-cycle tables are left at the hand-off cycle (no stalls carried forward).
//
// Verification: PS2X_VU1_VERIFY=1 or sotc_vu1_bench --verify compare each run against the
// interpreter; runs that used a native kernel skip only the ready-table/cycle comparison.
// PS2X_VU1_NATIVE=0 disables the kernels.
//
// Relaxed variant (the default; PS2X_VU1_EXACT=1 selects the exact one). Under round toward zero
// with flush-to-zero and denormals-are-zero, plain SSE arithmetic already gives the VU result for
// every operand the exact helpers treat as ordinary: results below the normal range flush to signed
// zero and results past it saturate to +-FLT_MAX instead of becoming infinite. The relaxed kernels
// therefore drop the per-operation range checks, the operand normalisation on loads and the
// MAC/status bookkeeping. What they give up:
//   - operands with an all-ones exponent (the VU reads them as +-2^128 range values, not inf/NaN);
//     they are not produced by FMAC results, only by loading such bit patterns from memory;
//   - the MAC register and the status flags are reconstructed from the last FMAC only (zero and
//     sign; no underflow/overflow bits, sticky bits not accumulated). No SotC VU1 program reads the
//     sticky bits, MAC reads (FMAND/FMEQ) only follow FMACs in other programs, and the one status
//     read inside these kernels (FSAND 0x002 at 0x17A8) gets the exact sign flag.
// sotc_vu1_bench --compare compares the relaxed kernels against the interpreter and sorts
// the differences (flags only, value-level GS differences, control-flow differences).

#include "ps2_vu1_impl.inl"

#include <cstdint>
#include <cstdlib>

using VuNativeProgram = VU1InterpreterAccess::CompiledProgram;
void registerVu1NativeEntry(uint64_t programHash, uint32_t pc, VuNativeProgram native);

namespace
{
    // Keys for the Native<> specialisations below; only used as template arguments.
    constexpr uint64_t kStripKernelKey = 0x50C0'4E41'5449'1140ull;
    constexpr uint64_t kClipPathKey = 0x50C0'4E41'5449'1410ull;

    // Program 00 of the game's VU1 microcode table (packet 0x0127D710, 0x3470 bytes).
    constexpr uint64_t kProgram00 = 0x810559E580AE6083ull;

    // MXCSR: round toward zero (the VU1 runner already sets it), flush-to-zero, denormals-are-zero.
    constexpr unsigned kRelaxedCsrBits = 0x6000u | 0x8000u | 0x0040u;

    // Sets FTZ/DAZ for the lifetime of a relaxed kernel.
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
    VU1_FORCE_INLINE __m128 splat(__m128 value)
    {
        return _mm_shuffle_ps(value, value, _MM_SHUFFLE(Lane, Lane, Lane, Lane));
    }

    // VU MAC bits (x is bit 3) for the zero and sign of `value` in the `dest` lanes.
    VU1_FORCE_INLINE uint32_t relaxedMac(__m128 value, uint32_t dest)
    {
        static constexpr uint8_t rev[16] = {0x0u, 0x8u, 0x4u, 0xCu, 0x2u, 0xAu, 0x6u, 0xEu, 0x1u, 0x9u, 0x5u, 0xDu, 0x3u, 0xBu, 0x7u, 0xFu};
        const uint32_t zero = rev[_mm_movemask_ps(_mm_cmpeq_ps(value, _mm_setzero_ps()))] & dest;
        const uint32_t sign = rev[_mm_movemask_ps(value)] & dest;
        return zero | (sign << 4);
    }

    VU1_FORCE_INLINE uint32_t relaxedStatus(uint32_t status, uint32_t mac)
    {
        const uint32_t current = ((mac & 0x000Fu) != 0u ? 1u : 0u) | ((mac & 0x00F0u) != 0u ? 2u : 0u);
        return (status & 0xFF0u) | current | (current << 6);
    }
}

template <>
struct VU1InterpreterAccess::Native<kClipPathKey>
{
    template <bool Relaxed>
    static StepResult run(VU1Interpreter &vu, const Vu1RunArgs &args);
};

template <>
struct VU1InterpreterAccess::Native<kStripKernelKey>
{
    template <bool Relaxed>
    static StepResult run(VU1Interpreter &vu, const Vu1RunArgs &args);
};

// Program 00, 0x1410-0x14D8 with its subroutines: called by the strip kernel after its loop when the
// strip has triangles that cross the clip volume (vi08 != 0). For each such triangle: winding cull
// (0x16E8), copy to the clip buffer with ST recentring (0x14F0), Sutherland-Hodgman clipping against
// five planes (0x2688, edge routine 0x2A08, buffer swaps 0x2D00/0x2D60), transform and XGKICK of the
// clipped polygon (0x28B8-0x29E8). Ends at 0x13D8 like the unclipped path.
//
// XGKICKs complete immediately. PATH1 copies the first qword one cycle after the kick, before the
// tag rewrite at 0x28F8, and the next polygon's output is written only after its own first XGKICK,
// which waits for the previous transfer. The GS packets are therefore those of the interpreter.
template <bool Relaxed>
VU1InterpreterAccess::StepResult VU1InterpreterAccess::Native<kClipPathKey>::run(VU1Interpreter &vu, const Vu1RunArgs &args)
{
    uint8_t *const mem = args.vuData;
    Vu1FastCtx ctx; // only passed through to the exact FMAC fallback, which does not use it
    const __m128 zero = _mm_setzero_ps();
    const __m128 vf0 = kVf0();
    [[maybe_unused]] std::conditional_t<Relaxed, RelaxedCsr, char> csr{};

    // Register file: raw and normalised VF values (FMAC results are both), ACC, VI. The relaxed
    // variant reads operands raw, so N aliases R.
    __m128 R[32];
    __m128 Nstore[Relaxed ? 1 : 32];
    __m128 *const N = Relaxed ? R : Nstore;
    for (uint32_t reg = 0; reg < 32u; ++reg)
    {
        R[reg] = _mm_loadu_ps(vu.m_state.vf[reg]);
        if constexpr (!Relaxed)
            N[reg] = vnorm(R[reg]);
    }
    R[0] = N[0] = vf0;
    __m128 accR = _mm_loadu_ps(vu.m_state.acc);
    __m128 accN = Relaxed ? accR : vnorm(accR);
    // Relaxed: the last FMAC result and its dest mask, for MAC/status when they are read.
    __m128 lastValue = zero;
    uint32_t lastDest = 0u;
    int32_t vi[16];
    for (uint32_t reg = 0; reg < 16u; ++reg)
        vi[reg] = vu.m_state.vi[reg];
    vi[0] = 0;
    float q = vu.m_state.q;
    uint32_t status = vu.m_state.status;
    uint32_t mac = vu.m_state.mac;
    uint32_t clip = vu.m_workingClip & 0xFFFFFFu;
    uint64_t cycles = 0u;

    const auto i16 = [](int32_t value) { return static_cast<int32_t>(static_cast<int16_t>(value)); };
    const auto nibbles = [](uint32_t m)
    {
        return ((m & 0x000Fu) != 0u ? 1u : 0u) | ((m & 0x00F0u) != 0u ? 2u : 0u) | ((m & 0x0F00u) != 0u ? 4u : 0u) |
               ((m & 0xF000u) != 0u ? 8u : 0u);
    };
    // FMAC results with eagerly committed MAC/status flags; every flag read below is at least
    // 4 cycles after the FMAC it observes.
    const auto flags = [&](const FmacFlags &fl) [[msvc::forceinline]]
    {
        mac = fl.mac;
        const uint32_t current = nibbles(mac);
        status = (status & 0xFF0u) | current | ((current | fl.extra) << 6);
    };
    // Relaxed: MAC/status of the last FMAC, rebuilt on demand.
    const auto syncFlags = [&]() [[msvc::forceinline]]
    {
        if constexpr (Relaxed)
        {
            if (lastDest != 0u)
            {
                mac = relaxedMac(lastValue, lastDest);
                status = relaxedStatus(status, mac);
            }
        }
    };
    const auto fm = [&]<FK K>(uint32_t dest, __m128 s, __m128 t, __m128 a) [[msvc::forceinline]]
    {
        if constexpr (Relaxed)
        {
            __m128 v;
            if constexpr (K == FK::Add)
                v = _mm_add_ps(s, t);
            else if constexpr (K == FK::Sub)
                v = _mm_sub_ps(s, t);
            else if constexpr (K == FK::Mul)
                v = _mm_mul_ps(s, t);
            else if constexpr (K == FK::Madd)
                v = _mm_add_ps(a, _mm_mul_ps(s, t));
            else
                v = _mm_sub_ps(a, _mm_mul_ps(s, t));
            lastValue = v;
            lastDest = dest;
            return v;
        }
        else
        {
            __m128 n;
            FmacFlags fl;
            const __m128 v = fmacF<K>(vu, ctx, 0u, dest, s, t, a, n, fl);
            flags(fl);
            return v;
        }
    };
    const auto mul = [&](uint32_t dest, __m128 s, __m128 t) [[msvc::forceinline]] { return fm.template operator()<FK::Mul>(dest, s, t, zero); };
    const auto add = [&](uint32_t dest, __m128 s, __m128 t) [[msvc::forceinline]] { return fm.template operator()<FK::Add>(dest, s, t, zero); };
    const auto sub = [&](uint32_t dest, __m128 s, __m128 t) [[msvc::forceinline]] { return fm.template operator()<FK::Sub>(dest, s, t, zero); };
    const auto madd = [&](uint32_t dest, __m128 s, __m128 t) [[msvc::forceinline]] { return fm.template operator()<FK::Madd>(dest, s, t, accN); };
    const auto msub = [&](uint32_t dest, __m128 s, __m128 t) [[msvc::forceinline]] { return fm.template operator()<FK::Msub>(dest, s, t, accN); };
    const auto setF = [&](uint32_t reg, uint32_t dest, __m128 value) [[msvc::forceinline]]
    {
        if (reg == 0u)
            return;
        R[reg] = blendDest(R[reg], value, dest);
        if constexpr (!Relaxed)
            N[reg] = blendDest(N[reg], value, dest);
    };
    const auto setAcc = [&](uint32_t dest, __m128 value) [[msvc::forceinline]]
    {
        accR = blendDest(accR, value, dest);
        if constexpr (Relaxed)
            accN = accR;
        else
            accN = blendDest(accN, value, dest);
    };
    const auto bc = [&](uint32_t reg, uint32_t lane) [[msvc::forceinline]]
    {
        switch (lane)
        {
        case 0u: return splat<0u>(N[reg]);
        case 1u: return splat<1u>(N[reg]);
        case 2u: return splat<2u>(N[reg]);
        default: return splat<3u>(N[reg]);
        }
    };
    const auto lq = [&](uint32_t reg, int32_t base, int32_t offset) [[msvc::forceinline]]
    {
        R[reg] = ldq(mem, qaddr(base, offset));
        if constexpr (!Relaxed)
            N[reg] = vnorm(R[reg]);
    };
    const auto sq = [&](uint32_t reg, int32_t base, int32_t offset) [[msvc::forceinline]]
    {
        _mm_storeu_ps(reinterpret_cast<float *>(mem + qaddr(base, offset)), R[reg]);
    };
    const auto ilw = [&](int32_t base, int32_t offset, uint32_t lane) [[msvc::forceinline]]
    {
        return i16(static_cast<int32_t>(ldw(mem, qaddr(base, offset) + lane * 4u) & 0xFFFFu));
    };
    const auto isw = [&](int32_t value, int32_t base, int32_t offset, uint32_t lane) [[msvc::forceinline]]
    {
        const uint32_t word = static_cast<uint32_t>(static_cast<uint16_t>(value & 0xFFFF));
        std::memcpy(mem + qaddr(base, offset) + lane * 4u, &word, 4u);
    };
    const auto move = [&](uint32_t to, uint32_t from) [[msvc::forceinline]]
    {
        R[to] = R[from];
        N[to] = N[from];
    };
    const auto mr32 = [&](uint32_t to, uint32_t from, uint32_t dest) [[msvc::forceinline]]
    {
        const __m128 r = _mm_shuffle_ps(R[from], R[from], _MM_SHUFFLE(0, 3, 2, 1));
        if constexpr (Relaxed)
            R[to] = blendDest(R[to], r, dest);
        else
        {
            const __m128 n = _mm_shuffle_ps(N[from], N[from], _MM_SHUFFLE(0, 3, 2, 1));
            R[to] = blendDest(R[to], r, dest);
            N[to] = blendDest(N[to], n, dest);
        }
    };
    const auto itofTo = [&](uint32_t to, uint32_t from, uint32_t shift) [[msvc::forceinline]]
    {
        const __m128 v = shift == 12u ? itof<12u>(R[from]) : itof<0u>(R[from]);
        R[to] = N[to] = v;
    };
    const auto ftoiTo = [&](uint32_t to, uint32_t from, uint32_t shift) [[msvc::forceinline]]
    {
        R[to] = shift == 4u ? ftoi<4u>(N[from]) : ftoi<0u>(N[from]);
        if constexpr (!Relaxed)
            N[to] = vnorm(R[to]);
    };
    const auto qn = [&]() [[msvc::forceinline]] { return Relaxed ? _mm_set1_ps(q) : vnorm(_mm_set1_ps(q)); };
    const auto divide = [&](float numN, float denN)
    {
        if constexpr (Relaxed)
        {
            // FTZ/DAZ and round toward zero: a denormal divisor compares equal to zero and an
            // overflowing quotient saturates to +-FLT_MAX.
            uint32_t di = 0u;
            if (denN == 0.0f)
            {
                di = numN == 0.0f ? 0x10u : 0x20u;
                q = std::signbit(numN) != std::signbit(denN) ? -std::numeric_limits<float>::max() : std::numeric_limits<float>::max();
            }
            else
                q = numN / denN;
            status = (status & 0xFCFu) | di | (di << 6);
            return;
        }
        const float num = vu.normalizeOperand(numN);
        const float den = vu.normalizeOperand(denN);
        uint32_t di = 0u;
        float result;
        if (den == 0.0f)
        {
            di = num == 0.0f ? 0x10u : 0x20u;
            result = std::signbit(num) != std::signbit(den) ? -std::numeric_limits<float>::max() : std::numeric_limits<float>::max();
        }
        else
            result = num / den;
        uint32_t ignored = 0u;
        q = vu.normalizeResult(result, ignored);
        status = (status & 0xFCFu) | di | (di << 6);
    };
    const auto clipw = [&](uint32_t reg) [[msvc::forceinline]]
    {
        const __m128 value = R[reg];
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
        if (vu.m_xgkick.active)
            xgkickAdvance(vu, kAll);
        vu.m_cycle += cycles;
        cycles = 0u;
        vu.startXgkick(static_cast<uint32_t>(static_cast<uint16_t>(address)));
        xgkickAdvance(vu, kAll);
    };
    const auto link = [](uint32_t balPc) { return static_cast<int32_t>((balPc + 16u) / 8u); };

    // 0x2A08: clip the edge from the previous vertex (vf23/vf25/vf24, moved to vf18/vf21/vf20) to the
    // next one at vi07+3 against the plane selected by vi10/vi11/vi08 and the sign in vf26.x.
    const auto clipEdge = [&]()
    {
        move(18, 23);
        vi[7] = i16(vi[7] + 3);
        move(21, 25);
        move(20, 24);
        setAcc(0xFu, mul(0xFu, N[5], bc(18, 0)));
        lq(23, vi[7], 0);
        setAcc(0xFu, madd(0xFu, N[6], bc(18, 1)));
        lq(25, vi[7], 1);
        setAcc(0xFu, madd(0xFu, N[7], bc(18, 2)));
        lq(24, vi[7], 2);
        setF(28, 0xFu, madd(0xFu, N[8], bc(18, 3)));
        setAcc(0xFu, mul(0xFu, N[5], bc(23, 0)));
        setAcc(0xFu, madd(0xFu, N[6], bc(23, 1)));
        setAcc(0xFu, madd(0xFu, N[7], bc(23, 2)));
        setF(29, 0xFu, madd(0xFu, N[8], bc(23, 3)));
        clipw(28);
        clipw(29);
        vi[1] = i16(static_cast<int32_t>(clip & 0x0FFFu));
        vi[12] = i16(vi[1] & vi[11]);
        if (vi[12] == vi[11])
        {
            cycles += 22u;
            return; // both outside
        }
        if (vi[12] == 0)
        {
            sq(18, vi[6], 0);
            sq(21, vi[6], 1);
            sq(20, vi[6], 2);
            vi[13] = i16(vi[13] + 1);
            vi[6] = i16(vi[6] + 3);
            cycles += 31u;
            return; // both inside
        }
        setF(28, 0x1u, mul(0x1u, N[28], bc(26, 0)));
        setF(29, 0x1u, mul(0x1u, N[29], bc(26, 0)));
        setF(28, 0xFu, sub(0xFu, N[28], bc(28, 3)));
        setF(29, 0xFu, sub(0xFu, N[29], bc(29, 3)));
        vi[1] = 1;
        if (vi[8] != 0)
        {
            if (vi[8] == vi[1])
            {
                mr32(28, 28, 0xFu);
                mr32(29, 29, 0xFu);
                cycles += 4u;
            }
            else
            {
                mr32(28, 28, 0xFu);
                mr32(29, 29, 0xFu);
                mr32(28, 28, 0xFu);
                mr32(29, 29, 0xFu);
                cycles += 8u;
            }
        }
        setF(27, 0xFu, sub(0xFu, N[29], N[28]));
        if (vi[12] != vi[10])
        {
            // 0x2B70: previous vertex inside, next outside: emit previous and the intersection.
            divide(N[28].m128_f32[0], N[27].m128_f32[0]);
            setF(27, 0x1u, mul(0x1u, N[0], qn()));
            setF(28, 0xFu, sub(0xFu, N[23], N[18]));
            setF(29, 0xFu, sub(0xFu, N[25], N[21]));
            setF(30, 0xFu, sub(0xFu, N[24], N[20]));
            {
                const __m128 v = absN(N[27]);
                R[27] = N[27] = v;
            }
            sq(18, vi[6], 0);
            sq(21, vi[6], 1);
            sq(20, vi[6], 2);
            vi[13] = i16(vi[13] + 1);
            setF(28, 0xFu, mul(0xFu, N[28], bc(27, 3)));
            vi[6] = i16(vi[6] + 3);
            setF(29, 0xFu, mul(0xFu, N[29], bc(27, 3)));
            setF(30, 0xFu, mul(0xFu, N[30], bc(27, 3)));
            setF(28, 0xFu, add(0xFu, N[18], N[28]));
            setF(29, 0xFu, add(0xFu, N[21], N[29]));
            vi[13] = i16(vi[13] + 1);
            setF(30, 0xFu, add(0xFu, N[20], N[30]));
            vi[6] = i16(vi[6] + 3);
            sq(28, vi[6], -3);
            sq(29, vi[6], -2);
            sq(30, vi[6], -1);
            cycles += 64u;
        }
        else
        {
            // 0x2C08: previous vertex outside, next inside: emit the intersection.
            divide(N[29].m128_f32[0], N[27].m128_f32[0]);
            setF(27, 0x1u, mul(0x1u, N[0], qn()));
            setF(28, 0xFu, sub(0xFu, N[18], N[23]));
            setF(29, 0xFu, sub(0xFu, N[21], N[25]));
            setF(30, 0xFu, sub(0xFu, N[20], N[24]));
            {
                const __m128 v = absN(N[27]);
                R[27] = N[27] = v;
            }
            setF(28, 0xFu, mul(0xFu, N[28], bc(27, 3)));
            setF(29, 0xFu, mul(0xFu, N[29], bc(27, 3)));
            setF(30, 0xFu, mul(0xFu, N[30], bc(27, 3)));
            setF(28, 0xFu, add(0xFu, N[23], N[28]));
            setF(29, 0xFu, add(0xFu, N[25], N[29]));
            vi[13] = i16(vi[13] + 1);
            setF(30, 0xFu, add(0xFu, N[24], N[30]));
            vi[6] = i16(vi[6] + 3);
            sq(28, vi[6], -3);
            sq(29, vi[6], -2);
            sq(30, vi[6], -1);
            cycles += 60u;
        }
    };
    // 0x2D00 / 0x2D60: close the polygon in the current buffer and switch buffers.
    const auto closeAndSwap = [&](int32_t from, int32_t to)
    {
        vi[7] = from;
        lq(23, vi[7], 0);
        lq(25, vi[7], 1);
        lq(24, vi[7], 2);
        vi[9] = vi[13];
        sq(23, vi[6], 0);
        sq(25, vi[6], 1);
        sq(24, vi[6], 2);
        vi[7] = from;
        vi[6] = to;
        vi[13] = 0;
        cycles += 14u;
    };
    struct Plane
    {
        uint32_t balPc;      // bal to the buffer switch
        bool positive;       // addw.x (+1) or subw.x (-1) into vf26.x
        int32_t outside;     // vi10: outside mask of the previous vertex
        int32_t outsideNext; // vi12 initial: outside mask of the next vertex
        int32_t component;   // vi08: 0 x, 1 y, 2 z
        uint32_t edgeBalPc;  // bal vi14, 0x2a08
    };
    static constexpr Plane kPlanes[5] = {
        {0x26A0u, false, 0x800, 0x20, 2, 0x26D0u}, {0x2708u, false, 0x200, 0x08, 1, 0x2738u},
        {0x2770u, true, 0x100, 0x04, 1, 0x27A0u},  {0x27D8u, false, 0x080, 0x02, 0, 0x2808u},
        {0x2840u, true, 0x040, 0x01, 0, 0x2870u},
    };

    // 0x2688: clip the triangle copied to 0x360 and draw what remains. Returns via jr vi15.
    const auto clipTriangle = [&]()
    {
        vi[6] = 0x360;
        vi[6] = i16(vi[6] + 9);
        vi[13] = 3;
        cycles += 3u;
        for (uint32_t plane = 0; plane < 5u; ++plane)
        {
            const Plane &p = kPlanes[plane];
            vi[14] = link(p.balPc);
            if (plane & 1u)
                closeAndSwap(0x380, 0x360);
            else
                closeAndSwap(0x360, 0x380);
            {
                // subw.x / addw.x vf26, vf00, vf00w
                const __m128 v = p.positive ? add(0x8u, N[0], bc(0, 3)) : sub(0x8u, N[0], bc(0, 3));
                setF(26, 0x8u, v);
            }
            vi[10] = p.outside;
            vi[12] = p.outsideNext;
            vi[11] = i16(vi[10] | vi[12]);
            cycles += 6u;
            do
            {
                vi[8] = p.component;
                vi[14] = link(p.edgeBalPc);
                vi[9] = i16(vi[9] - 1);
                clipEdge();
                cycles += 7u;
            } while (vi[9] != 0);
            cycles += 2u;
            if (vi[13] == 0)
            {
                cycles += 2u;
                return; // everything clipped away (0x29F8 jr vi15)
            }
        }
        // 0x28A8: bal vi14, 0x2ce0
        vi[14] = link(0x28A8u);
        vi[9] = vi[13];
        vi[7] = 0x380;
        vi[6] = 0x3A1;
        vi[8] = i16(vi[6] + 0);
        vi[6] = i16(vi[6] - 1);
        isw(vi[2], vi[6], 0, 0u);
        vi[13] = vi[9];
        {
            const __m128 r = _mm_shuffle_ps(R[17], R[17], _MM_SHUFFLE(0, 3, 2, 1));
            const __m128 n = _mm_shuffle_ps(N[17], N[17], _MM_SHUFFLE(0, 3, 2, 1));
            R[31] = blendDest(R[31], r, 0x1u);
            N[31] = blendDest(N[31], n, 0x1u);
        }
        cycles += 12u;
        xgkick(vi[6]);
        vi[13] = i16(vi[13] | vi[2]);
        isw(vi[13], vi[6], 0, 0u);
        lq(18, vi[7], 0);
        lq(21, vi[7], 1);
        lq(20, vi[7], 2);
        vi[7] = i16(vi[7] + 3);
        setAcc(0xFu, mul(0xFu, N[1], bc(18, 0)));
        vu.m_state.i = std::bit_cast<float>(0x437F0000u);
        const __m128 iN = vnorm(_mm_set1_ps(vu.m_state.i));
        setAcc(0xFu, madd(0xFu, N[2], bc(18, 1)));
        setAcc(0xFu, madd(0xFu, N[3], bc(18, 2)));
        setF(18, 0xFu, madd(0xFu, N[4], bc(18, 3)));
        ftoiTo(24, 20, 0u);
        divide(N[0].m128_f32[3], N[18].m128_f32[3]);
        cycles += 20u;
        do
        {
            setF(23, 0xEu, mul(0xEu, N[18], qn()));             // 2950 mulq.xyz vf23 ; waitq
            setAcc(0x1u, mul(0x1u, N[17], N[0]));              // 2958 mula.w acc, vf17, vf00
            lq(18, vi[7], 0);                                   //   lq vf18
            setF(23, 0x1u, madd(0x1u, N[31], qn()));           // 2960 maddq.w vf23, vf31, q
            lq(20, vi[7], 2);                                   //   lq vf20
            setF(25, 0xEu, mul(0xEu, N[21], qn()));            // 2968 mulq.xyz vf25, vf21, q
            lq(21, vi[7], 1);                                   // 2970 lq vf21
            setAcc(0xFu, mul(0xFu, N[1], bc(18, 0)));          // 2978 mulax
            vi[7] = i16(vi[7] + 3);
            {                                                   // 2980 minii.w vf23, vf23, i
                const __m128 v = _mm_min_ps(N[23], iN);
                R[23] = blendDest(R[23], v, 0x1u);
                N[23] = blendDest(N[23], v, 0x1u);
            }
            vi[9] = i16(vi[9] - 1);
            setAcc(0xFu, madd(0xFu, N[2], bc(18, 1)));         // 2988 madday
            vi[8] = i16(vi[8] + 3);
            setAcc(0xFu, madd(0xFu, N[3], bc(18, 2)));         // 2990 maddaz
            setF(18, 0xFu, madd(0xFu, N[4], bc(18, 3)));       // 2998 maddw vf18, vf04, vf18w
            {                                                   // 29A0 maxx.w vf23, vf23, vf00x
                const __m128 v = _mm_max_ps(N[23], zero);
                R[23] = blendDest(R[23], v, 0x1u);
                N[23] = blendDest(N[23], v, 0x1u);
            }
            sq(24, vi[8], -2);                                  // 29A8
            divide(N[0].m128_f32[3], N[18].m128_f32[3]);        // 29B0
            ftoiTo(23, 23, 4u);                                 // 29B8 ftoi4 vf23
            sq(25, vi[8], -3);                                  // 29C0
            ftoiTo(24, 20, 0u);                                 // 29C8 ftoi0 vf24, vf20
            sq(23, vi[8], -1);                                  // 29D8 (delay slot)
            cycles += 18u;
        } while (vi[9] != 0);
        xgkick(vi[6]);                                          // 29E8
        cycles += 4u;
    };

    // 13B8-13D0
    isw(vi[6], 0, 1017, 1u);
    isw(vi[8], 0, 1017, 2u);
    isw(vi[14], 0, 1017, 3u);
    // 1410
    lq(5, 0, 1004);
    lq(6, 0, 1005);
    lq(7, 0, 1006);
    lq(8, 0, 1007);
    vi[3] = ilw(vi[11], 0, 0u);
    vi[5] = i16(vi[6] + 1);
    vi[4] = i16(vi[11] + 1);
    vi[9] = 0x7FFF;
    vi[3] = i16(vi[9] & vi[3]);
    isw(vi[3], 0, 1017, 0u);
    cycles += 14u;
    do
    {
        vi[1] = ilw(vi[5], 0, 3u);
        vi[5] = i16(vi[5] + 3);
        vi[4] = i16(vi[4] + 3);
        vi[3] = i16(vi[3] - 1);
        cycles += 6u;
        if (vi[1] == vi[2])
        {
            // 0x1490: bal vi15, 0x14e0 ; 0x14E0: bal vi14, 0x16e8
            vi[15] = link(0x1490u);
            vi[14] = link(0x14E0u);
            cycles += 4u;
            // 0x16E8: winding cull.
            bool draw = true;
            vi[10] = ilw(0, 1011, 2u);
            vi[11] = ilw(vi[4], -3, 3u);
            lq(30, vi[5], -7);
            lq(23, vi[5], -4);
            lq(31, vi[5], -1);
            cycles += 5u;
            if (vi[10] != 0)
                cycles += 2u;
            else
            {
                itofTo(30, 30, 12u);
                cycles += 2u;
                if (vi[11] != 0)
                    cycles += 2u;
                else
                {
                    itofTo(23, 23, 12u);
                    itofTo(31, 31, 12u);
                    setF(26, 0xCu, sub(0xCu, N[23], N[30]));
                    setF(27, 0xCu, sub(0xCu, N[31], N[23]));
                    vi[10] = ilw(0, 1017, 0u);
                    vi[9] = 2;
                    setAcc(0x8u, mul(0x8u, N[26], bc(27, 1)));
                    vi[12] = 1;
                    if constexpr (Relaxed)
                    {
                        // The cull branches on the sign flag of this cross product. When it cancels
                        // to zero (degenerate triangles) the exact model takes the sign of the
                        // unrounded result, so use the exact helper here in both variants.
                        __m128 n;
                        FmacFlags fl;
                        const __m128 v = fmacF<FK::Msub>(vu, ctx, 0u, 0x8u, vnorm(N[27]), vnorm(bc(26, 1)), vnorm(accN), n, fl);
                        flags(fl);
                        lastDest = 0u; // MAC/status now hold the exact flags
                        setF(23, 0x8u, v);
                    }
                    else
                        setF(23, 0x8u, msub(0x8u, N[27], bc(26, 1)));
                    vi[10] = i16(vi[10] - vi[3]);
                    syncFlags();
                    vi[1] = i16(static_cast<int32_t>((status & 0xFFFu) & 0x2u));
                    vi[11] = i16(vi[10] & vi[12]);
                    const bool odd = vi[11] == vi[12];
                    draw = (vi[1] == vi[9]) ? !odd : odd;
                    cycles += 22u;
                }
            }
            if (draw)
            {
                // 0x14F0: copy the triangle to 0x360, w = 1, ST recentred on the integer part of its mean.
                vi[6] = 0x360;
                lq(18, vi[4], -9);
                lq(27, vi[4], -8);
                lq(24, vi[4], -7);
                R[18] = blendDest(R[18], vf0, 0x1u);
                N[18] = blendDest(N[18], vf0, 0x1u);
                lq(23, vi[4], -6);
                lq(28, vi[4], -5);
                lq(20, vi[4], -4);
                sq(18, vi[6], 0);
                R[23] = blendDest(R[23], vf0, 0x1u);
                N[23] = blendDest(N[23], vf0, 0x1u);
                sq(24, vi[6], 2);
                lq(18, vi[4], -3);
                lq(29, vi[4], -2);
                lq(24, vi[4], -1);
                setF(26, 0xCu, add(0xCu, N[27], N[28]));
                sq(23, vi[6], 3);
                R[18] = blendDest(R[18], vf0, 0x1u);
                N[18] = blendDest(N[18], vf0, 0x1u);
                sq(20, vi[6], 5);
                sq(18, vi[6], 6);
                setF(26, 0xCu, add(0xCu, N[26], N[29]));
                vu.m_state.i = std::bit_cast<float>(0x3EAAAAAAu);
                sq(24, vi[6], 8);
                setF(26, 0xCu, mul(0xCu, N[26], vnorm(_mm_set1_ps(vu.m_state.i))));
                ftoiTo(26, 26, 0u);
                mr32(27, 0, 0x3u);
                mr32(28, 0, 0x3u);
                mr32(29, 0, 0x3u);
                itofTo(26, 26, 0u);
                vi[10] = ilw(0, 1011, 3u);
                vi[9] = 0;
                cycles += 29u;
                // 15D8/15F0/1608: ibeq on modes 0/1/2; the vi09 updates sit in the delay slots.
                const int32_t mode = vi[10];
                uint32_t subDest = 0u;
                vi[9] = 1;
                if (mode == 0)
                    subDest = 0xCu;
                else
                {
                    vi[9] = 2;
                    if (mode == 1)
                        subDest = 0x4u;
                    else if (mode == 2)
                        subDest = 0x8u;
                }
                if (subDest != 0u)
                {
                    setF(27, subDest, sub(subDest, N[27], N[26]));
                    setF(28, subDest, sub(subDest, N[28], N[26]));
                    setF(29, subDest, sub(subDest, N[29], N[26]));
                }
                sq(27, vi[6], 1);
                sq(28, vi[6], 4);
                sq(29, vi[6], 7);
                cycles += 12u;
                clipTriangle();
            }
        }
        cycles += 2u;
    } while (vi[3] != 0);
    // 0x14B0
    lq(5, 0, 1000);
    lq(6, 0, 1001);
    lq(7, 0, 1002);
    lq(8, 0, 1003);
    cycles += 6u;

    for (uint32_t reg = 1; reg < 32u; ++reg)
        _mm_storeu_ps(vu.m_state.vf[reg], R[reg]);
    _mm_storeu_ps(vu.m_state.acc, accR);
    for (uint32_t reg = 1; reg < 16u; ++reg)
        vu.m_state.vi[reg] = vi[reg];
    vu.m_state.q = q;
    syncFlags();
    vu.m_state.status = status;
    vu.m_state.mac = mac;
    vu.m_state.clip = clip;
    vu.m_workingClip = clip;
    vu.m_cycle += cycles;
    return StepResult::Continue;
}

template <bool Relaxed>
VU1InterpreterAccess::StepResult VU1InterpreterAccess::Native<kStripKernelKey>::run(VU1Interpreter &vu, const Vu1RunArgs &args)
{
    // Entry: "bal vi14, 0x1140" + nop delay slot at 0x05B0 or 0x0628. Routine 0x1140 transforms a
    // strip of vertices (position through vf01-vf04, perspective divide, STQ, colour, fog clamp,
    // clip test through vf05-vf08, per-triangle trivial-reject/needs-clip classification) and
    // reaches 0x13B8 after the loop.
    if (vu.m_state.branchPending || vu.m_state.ebit || vu.m_state.haltAfterDelaySlot || vu.m_stopRequested ||
        args.dataSize != kFastMemSize || !args.vuData)
        return StepResult::Fallback;
    if ((vu.m_flagValid | vu.m_vfWriteValid | vu.m_viWriteValid | vu.m_accWriteValid | vu.m_storeValid) != 0u ||
        vu.m_xgkick.active || vu.m_fdiv.valid || vu.m_efu[0].valid || vu.m_efu[1].valid)
        return StepResult::Fallback;

    uint8_t *const mem = args.vuData;
    const uint32_t entryPc = vu.m_state.pc;
    const uint64_t entryCycle = vu.m_cycle;
    const int32_t top = static_cast<int16_t>(vu.m_state.top & 0x3FFu);
    const uint32_t count = ldw(mem, qaddr(top, 0)) & 0x7FFFu;
    if (count == 0u)
        return StepResult::Fallback;
    // Cycle costs of the interpreter's pipeline model for each path, fitted over 3,054 gameplay runs
    // (mean error 0.5 cycles per run): the bal/prologue/exit, a vertex on the fast path, a vertex whose
    // triangle touches the clip volume (plus one cycle when it is among the strip's first two), each
    // FCOR plane test, and a triangle classified as needing clipping.
    constexpr uint64_t kCyclesFixed = 31u;
    constexpr uint64_t kCyclesFastVertex = 24u;
    constexpr uint64_t kCyclesCheckedVertex = 32u;
    constexpr uint64_t kCyclesEarlyExtra = 1u;
    constexpr uint64_t kCyclesPlaneTest = 3u;
    constexpr uint64_t kCyclesNeedsClip = 2u;
    constexpr uint64_t kCyclesHandOff = 4u; // 13B8-13D0 when no triangle needs clipping
    // Worst case per vertex is the full classification path; never risk crossing the budget.
    constexpr uint64_t kWorstVertexCycles = kCyclesCheckedVertex + kCyclesEarlyExtra + 5u * kCyclesPlaneTest + kCyclesNeedsClip;
    if (vu.m_cycle + kCyclesFixed + kWorstVertexCycles * count + 64u > args.budgetEnd)
        return StepResult::Fallback;

    Vu1FastCtx ctx; // only passed through to the exact FMAC fallback, which does not use it
    float(*const vf)[4] = vu.m_state.vf;
    [[maybe_unused]] std::conditional_t<Relaxed, RelaxedCsr, char> csr{};

    // Operand normalisation; the relaxed variant reads operands raw.
    const auto nrm = [](__m128 value) [[msvc::forceinline]] { return Relaxed ? value : vnorm(value); };
    const auto raw = [&](uint32_t reg) { return _mm_loadu_ps(vf[reg]); };
    const __m128 n1 = nrm(raw(1)), n2 = nrm(raw(2)), n3 = nrm(raw(3)), n4 = nrm(raw(4));
    const __m128 n5 = nrm(raw(5)), n6 = nrm(raw(6)), n7 = nrm(raw(7)), n8 = nrm(raw(8));
    const __m128 r17 = raw(17), n17 = nrm(r17);
    const __m128 one = bcast<3u>(kVf0());
    const __m128 zero = _mm_setzero_ps();

    // Registers written by the routine, raw and normalised.
    __m128 r18 = raw(18), n18 = nrm(r18);
    __m128 r20 = raw(20), n20 = nrm(r20);
    __m128 r21 = raw(21), n21 = nrm(r21);
    __m128 n23 = nrm(raw(23));
    __m128 n24 = nrm(raw(24));
    __m128 r25 = raw(25), n25 = nrm(r25);
    __m128 r26 = raw(26), n26 = nrm(r26);
    __m128 r27 = raw(27), n27 = nrm(r27);
    __m128 n28 = nrm(raw(28));
    __m128 r29 = raw(29), n29 = nrm(r29);
    __m128 acc = nrm(_mm_loadu_ps(vu.m_state.acc));
    int32_t vi1 = vu.m_state.vi[1], vi2 = vu.m_state.vi[2], vi3 = vu.m_state.vi[3], vi4 = vu.m_state.vi[4];
    int32_t vi5 = vu.m_state.vi[5], vi6 = vu.m_state.vi[6], vi8 = vu.m_state.vi[8], vi9 = vu.m_state.vi[9];
    int32_t vi10 = vu.m_state.vi[10], vi11 = vu.m_state.vi[11];
    uint32_t clip = vu.m_workingClip & 0xFFFFFFu;
    float q = vu.m_state.q;
    uint32_t lastDi = 0u, orDi = 0u;
    __m128 accZ = zero, accS = zero;
    uint32_t accScalar = 0u;
    FmacFlags lastFlags{};
    uint64_t cycles = 0u;

    const auto i16 = [](int32_t value) { return static_cast<int32_t>(static_cast<int16_t>(value)); };
    const auto store = [&](int32_t base, int32_t offset, __m128 value) { _mm_storeu_ps(reinterpret_cast<float *>(mem + qaddr(base, offset)), value); };
    const auto storeW = [&](int32_t base, int32_t offset, int32_t value)
    {
        const uint32_t word = static_cast<uint32_t>(static_cast<uint16_t>(value & 0xFFFF));
        std::memcpy(mem + qaddr(base, offset) + 12u, &word, 4u);
    };
    const auto load = [&](int32_t base, int32_t offset) { return ldq(mem, qaddr(base, offset)); };
    const auto qn = [&]() { return nrm(_mm_set1_ps(q)); };
    // FMAC with sticky-flag accumulation (the MAC register only matters for the last FMAC).
    // Relaxed: plain SSE; MAC/status come from the final vf25 (see the write-back).
    const auto mul = [&](uint32_t dest, __m128 s, __m128 t) [[msvc::forceinline]]
    {
        if constexpr (Relaxed)
            return _mm_mul_ps(s, t);
        else
        {
            __m128 n;
            return fmacV<FK::Mul>(vu, ctx, 0u, dest, s, t, zero, n, accZ, accS, accScalar);
        }
    };
    const auto madd = [&](uint32_t dest, __m128 s, __m128 t, __m128 a) [[msvc::forceinline]]
    {
        if constexpr (Relaxed)
            return _mm_add_ps(a, _mm_mul_ps(s, t));
        else
        {
            __m128 n;
            return fmacV<FK::Madd>(vu, ctx, 0u, dest, s, t, a, n, accZ, accS, accScalar);
        }
    };
    const auto mulLast = [&](uint32_t dest, __m128 s, __m128 t) [[msvc::forceinline]]
    {
        if constexpr (Relaxed)
            return _mm_mul_ps(s, t);
        else
        {
            __m128 n;
            FmacFlags flags;
            const __m128 value = fmacF<FK::Mul>(vu, ctx, 0u, dest, s, t, zero, n, flags);
            if (dest != 0u)
            {
                lastFlags = flags;
                accScalar |= flags.status | flags.extra;
            }
            return value;
        }
    };
    const auto divide = [&](__m128 denominatorN) [[msvc::forceinline]]
    {
        if constexpr (Relaxed)
        {
            const float den = laneF<3u>(denominatorN);
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
            return;
        }
        const float num = vu.normalizeOperand(1.0f);
        const float den = vu.normalizeOperand(laneF<3u>(denominatorN));
        uint32_t di = 0u;
        float result;
        if (den == 0.0f)
        {
            di = num == 0.0f ? 0x10u : 0x20u;
            result = std::signbit(num) != std::signbit(den) ? -std::numeric_limits<float>::max() : std::numeric_limits<float>::max();
        }
        else
            result = num / den;
        uint32_t ignored = 0u;
        q = vu.normalizeResult(result, ignored);
        lastDi = di;
        orDi |= di;
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
    const auto blendW = [](__m128 old, __m128 value) { return blendDest(old, value, 0x1u); };
    const auto blendXyz = [](__m128 old, __m128 value) { return blendDest(old, value, 0xEu); };

    // 05B0/0628: bal vi14, 0x1140 ; delay slot nop
    const int32_t vi14 = static_cast<int32_t>((entryPc + 16u) / 8u);
    // 1140-1240: prologue.
    vi4 = top;                                            // xtop vi04
    vi9 = 0x7FFF;                                         // iaddiu vi09, vi00, 0x7fff
    vi11 = vi4;                                           // iaddiu vi11, vi04, 0
    vi3 = i16(static_cast<int32_t>(ldw(mem, qaddr(vi4, 0)) & 0xFFFFu));        // ilwr.x vi03, (vi04)
    vi6 = i16(static_cast<int32_t>(ldw(mem, qaddr(vi4, 0) + 12u) & 0xFFFFu));  // ilwr.w vi06, (vi04)
    vi2 = i16(vi9 + 1);                                   // iaddiu vi02, vi09, 1
    r26 = load(vi4, 0); n26 = nrm(r26); vi4 = i16(vi4 + 1);                    // lqi vf26, (vi04++)
    vi3 = i16(vi9 & vi3);                                 // iand vi03, vi09, vi03
    vi5 = vi6;                                            // iaddiu vi05, vi06, 0
    vi8 = 0;                                              // iaddiu vi08, vi00, 0
    {                                                     // mr32.w vf29, vf17
        const __m128 mr = _mm_shuffle_ps(r17, r17, _MM_SHUFFLE(0, 3, 2, 1));
        const __m128 mn = _mm_shuffle_ps(n17, n17, _MM_SHUFFLE(0, 3, 2, 1));
        r29 = blendW(r29, mr);
        n29 = blendW(n29, mn);
    }
    vi10 = 2;                                             // iaddi vi10, vi00, 2
    store(vi5, 0, r26); vi5 = i16(vi5 + 1);               // sqi vf26, (vi05++)
    r18 = load(vi4, 0); n18 = nrm(r18);                 // lq vf18, 0(vi04)
    r21 = load(vi4, 1); n21 = nrm(r21);                 // lq vf21, 1(vi04)
    r20 = load(vi4, 2); n20 = nrm(r20);                 // lq vf20, 2(vi04)
    acc = mul(0xFu, n1, bcast<0u>(n18));                  // mulax.xyzw acc, vf01, vf18x
    acc = madd(0xFu, n2, bcast<1u>(n18), acc);            // madday
    acc = madd(0xFu, n3, bcast<2u>(n18), acc);            // maddaz
    n23 = madd(0xFu, n4, one, acc);                       // maddw.xyzw vf23, vf04, vf00w
    n25 = itof<12u>(r21); r25 = n25;                      // itof12.xyzw vf25, vf21
    acc = mul(0xFu, n5, bcast<0u>(n18));                  // mulax.xyzw acc, vf05, vf18x
    acc = madd(0xFu, n6, bcast<1u>(n18), acc);            // madday
    acc = madd(0xFu, n7, bcast<2u>(n18), acc);            // maddaz ; div q, vf00w, vf23w
    divide(n23);
    n28 = madd(0xFu, n8, one, acc);                       // maddw.xyzw vf28, vf08, vf00w ; sq vf25, 1(vi04)
    store(vi4, 1, r25);
    n24 = itof<0u>(r20);                                  // itof0.xyzw vf24, vf20 ; loi 255
    vu.m_state.i = std::bit_cast<float>(0x437F0000u);
    const __m128 iN = nrm(_mm_set1_ps(vu.m_state.i));
    vi4 = i16(vi4 + 3);                                   // iaddiu vi04, vi04, 3
    r25 = _mm_insert_ps(r25, one, 0x20);                  // mr32.zw vf25, vf00 (z = vf00.w, w = vf00.x)
    r25 = _mm_insert_ps(r25, zero, 0x30);
    n25 = r25;
    store(vi4, -1, n24);                                  // sq vf24, -1(vi04)
    n26 = blendXyz(n26, mul(0xEu, n23, qn())); r26 = blendXyz(r26, n26);       // mulq.xyz vf26, vf23, q ; waitq
    n25 = blendXyz(n25, mulLast(0xEu, n25, qn())); r25 = n25;                  // mulq.xyz vf25, vf25, q
    r18 = load(vi4, 0); n18 = nrm(r18);                 //   lq vf18, 0(vi04)
    acc = blendW(acc, mul(0x1u, n17, kVf0()));            // mula.w acc, vf17, vf00
    clipw(n28);                                           // clipw.xyz vf28, vf28w
    cycles += kCyclesFixed;

    // 1248-13B0: one vertex per iteration (software pipelined).
    for (;;)
    {
        n26 = blendW(n26, madd(0x1u, n29, qn(), acc)); r26 = blendW(r26, n26);   // A maddq.w vf26, vf29, q
        r21 = load(vi4, 1); n21 = nrm(r21);                                       //   lq vf21, 1(vi04)
        acc = mul(0xFu, n1, bcast<0u>(n18));                                        // B mulax
        store(vi5, 1, r20);                                                         //   sq vf20, 1(vi05)
        acc = madd(0xFu, n2, bcast<1u>(n18), acc);                                  // C madday
        vi10 = i16(vi10 - 1);                                                       //   iaddi vi10, vi10, -1
        acc = madd(0xFu, n3, bcast<2u>(n18), acc);                                  // D maddaz
        vi1 = (clip & 0x3FFFFu) != 0u ? 1 : 0;                                      //   fcand vi01, 0x3ffff
        n23 = madd(0xFu, n4, one, acc);                                             // E maddw vf23
        r20 = load(vi4, 2); n20 = nrm(r20);                                       //   lq vf20, 2(vi04)
        {                                                                           // F minii.w vf26, vf26, i
            const __m128 value = _mm_min_ps(n26, iN);
            r26 = blendW(r26, value); n26 = blendW(n26, value);
        }
        store(vi5, 0, r25);                                                         //   sq vf25, 0(vi05)
        n25 = itof<12u>(r21); r25 = n25;                                            // G itof12 vf25, vf21
        vi5 = i16(vi5 + 3);                                                         //   iaddiu vi05, vi05, 3
        acc = mul(0xFu, n5, bcast<0u>(n18));                                        // H mulax
        vi4 = i16(vi4 + 3);                                                         //   iaddiu vi04, vi04, 3
        n24 = itof<0u>(r20);                                                        // I itof0 vf24, vf20
        divide(n23);                                                                //   div q, vf00w, vf23w
        {                                                                           // J maxx.w vf26, vf26, vf00x
            const __m128 value = _mm_max_ps(n26, zero);
            r26 = blendW(r26, value); n26 = blendW(n26, value);
        }
        vi3 = i16(vi3 - 1);                                                         //   iaddi vi03, vi03, -1
        acc = madd(0xFu, n6, bcast<1u>(n18), acc);                                  // K madday
        store(vi4, -2, n25);                                                        //   sq vf25, -2(vi04)
        acc = madd(0xFu, n7, bcast<2u>(n18), acc);                                  // L maddaz
        r25 = _mm_insert_ps(r25, one, 0x20); r25 = _mm_insert_ps(r25, zero, 0x30); //   mr32.zw vf25, vf00
        n25 = r25;
        n28 = madd(0xFu, n8, one, acc);                                             // M maddw vf28
        store(vi4, -1, n24);                                                        //   sq vf24, -1(vi04)
        r27 = ftoi<4u>(n26); n27 = nrm(r27);                                      // N ftoi4 vf27, vf26
        const bool needsCheck = vi1 != 0;                                           //   ibne vi01, vi00, 0x12e8
        acc = blendW(acc, mul(0x1u, n17, kVf0()));                                  // O mula.w acc, vf17, vf00
        r18 = load(vi4, 0); n18 = nrm(r18);                                       //   lq vf18, 0(vi04)
        if (!needsCheck)
        {
            n26 = blendXyz(n26, mul(0xEu, n23, qn())); r26 = blendXyz(r26, n26);   // P mulq.xyz vf26 ; waitq
            n25 = blendXyz(n25, mulLast(0xEu, n25, qn())); r25 = n25;              // Q mulq.xyz vf25
            const bool more = vi3 != 0;                                             //   ibne vi03, vi00, 0x1248
            clipw(n28);                                                             // R clipw vf28
            store(vi5, -1, r27);                                                    //   sq vf27, -1(vi05)
            cycles += kCyclesFastVertex;
            if (more)
                continue;
            break;                                                                  // 12D8 b 0x13b8 ; nop
        }
        // 12E8: a vertex of the current triangle is outside the clip volume.
        vi9 = i16(static_cast<int32_t>(static_cast<uint32_t>(_mm_extract_ps(r27, 3)) & 0xFFFFu)); // mtir vi09, vf27w
        vi1 = (clip & 0x020820u) != 0u ? 1 : 0;                                     // fcand vi01, 0x020820
        n26 = blendXyz(n26, mul(0xEu, n23, qn())); r26 = blendXyz(r26, n26);       // mulq.xyz vf26
        storeW(vi4, -6, vi1);                                                       //   isw.w vi01, -6(vi04)
        n25 = blendXyz(n25, mulLast(0xEu, n25, qn())); r25 = n25;                  // mulq.xyz vf25
        vi9 = i16(vi9 | vi2);                                                       //   ior vi09, vi09, vi02
        const bool early = vi10 >= 0;                                               // ibgez vi10, 0x13a8
        {                                                                           //   mfir.w vf27, vi09 (delay slot)
            const __m128 value = _mm_castsi128_ps(_mm_set1_epi32(static_cast<int16_t>(vi9 & 0xFFFF)));
            r27 = blendW(r27, value); n27 = blendW(n27, nrm(value));
        }
        cycles += kCyclesCheckedVertex + (vi10 >= 0 ? kCyclesEarlyExtra : 0u);
        if (!early)
        {
            static constexpr uint32_t kPlanes[5] = {0xFF7DF7u, 0xFFBEFBu, 0xFFDF7Du, 0xFFEFBEu, 0xFDF7DFu};
            bool rejected = false;
            for (const uint32_t plane : kPlanes)
            {
                vi1 = (clip | plane) == 0xFFFFFFu ? 1 : 0;                          // fcor vi01, plane
                cycles += kCyclesPlaneTest;                                         // ibne vi01, vi00, 0x13a8 ; nop
                if (vi1 != 0)
                {
                    rejected = true;
                    break;
                }
            }
            if (!rejected)
            {
                storeW(vi5, -3, vi2);                                               // isw.w vi02, -3(vi05)
                vi8 = i16(vi8 + 1);                                                 // iaddiu vi08, vi08, 1
                cycles += kCyclesNeedsClip;
            }
        }
        const bool more = vi3 != 0;                                                 // 13A8 ibne vi03, vi00, 0x1248
        clipw(n28);                                                                 // 13B0 clipw vf28
        store(vi5, -1, r27);                                                        //   sq vf27, -1(vi05)
        if (!more)
            break;
    }

    // Write back. Every FMAC result is normal or zero, so raw == normalised for those registers.
    _mm_storeu_ps(vf[18], r18);
    _mm_storeu_ps(vf[20], r20);
    _mm_storeu_ps(vf[21], r21);
    _mm_storeu_ps(vf[23], n23);
    _mm_storeu_ps(vf[24], n24);
    _mm_storeu_ps(vf[25], r25);
    _mm_storeu_ps(vf[26], r26);
    _mm_storeu_ps(vf[27], r27);
    _mm_storeu_ps(vf[28], n28);
    _mm_storeu_ps(vf[29], r29);
    _mm_storeu_ps(vu.m_state.acc, acc);
    vu.m_state.vi[1] = vi1;
    vu.m_state.vi[2] = vi2;
    vu.m_state.vi[3] = vi3;
    vu.m_state.vi[4] = vi4;
    vu.m_state.vi[5] = vi5;
    vu.m_state.vi[6] = vi6;
    vu.m_state.vi[8] = vi8;
    vu.m_state.vi[9] = vi9;
    vu.m_state.vi[10] = vi10;
    vu.m_state.vi[11] = vi11;
    vu.m_state.vi[14] = vi14;
    vu.m_state.q = q;

    const auto nibbles = [](uint32_t mac)
    {
        return ((mac & 0x000Fu) != 0u ? 1u : 0u) | ((mac & 0x00F0u) != 0u ? 2u : 0u) | ((mac & 0x0F00u) != 0u ? 4u : 0u) |
               ((mac & 0xF000u) != 0u ? 8u : 0u);
    };
    uint32_t status = vu.m_state.status;
    if constexpr (Relaxed)
    {
        // The last FMAC is the mulq.xyz into vf25 (Q); nothing writes vf25.xyz after it.
        lastFlags.mac = relaxedMac(n25, 0xEu);
        status = relaxedStatus(status, lastFlags.mac);
    }
    else
    {
        const uint32_t sticky = nibbles(lastFlags.mac) | accScalar | stickyOf(accZ, accS);
        status = (status & 0xFF0u) | nibbles(lastFlags.mac) | (sticky << 6);
    }
    status = (status & 0xFCFu) | lastDi | (orDi << 6);
    vu.m_state.status = status;
    vu.m_state.mac = lastFlags.mac;
    vu.m_workingClip = clip;
    vu.m_state.clip = clip;

    vu.m_cycle += cycles;
    vu.m_state.pc = 0x13B8u;
    // Clipping a triangle costs at most a few thousand cycles; if the budget could run out inside the
    // clipper, leave it to the compiled code, which can stop and resume at any pair.
    constexpr uint64_t kWorstClipTriangleCycles = 8000u;
    if (vi8 == 0)
    {
        // 13B8-13D0: isw.y vi06 / isw.z vi08 / ibne vi08 / isw.w vi14 (delay slot), all to 1017.
        const auto storeLane = [&](int32_t value, uint32_t lane)
        {
            const uint32_t word = static_cast<uint32_t>(static_cast<uint16_t>(value & 0xFFFF));
            std::memcpy(mem + qaddr(0, 1017) + lane * 4u, &word, 4u);
        };
        storeLane(vi6, 1u);
        storeLane(vi8, 2u);
        storeLane(vi14, 3u);
        vu.m_cycle += kCyclesHandOff;
        vu.m_state.pc = 0x13D8u;
    }
    else if (vu.m_cycle + kWorstClipTriangleCycles * static_cast<uint64_t>(vi8) + 64u <= args.budgetEnd)
    {
        Native<kClipPathKey>::run<Relaxed>(vu, args);
        vu.m_state.pc = 0x13D8u;
    }
    vu.m_state.cycles = vu.m_cycle;
    // Ready cycles recorded before the kernel lie at most a few latencies past its start; after 64
    // cycles none can stall the compiled code that resumes here.
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
    return StepResult::Continue;
}

// PS2X_VU1_EXACT=1 selects the bit-exact kernels; the default is the relaxed variant.
bool sotcVu1NativeExact()
{
    static const bool exact = []
    {
        const char *value = std::getenv("PS2X_VU1_EXACT");
        return value && *value && *value != '0';
    }();
    return exact;
}

void registerSotcVu1SkinKernels(); // sotc_vu1_skin.cpp (relaxed only)

void registerSotcVu1NativeKernels()
{
    if (!sotcVu1NativeExact())
        registerSotcVu1SkinKernels();
    const VuNativeProgram strip = sotcVu1NativeExact() ? &VU1InterpreterAccess::Native<kStripKernelKey>::run<false>
                                                       : &VU1InterpreterAccess::Native<kStripKernelKey>::run<true>;
    registerVu1NativeEntry(kProgram00, 0x05B0u, strip);
    registerVu1NativeEntry(kProgram00, 0x0628u, strip);
}
