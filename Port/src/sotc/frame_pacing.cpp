// Frame pacing (SOTC_FRAME_LOCK).
//
// How the game paces frames: the main loop (sub_001C2898) ends a frame with iFinish (flip request
// at 0x1DC810 = 1) and then iosSendMsg(0x268BA8, 0, 1). That queue has no buffer, so the send blocks
// until the GP thread (sub_001BE700) receives it. The GP thread runs once per VBlank: it counts the
// field (0x1DC860, the main loop's time step), and if iosCheckDrawFinish reports the GS pipeline
// idle it accepts the flip and receives the main loop's message. The next frame then starts with
// iosGPFrameInit, which copies the finished image into the single display buffer.
//
// So the game is double buffered against VBlank: a frame that misses a VBlank by 1 ms waits a whole
// field, and when the host cannot hold 60 the rate alternates between 60 and 30 fps.
//
// 60 / 0: the game's own pacing (up to 60 fps).
// 30:     the GP thread accepts a flip only when at least two fields have passed since the previous
//         one, giving an even 33.3 ms cadence whenever a frame fits in two fields.
//
// Accepting finished frames between VBlanks (mailbox-style) was tried for the 60 mode and measured
// no better: the game's time step is a whole number of fields, so it only let the CPU run ahead.

#include "sotc/frame_pacing.h"
#include "sotc/function_hooks.h"
#include "sotc/log.h"
#include "ps2_runtime_macros.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace sotc
{
    namespace
    {
        constexpr uint32_t kIosCheckDrawFinish = 0x001BD8E8u;
        constexpr uint32_t kGpCheckReturn = 0x001BE798u;   // GP thread: jal iosCheckDrawFinish at 0x1BE790
        constexpr uint32_t kFlipRequest = 0x001DC810u;

        enum class Mode
        {
            GameDefault,
            Lock30,
        };

        Mode g_mode = Mode::GameDefault;
        GuestFunction g_iosCheckDrawFinish = nullptr;
        // All guest threads run on the EE executor thread, so a plain counter suffices.
        uint64_t g_fieldsSinceFlip = 0u;

        uint32_t read32(const uint8_t *rdram, uint32_t address)
        {
            uint32_t value;
            std::memcpy(&value, rdram + (address & PS2_RAM_MASK), sizeof(value));
            return value;
        }

        // GP thread, once per VBlank.
        void checkDrawFinishHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const bool fromGpThread = GPR_U32(ctx, 31) == kGpCheckReturn;
            g_iosCheckDrawFinish(rdram, ctx, runtime);
            if (!fromGpThread)
                return;
            ++g_fieldsSinceFlip;
            const bool finished = GPR_U32(ctx, 2) != 0u;
            if (!finished || read32(rdram, kFlipRequest) == 0u)
                return;
            if (g_mode == Mode::Lock30 && g_fieldsSinceFlip < 2u)
            {
                SET_GPR_U32(ctx, 2, 0u); // not yet: keep the frame for a second field
                return;
            }
            g_fieldsSinceFlip = 0u;
        }

    }

    void installFramePacing(PS2Runtime &runtime)
    {
        const char *setting = std::getenv("SOTC_FRAME_LOCK");
        const std::string_view value = setting ? setting : "60";
        if (value == "30")
            g_mode = Mode::Lock30;
        else if (value != "60" && value != "0" && value != "off")
            SOTC_ERROR(Hook, "SOTC_FRAME_LOCK=" << value << " is not 30 or 60; using 60");
        if (g_mode == Mode::GameDefault)
        {
            SOTC_INFO(Hook, "frame pacing: the game's own, up to 60 fps (SOTC_FRAME_LOCK=60)");
            return;
        }
        g_iosCheckDrawFinish = runtime.lookupFunction(kIosCheckDrawFinish);
        auto &hooks = FunctionHooks::instance();
        if (!g_iosCheckDrawFinish ||
            !hooks.replace(kIosCheckDrawFinish, "iosCheckDrawFinish", &checkDrawFinishHook, ReplacementStatus::Observed,
                           "frame pacing: holds flips for the 30 fps lock"))
        {
            SOTC_ERROR(Hook, "failed to bind frame pacing");
            return;
        }
        SOTC_INFO(Hook, "frame pacing: 30 fps lock (SOTC_FRAME_LOCK=30)");
    }
}
