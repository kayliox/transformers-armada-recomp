// TODO 182 / 172 / 183: the ragdolls simulated at the console's 1/30 s at any frame rate.
//
// Every ragdoll (the player's, his sidekick's, the Decepticlones' deaths and knock-downs: taRagDoll, from the pool of
// taRagDollManager) is a Verlet particle system, taVerletIntegrator (count +0x00, particles +0x04, 0x50 bytes each:
// the position +0x00, the position a step before +0x10, the force +0x30). Its Update (0x3C9E90, a0 the integrator,
// f12 the frame's time; called by taRagDoll's update 0x3C4800, which then poses the model from the particles) adds
// gravity to the forces, moves each particle by x + (x - x_before) * damping + force * dt^2 (0x3CA430: the damping a
// constant per step), pushes the particles out of the scenery and the other bodies and keeps the bones' lengths
// (0x3CA310), and clears the forces. The velocity is the step's own: a frame of 1/240 s loses as much to damping as
// one of 1/30 s, and a kick's push out of the kicking foot is spread over eight short pushes that each move it a
// little. Measured on the user's state (Megatron, 182): at 30 the kick gave the player's ragdoll root 114 u/s in one
// frame and he flew into the lava; at 60, 120 and 240 it pushed him at ~40 u/s for a few frames and that was gone
// within ~10 ms - he stayed at Megatron's feet and was kicked again. The same frame-length dependence makes ragdolls
// tumble and spin differently (172) and the human-size one bounce (183).
//
// So each integrator is stepped at the console's rhythm: a step once every 1/30 s of the level's frames (a credit of
// 30/R a frame, as 183's velocity hand-over counts), with the time of those frames summed (1/30 s, or less in the
// game's slow motion, as the console's own frame would have). The frames between take no step. For a smooth picture
// at the PC rate the particles shown between steps are the last two steps' blend at the frame's place between them
// (the pose a step behind, as a fixed-step simulation is shown); the simulation's own positions are put back before
// the next step, so the steps themselves are the console's. The forces given between two steps (a hit's push, the
// game's ApplyForce 0x3C4F30) are kept for the next step, as the console gives them to its next frame.
// TF_RAGDOLL_FIXED_STEP=0: a step every frame, as before; TF_RAGDOLL_SMOOTH=0: the steps shown as they are (30 a
// second); TF_RAGDOLL_STEP_LOG=1: each step's ragdoll, its time and its root particle.
#include "ragdoll_step.h"

#include "port.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_memory.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>
#include "game_version.h" // (TF_GA: the code addresses that differ in the PAL release)

namespace ragdoll_step
{
    namespace
    {
        constexpr uint32_t kUpdate = TF_GA(0x003C9E90u);             // taVerletIntegrator::Update(this, dt)
        constexpr uint32_t kParticleSize = 0x50u, kActive = 0x48u; // (+0x48: Update leaves at once when 0)
        constexpr uint32_t kMaxParticles = 256u;
        PS2Runtime::RecompiledFunction g_update = nullptr;
        bool g_on = true, g_smooth = true, g_log = false;

        struct Doll
        {
            uint32_t count = 0u, particles = 0u;
            uint64_t seen = 0u;          // (the VBlank it was last stepped or shown at)
            float credit = 0.0f;         // 30/R a frame; a step at 1
            float time = 0.0f;           // the frames' time since the last step
            bool before = false;         // a step before the last: the blend's start
            bool shown = false;          // the particles hold the blend, `real` the simulation's own positions
            std::vector<float> real, last, shown_; // (x y z a particle: the simulation's, the last step's start, what is shown)
        };
        std::unordered_map<uint32_t, Doll> g_dolls;

        uint32_t word(const uint8_t *rdram, uint32_t a)
        {
            uint32_t v = 0u;
            if (a >= 0x00100000u && a < PS2_RAM_SIZE - 4u)
                std::memcpy(&v, rdram + a, sizeof(v));
            return v;
        }
        bool validRange(uint32_t a, uint32_t bytes) { return a >= 0x00100000u && a < PS2_RAM_SIZE && bytes < PS2_RAM_SIZE - a; }

        void readPositions(const uint8_t *rdram, const Doll &d, std::vector<float> &out)
        {
            out.resize(3u * d.count);
            for (uint32_t i = 0; i < d.count; ++i)
                std::memcpy(&out[3u * i], rdram + d.particles + kParticleSize * i, 3u * sizeof(float));
        }
        void writePositions(uint8_t *rdram, const Doll &d, const std::vector<float> &in)
        {
            for (uint32_t i = 0; i < d.count && 3u * i + 2u < in.size(); ++i)
                std::memcpy(rdram + d.particles + kParticleSize * i, &in[3u * i], 3u * sizeof(float));
        }

        // (TF_RAGDOLL_STEP_LOG) a step: the doll, its time, its first two particles and the first one's force
        void logStep(const uint8_t *rdram, PS2Runtime *runtime, uint32_t self, uint32_t count, uint32_t particles, float time)
        {
            float a[3] = {}, b[3] = {}, f[3] = {};
            std::memcpy(a, rdram + particles, sizeof(a));
            std::memcpy(b, rdram + particles + kParticleSize, sizeof(b));
            std::memcpy(f, rdram + particles + 0x30u, sizeof(f));
            std::fprintf(stderr, "[ragdoll step] tick %llu doll %08x (%u) time %.4f p0 %.3f %.3f %.3f p1 %.3f %.3f %.3f force %.2f %.2f %.2f\n",
                         static_cast<unsigned long long>(runtime->eeScheduler().currentVSyncTick()), self, count, time, a[0], a[1], a[2], b[0], b[1],
                         b[2], f[0], f[1], f[2]);
        }

        void update(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const int rate = tf::port::levelRate();
            const uint32_t self = getRegU32(ctx, 4);
            const uint32_t count = word(rdram, self), particles = word(rdram, self + 4u);
            const bool entry = ctx->pc == kUpdate;
            uint8_t active = 0u;
            if (validRange(self, 0x60u))
                active = rdram[self + kActive];
            if (g_log && entry && active != 0u && count >= 2u && count <= kMaxParticles && validRange(particles, kParticleSize * count) &&
                (!g_on || rate <= 30))
                logStep(rdram, runtime, self, count, particles, ctx->f[12]);
            if (!g_on || !entry || rate <= 30 || active == 0u || count == 0u || count > kMaxParticles ||
                !validRange(particles, kParticleSize * count))
            {
                if (entry)
                    g_dolls.erase(self); // (at rest, gone or not ours: a later step starts it afresh)
                g_update(rdram, ctx, runtime);
                return;
            }
            const uint64_t tick = runtime->eeScheduler().currentVSyncTick();
            const float share = 30.0f / static_cast<float>(rate);
            Doll &d = g_dolls[self];
            const uint64_t gap = static_cast<uint64_t>(std::max(2, tf::port::vblankRate() / 7)); // (~4 console frames)
            if (d.count != count || d.particles != particles || tick > d.seen + gap)
            {
                d = Doll{};
                d.count = count;
                d.particles = particles;
                d.credit = 1.0f - share; // (a new ragdoll: its first frame a step at once)
            }
            if (d.shown) // (the simulation's own positions back before it goes on)
            {
                // (Unless the game moved them since. All of them - a teleport: it goes on from there. Some of them - TODO 273,
                // Cyclonus' whirlwind grab holds the player's ragdoll by one particle, set to his hand every frame: those
                // keep where the game put them, the others get the simulation's own back. Before, one moved particle left
                // every particle at the blend - the pose a step behind - so each step's work was thrown away the frame
                // after: the held player stood still in his first pose for the whole spin, at every rate above 30.)
                std::vector<float> now;
                readPositions(rdram, d, now);
                size_t moved = 0u;
                for (uint32_t i = 0; i < d.count && 3u * i + 2u < now.size() && 3u * i + 2u < d.shown_.size() && 3u * i + 2u < d.real.size(); ++i)
                {
                    const size_t k = 3u * i;
                    if (now[k] == d.shown_[k] && now[k + 1u] == d.shown_[k + 1u] && now[k + 2u] == d.shown_[k + 2u])
                        continue;
                    ++moved;
                    for (size_t c = 0; c < 3u; ++c)
                    {
                        d.real[k + c] = now[k + c]; // (the game's own place for it: kept)
                        if (k + c < d.last.size())
                            d.last[k + c] = now[k + c]; // (and shown there, not blended from where it was)
                    }
                }
                if (g_log && moved != 0u) // (TF_RAGDOLL_STEP_LOG: the game moved particles between two steps)
                    std::fprintf(stderr, "[ragdoll step] doll %08x: the game moved %zu of %u particles between steps%s\n", self, moved, d.count,
                                 moved >= d.count ? " (all: taken as a teleport)" : " (kept where it put them)");
                if (moved >= d.count)
                    d.before = false;
                else
                    writePositions(rdram, d, d.real);
                d.shown = false;
            }
            d.seen = tick;
            d.time += ctx->f[12];
            d.credit += share;
            const uint32_t ra = getRegU32(ctx, 31);
            if (d.credit >= 0.999f)
            {
                d.credit = std::max(0.0f, d.credit - 1.0f);
                readPositions(rdram, d, d.last); // (where this step starts: the blend's start from now on)
                d.before = true;
                if (g_log)
                    logStep(rdram, runtime, self, count, particles, d.time);
                ctx->f[12] = d.time;
                d.time = 0.0f;
                g_update(rdram, ctx, runtime);
                if (ctx->pc != ra)
                    return; // (stopped part way: shown as it stands)
            }
            else
                ctx->pc = ra; // (no step this frame)
            if (g_smooth && d.before)
            {
                readPositions(rdram, d, d.real);
                d.shown_.assign(d.real.size(), 0.0f);
                const float t = std::clamp(d.credit, 0.0f, 1.0f);
                for (size_t i = 0; i < d.shown_.size(); ++i)
                    d.shown_[i] = i < d.last.size() ? d.last[i] + (d.real[i] - d.last[i]) * t : d.real[i];
                writePositions(rdram, d, d.shown_);
                d.shown = true;
            }
        }
    }

    void install(PS2Runtime &runtime)
    {
        if (const char *v = std::getenv("TF_RAGDOLL_FIXED_STEP"); v && *v == '0')
            g_on = false;
        if (const char *v = std::getenv("TF_RAGDOLL_SMOOTH"); v && *v == '0')
            g_smooth = false;
        g_log = std::getenv("TF_RAGDOLL_STEP_LOG") != nullptr;
        if ((g_on || g_log) && (g_update = runtime.lookupFunction(kUpdate)) != nullptr)
            runtime.registerFunction(kUpdate, update); // (with the step off, the log alone: every frame's step as before)
        else
            g_on = false;
    }
}
