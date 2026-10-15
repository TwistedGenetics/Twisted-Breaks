#include "Core/MikroUsb.h"
#include "Core/MikroProtocol.h"
#include <atomic>

#if JUCE_MAC
 #include <IOKit/hid/IOHIDManager.h>
 #include <IOKit/hid/IOHIDKeys.h>
 #include <IOKit/IOReturn.h>
 #include <CoreFoundation/CoreFoundation.h>
#endif

namespace tb
{
#if JUCE_MAC
    namespace
    {
        juce::String ioReturnText (IOReturn r)
        {
            juce::String what;
            switch (r)
            {
                case kIOReturnExclusiveAccess: what = "held by another app (Native Instruments' background service or Maschine?)"; break;
                case kIOReturnNotPermitted:    what = "not permitted by macOS"; break;
                case kIOReturnNotPrivileged:   what = "not privileged"; break;
                case kIOReturnNoDevice:        what = "device gone"; break;
                case kIOReturnNotOpen:         what = "not open"; break;
                case kIOReturnBadArgument:     what = "bad argument (report size?)"; break;
                case kIOReturnTimeout:         what = "timed out"; break;
                case kIOReturnUnsupported:     what = "unsupported"; break;
                default:                       what = "error"; break;
            }
            return what + " (0x" + juce::String::toHexString ((int) r) + ")";
        }

        int intProperty (IOHIDDeviceRef d, CFStringRef key)
        {
            int v = 0;
            if (auto ref = (CFNumberRef) IOHIDDeviceGetProperty (d, key))
                if (CFGetTypeID (ref) == CFNumberGetTypeID())
                    CFNumberGetValue (ref, kCFNumberIntType, &v);
            return v;
        }

        juce::String stringProperty (IOHIDDeviceRef d, CFStringRef key)
        {
            if (auto ref = (CFStringRef) IOHIDDeviceGetProperty (d, key))
                if (CFGetTypeID (ref) == CFStringGetTypeID())
                {
                    char buf[256] = {};
                    if (CFStringGetCString (ref, buf, sizeof (buf), kCFStringEncodingUTF8)) return juce::String::fromUTF8 (buf);
                }
            return {};
        }
    }

    struct MikroUsb::Impl final : juce::Thread
    {
        explicit Impl (MikroUsb& o) : juce::Thread ("Mikro MK3 USB"), owner (o) {}

        void run() override
        {
            loop = CFRunLoopGetCurrent();
            ready.signal();
            while (! threadShouldExit())
                CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.05, false);
        }

        static void inputCallback (void* ctx, IOReturn result, void*, IOHIDReportType, uint32_t reportId, uint8_t* report, CFIndex length)
        {
            auto* self = static_cast<Impl*> (ctx);
            if (result != kIOReturnSuccess || length <= 0 || ! self->owner.onReport) return;
            // IOKit normally includes the id; add it if this device's reports don't.
            if (reportId != 0 && report[0] != (uint8_t) reportId && length < 255)
            {
                uint8_t tmp[256];
                tmp[0] = (uint8_t) reportId;
                std::memcpy (tmp + 1, report, (size_t) length);
                self->owner.onReport (tmp, (int) length + 1);
            }
            else self->owner.onReport (report, (int) length);
        }

        static void removalCallback (void* ctx, IOReturn, void*)
        {
            auto* self = static_cast<Impl*> (ctx);
            self->removed = true;
            if (self->owner.onRemoved) self->owner.onRemoved();
        }

        bool open()
        {
            if (! isThreadRunning())
            {
                startThread();
                ready.wait (2000);
            }
            if (loop == nullptr) { error = "couldn't start the USB thread"; return false; }

            manager = IOHIDManagerCreate (kCFAllocatorDefault, kIOHIDOptionsTypeNone);
            if (manager == nullptr) { error = "couldn't create the HID manager"; return false; }
            auto match = CFDictionaryCreateMutable (kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            int vid = mikro::kVendorId, pid = mikro::kProductId;
            auto v = CFNumberCreate (kCFAllocatorDefault, kCFNumberIntType, &vid);
            auto p = CFNumberCreate (kCFAllocatorDefault, kCFNumberIntType, &pid);
            CFDictionarySetValue (match, CFSTR (kIOHIDVendorIDKey), v);
            CFDictionarySetValue (match, CFSTR (kIOHIDProductIDKey), p);
            IOHIDManagerSetDeviceMatching (manager, match);
            CFRelease (v); CFRelease (p); CFRelease (match);

            CFSetRef set = IOHIDManagerCopyDevices (manager);
            if (set == nullptr || CFSetGetCount (set) == 0)
            {
                if (set != nullptr) CFRelease (set);
                error = "no Maschine Mikro MK3 found on USB";
                return false;
            }
            const CFIndex n = CFSetGetCount (set);
            std::vector<const void*> devs ((size_t) n);
            CFSetGetValues (set, devs.data());
            device = (IOHIDDeviceRef) devs[0];
            CFRetain (device);
            CFRelease (set);

            const IOReturn r = IOHIDDeviceOpen (device, kIOHIDOptionsTypeNone);
            if (r != kIOReturnSuccess)
            {
                error = "found the Mikro, but couldn't open it: " + ioReturnText (r);
                CFRelease (device); device = nullptr;
                return false;
            }
            maxIn  = intProperty (device, CFSTR (kIOHIDMaxInputReportSizeKey));
            maxOut = intProperty (device, CFSTR (kIOHIDMaxOutputReportSizeKey));
            product = stringProperty (device, CFSTR (kIOHIDProductKey));
            inBuffer.assign ((size_t) juce::jmax (64, maxIn > 0 ? maxIn : 1024), 0);
            removed = false;
            IOHIDDeviceRegisterInputReportCallback (device, inBuffer.data(), (CFIndex) inBuffer.size(), &Impl::inputCallback, this);
            IOHIDDeviceRegisterRemovalCallback (device, &Impl::removalCallback, this);
            IOHIDDeviceScheduleWithRunLoop (device, loop, kCFRunLoopDefaultMode);
            error = {};
            return true;
        }

        void close()
        {
            if (device != nullptr)
            {
                IOHIDDeviceRegisterInputReportCallback (device, inBuffer.data(), (CFIndex) inBuffer.size(), nullptr, nullptr);
                IOHIDDeviceRegisterRemovalCallback (device, nullptr, nullptr);
                if (loop != nullptr) IOHIDDeviceUnscheduleFromRunLoop (device, loop, kCFRunLoopDefaultMode);
                if (! removed) IOHIDDeviceClose (device, kIOHIDOptionsTypeNone);
                CFRelease (device);
                device = nullptr;
            }
            if (manager != nullptr) { CFRelease (manager); manager = nullptr; }
            stopThread (1000);
            loop = nullptr;
        }

        bool write (const std::vector<uint8_t>& rep)
        {
            if (device == nullptr || removed || rep.empty()) { error = "not open"; return false; }
            const IOReturn r = IOHIDDeviceSetReport (device, kIOHIDReportTypeOutput, rep[0], rep.data(), (CFIndex) rep.size());
            if (r != kIOReturnSuccess)
            {
                error = "write of report 0x" + juce::String::toHexString ((int) rep[0]) + " (" + juce::String ((int) rep.size())
                        + " bytes) refused: " + ioReturnText (r);
                return false;
            }
            return true;
        }

        MikroUsb& owner;
        juce::WaitableEvent ready;
        CFRunLoopRef loop = nullptr;
        IOHIDManagerRef manager = nullptr;
        IOHIDDeviceRef device = nullptr;
        std::vector<uint8_t> inBuffer;
        std::atomic<bool> removed { false };
        int maxIn = 0, maxOut = 0;
        juce::String product, error;
    };

    MikroUsb::MikroUsb() : impl (std::make_unique<Impl> (*this)) {}
    MikroUsb::~MikroUsb() { close(); }
    bool MikroUsb::isSupported() noexcept { return true; }
    bool MikroUsb::open()
    {
        if (isOpen()) return true;
        if (impl->open()) return true;
        const auto e = impl->error;
        impl->close();
        impl->error = e;
        return false;
    }
    void MikroUsb::close() { impl->close(); }
    bool MikroUsb::isOpen() const noexcept { return impl->device != nullptr && ! impl->removed; }
    bool MikroUsb::write (const std::vector<uint8_t>& r) { return impl->write (r); }
    juce::String MikroUsb::getLastError() const { return impl->error; }
    juce::String MikroUsb::describe() const
    {
        return (impl->product.isNotEmpty() ? impl->product : juce::String ("Maschine Mikro MK3"))
               + ", reports in " + juce::String (impl->maxIn) + " / out " + juce::String (impl->maxOut) + " bytes";
    }

#else
    struct MikroUsb::Impl {};
    MikroUsb::MikroUsb() {}
    MikroUsb::~MikroUsb() = default;
    bool MikroUsb::isSupported() noexcept { return false; }
    bool MikroUsb::open() { return false; }
    void MikroUsb::close() {}
    bool MikroUsb::isOpen() const noexcept { return false; }
    bool MikroUsb::write (const std::vector<uint8_t>&) { return false; }
    juce::String MikroUsb::getLastError() const { return "the Mikro MK3 USB connection is Mac only"; }
    juce::String MikroUsb::describe() const { return {}; }
#endif
}
