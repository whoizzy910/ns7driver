// NS7Audio.cpp - CoreAudio plug-in that shows the Numark NS7 as a 4-in / 4-out
// audio device. Audio moves through memory shared with ns7d (which owns the
// USB side); CoreAudio's clock follows the NS7's own crystal via the clock
// ns7d publishes. Built on libASPL (MIT).
//
// SPDX-License-Identifier: MIT

#include <aspl/Driver.hpp>

#include <CoreAudio/AudioServerPlugIn.h>
#include <mach/mach_time.h>
#include <xpc/xpc.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>

#include "../src/ns7_shm.h"

namespace {

constexpr UInt32 kRate = 44100;
constexpr UInt32 kChannels = NS7_SHM_CHANNELS;
constexpr uint64_t kMask = NS7_SHM_RING_FRAMES - 1;

// The shared memory, once ns7d has handed it over. Mappings are never
// unmapped, so the IO thread can always use a pointer it loaded.
std::atomic<ns7_shm*> gShm{nullptr};
std::atomic<bool> gIORunning{false};   // so a reconnected ns7d learns IO is already running

ns7_shm* ConnectToDaemon()
{
    xpc_connection_t c = xpc_connection_create_mach_service(NS7_XPC_SERVICE, nullptr, 0);
    if (!c) {
        return nullptr;
    }
    xpc_connection_set_event_handler(c, ^(xpc_object_t) {
    });
    xpc_connection_resume(c);

    xpc_object_t msg = xpc_dictionary_create(nullptr, nullptr, 0);
    xpc_dictionary_set_string(msg, "op", "hello");
    xpc_object_t reply = xpc_connection_send_message_with_reply_sync(c, msg);
    xpc_release(msg);

    ns7_shm* shm = nullptr;
    if (xpc_get_type(reply) == XPC_TYPE_DICTIONARY) {
        xpc_object_t mem = xpc_dictionary_get_value(reply, "shm");
        void* addr = nullptr;
        size_t size = mem ? xpc_shmem_map(mem, &addr) : 0;
        if (size >= sizeof(ns7_shm) && ns7_shm_valid(static_cast<ns7_shm*>(addr))) {
            shm = static_cast<ns7_shm*>(addr);
        }
    }
    xpc_release(reply);
    if (!shm) {
        xpc_connection_cancel(c);
        xpc_release(c);
    }
    return shm;   // the connection is kept open for the life of the mapping
}

AudioStreamBasicDescription FloatFormat()
{
    AudioStreamBasicDescription f = {};
    f.mSampleRate = kRate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
    f.mBitsPerChannel = 32;
    f.mChannelsPerFrame = kChannels;
    f.mBytesPerFrame = 4 * kChannels;
    f.mFramesPerPacket = 1;
    f.mBytesPerPacket = 4 * kChannels;
    return f;
}

// Device whose clock is the NS7's: zero timestamps come from ns7d's clock.
class NS7Device : public aspl::Device
{
public:
    using aspl::Device::Device;

protected:
    OSStatus GetZeroTimeStampImpl(UInt32 clientID,
        Float64* outSampleTime,
        UInt64* outHostTime,
        UInt64* outSeed) override
    {
        ns7_shm* shm = gShm.load(std::memory_order_acquire);
        ns7_clock c;
        if (shm && ns7_clock_read(shm, &c)) {
            ns7_clock_zero(&c, mach_absolute_time(), NS7_SHM_RING_FRAMES, outSampleTime, outHostTime);
            *outSeed = c.epoch;   // changes whenever the NS7 restarts streaming
            return kAudioHardwareNoError;
        }
        return aspl::Device::GetZeroTimeStampImpl(clientID, outSampleTime, outHostTime, outSeed);
    }
};

class NS7Handler : public aspl::ControlRequestHandler, public aspl::IORequestHandler
{
public:
    OSStatus OnStartIO() override
    {
        gIORunning.store(true);
        if (ns7_shm* shm = gShm.load(std::memory_order_acquire)) {
            __atomic_store_n(&shm->client_running, 1u, __ATOMIC_RELEASE);
        }
        return kAudioHardwareNoError;
    }

    void OnStopIO() override
    {
        gIORunning.store(false);
        if (ns7_shm* shm = gShm.load(std::memory_order_acquire)) {
            __atomic_store_n(&shm->client_running, 0u, __ATOMIC_RELEASE);
        }
    }

    // Mixed output of all apps: into the ring at its sample time.
    void OnWriteMixedOutput(const std::shared_ptr<aspl::Stream>&,
        Float64,
        Float64 timestamp,
        const void* bytes,
        UInt32 bytesCount) override
    {
        ns7_shm* shm = gShm.load(std::memory_order_acquire);
        if (!shm || timestamp < 0) {
            return;
        }
        const float* src = static_cast<const float*>(bytes);
        const UInt32 frames = bytesCount / (sizeof(float) * kChannels);
        const uint64_t t = static_cast<uint64_t>(timestamp);
        for (UInt32 f = 0; f < frames; f++) {
            std::memcpy(&shm->out[((t + f) & kMask) * kChannels], src + f * kChannels, sizeof(float) * kChannels);
        }
    }

    // Input for each app: from the ring at its sample time, silence where ns7d
    // hasn't written yet (or the data is older than the ring).
    void OnReadClientInput(const std::shared_ptr<aspl::Client>&,
        const std::shared_ptr<aspl::Stream>&,
        Float64,
        Float64 timestamp,
        void* bytes,
        UInt32 bytesCount) override
    {
        float* dst = static_cast<float*>(bytes);
        const UInt32 frames = bytesCount / (sizeof(float) * kChannels);
        ns7_shm* shm = gShm.load(std::memory_order_acquire);
        if (!shm || timestamp < 0) {
            std::memset(bytes, 0, bytesCount);
            return;
        }
        const uint64_t written = __atomic_load_n(&shm->in_written, __ATOMIC_ACQUIRE);
        const uint64_t t = static_cast<uint64_t>(timestamp);
        for (UInt32 f = 0; f < frames; f++) {
            const uint64_t pos = t + f;
            if (pos < written && written - pos < NS7_SHM_RING_FRAMES - 1024) {
                std::memcpy(dst + f * kChannels, &shm->in[(pos & kMask) * kChannels], sizeof(float) * kChannels);
            } else {
                std::memset(dst + f * kChannels, 0, sizeof(float) * kChannels);
            }
        }
    }
};

// Adds the device when the NS7 is streaming and removes it when it's not.
class NS7Driver : public aspl::DriverRequestHandler
{
public:
    NS7Driver(std::shared_ptr<aspl::Context> context, std::shared_ptr<aspl::Plugin> plugin)
        : context_(std::move(context))
        , plugin_(std::move(plugin))
        , handler_(std::make_shared<NS7Handler>())
    {
    }

    OSStatus OnInitialize() override
    {
        std::thread([this] { Monitor(); }).detach();
        return kAudioHardwareNoError;
    }

private:
    std::shared_ptr<aspl::Device> MakeDevice(ns7_shm* shm)
    {
        aspl::DeviceParameters p;
        p.Name = "Numark NS7";
        p.Manufacturer = "Numark (ns7driver)";
        p.DeviceUID = "ns7driver:15E4:0071";
        p.ModelUID = "ns7driver:ns7";
        p.SampleRate = kRate;
        p.ChannelCount = kChannels;
        p.SafetyOffset = std::max(shm->out_safety, shm->in_safety);
        p.ZeroTimeStampPeriod = NS7_SHM_RING_FRAMES;
        p.ClockIsStable = true;
        p.CanBeDefault = true;
        p.CanBeDefaultForSystemSounds = true;
        p.EnableMixing = true;

        auto device = std::make_shared<NS7Device>(context_, p);

        aspl::StreamParameters out;
        out.Direction = aspl::Direction::Output;
        out.StartingChannel = 1;
        out.Format = FloatFormat();
        out.Latency = shm->out_latency;
        device->AddStreamAsync(out);

        aspl::StreamParameters in;
        in.Direction = aspl::Direction::Input;
        in.StartingChannel = 1;
        in.Format = FloatFormat();
        in.Latency = shm->in_latency;
        device->AddStreamAsync(in);

        device->SetControlHandler(handler_);
        device->SetIOHandler(handler_);
        return device;
    }

    void Monitor()
    {
        uint64_t lastPos = 0;
        int stalled = 0;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));

            ns7_shm* shm = gShm.load(std::memory_order_acquire);
            if (!shm) {
                shm = ConnectToDaemon();
                if (!shm) {
                    continue;
                }
                __atomic_store_n(&shm->client_running, gIORunning.load() ? 1u : 0u, __ATOMIC_RELEASE);
                gShm.store(shm, std::memory_order_release);
                lastPos = 0;
                stalled = 0;
            }

            bool present = __atomic_load_n(&shm->device_present, __ATOMIC_ACQUIRE) != 0;

            // ns7d gone (restarted or crashed): its clock stops moving. Reconnect.
            const uint64_t pos = __atomic_load_n(&shm->clock_pos, __ATOMIC_ACQUIRE);
            stalled = present && pos == lastPos ? stalled + 1 : 0;
            lastPos = pos;
            if (stalled >= 4) {
                present = false;
                gShm.store(nullptr, std::memory_order_release);
            }

            if (present && !added_) {
                if (!device_) {
                    device_ = MakeDevice(shm);
                }
                plugin_->AddDevice(device_);
                added_ = true;
            } else if (!present && added_) {
                plugin_->RemoveDevice(device_);
                added_ = false;
            }
        }
    }

    std::shared_ptr<aspl::Context> context_;
    std::shared_ptr<aspl::Plugin> plugin_;
    std::shared_ptr<NS7Handler> handler_;
    std::shared_ptr<aspl::Device> device_;
    bool added_ = false;
};

std::shared_ptr<aspl::Driver> CreateDriver()
{
    auto context = std::make_shared<aspl::Context>();
    auto plugin = std::make_shared<aspl::Plugin>(context);
    auto driver = std::make_shared<aspl::Driver>(context, plugin);
    static auto handler = std::make_shared<NS7Driver>(context, plugin);
    driver->SetDriverHandler(handler);
    return driver;
}

} // namespace

extern "C" void* NS7AudioEntryPoint(CFAllocatorRef, CFUUIDRef typeUUID)
{
    if (!CFEqual(typeUUID, kAudioServerPlugInTypeUUID)) {
        return nullptr;
    }
    static std::shared_ptr<aspl::Driver> driver = CreateDriver();
    return driver->GetReference();
}
