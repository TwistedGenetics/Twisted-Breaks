#include "Core/MikroUsb.h"
#include "Core/MikroProtocol.h"
#include <atomic>

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
 #include <setupapi.h>
 #include <hidsdi.h>
 #include <hidpi.h>
 #pragma comment (lib, "hid.lib")
 #pragma comment (lib, "setupapi.lib")
#endif

#if JUCE_MAC
 #include <IOKit/hid/IOHIDManager.h>
 #include <IOKit/hid/IOHIDKeys.h>
 #include <IOKit/IOReturn.h>
 #include <IOKit/IOKitLib.h>
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

        /** What the USB bus itself shows from Native Instruments (vendor 0x17CC), whatever driver has it. */
        juce::String usbDevicesFromNI()
        {
            juce::StringArray found;
            for (const char* cls : { "IOUSBHostDevice", "IOUSBDevice" })
            {
                CFMutableDictionaryRef m = IOServiceMatching (cls);
                if (m == nullptr) continue;
                int vid = mikro::kVendorId;
                auto v = CFNumberCreate (kCFAllocatorDefault, kCFNumberIntType, &vid);
                CFDictionarySetValue (m, CFSTR ("idVendor"), v);
                CFRelease (v);
                io_iterator_t it = 0;
                if (IOServiceGetMatchingServices (MACH_PORT_NULL, m, &it) != KERN_SUCCESS) continue;   // consumes m
                while (io_object_t o = IOIteratorNext (it))
                {
                    juce::String name = "?";
                    if (auto ref = (CFStringRef) IORegistryEntryCreateCFProperty (o, CFSTR ("USB Product Name"), kCFAllocatorDefault, 0))
                    {
                        char buf[128] = {};
                        if (CFGetTypeID (ref) == CFStringGetTypeID() && CFStringGetCString (ref, buf, sizeof (buf), kCFStringEncodingUTF8))
                            name = juce::String::fromUTF8 (buf);
                        CFRelease (ref);
                    }
                    int pid = 0;
                    if (auto num = (CFNumberRef) IORegistryEntryCreateCFProperty (o, CFSTR ("idProduct"), kCFAllocatorDefault, 0))
                    {
                        if (CFGetTypeID (num) == CFNumberGetTypeID()) CFNumberGetValue (num, kCFNumberIntType, &pid);
                        CFRelease (num);
                    }
                    found.addIfNotAlreadyThere (name + " (0x" + juce::String::toHexString (pid).paddedLeft ('0', 4) + ")");
                    IOObjectRelease (o);
                }
                IOObjectRelease (it);
            }
            return found.isEmpty() ? juce::String ("nothing from Native Instruments on USB") : "on USB: " + found.joinIntoString (", ");
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

            // Every HID device from Native Instruments: the Mikro may show more than one interface.
            auto match = CFDictionaryCreateMutable (kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            int vid = mikro::kVendorId;
            auto v = CFNumberCreate (kCFAllocatorDefault, kCFNumberIntType, &vid);
            CFDictionarySetValue (match, CFSTR (kIOHIDVendorIDKey), v);
            IOHIDManagerSetDeviceMatching (manager, match);
            CFRelease (v); CFRelease (match);

            // The manager only lists devices once it's scheduled and open.
            IOHIDManagerScheduleWithRunLoop (manager, loop, kCFRunLoopDefaultMode);
            const IOReturn mo = IOHIDManagerOpen (manager, kIOHIDOptionsTypeNone);
            managerOpen = true;

            CFSetRef set = nullptr;
            for (int attempt = 0; attempt < 20; ++attempt)
            {
                set = IOHIDManagerCopyDevices (manager);
                if (set != nullptr && CFSetGetCount (set) > 0) break;
                if (set != nullptr) { CFRelease (set); set = nullptr; }
                juce::Thread::sleep (50);
            }

            // Pick the Mikro's interface with the biggest output report (lights + screen).
            juce::StringArray seen;
            IOHIDDeviceRef best = nullptr;
            int bestOut = -1;
            if (set != nullptr)
            {
                const CFIndex n = CFSetGetCount (set);
                std::vector<const void*> devs ((size_t) n);
                CFSetGetValues (set, devs.data());
                for (auto* d : devs)
                {
                    auto dev = (IOHIDDeviceRef) d;
                    const int pid = intProperty (dev, CFSTR (kIOHIDProductIDKey));
                    const int out = intProperty (dev, CFSTR (kIOHIDMaxOutputReportSizeKey));
                    seen.add (stringProperty (dev, CFSTR (kIOHIDProductKey)) + " (0x" + juce::String::toHexString (pid).paddedLeft ('0', 4)
                              + ", usage page 0x" + juce::String::toHexString (intProperty (dev, CFSTR (kIOHIDPrimaryUsagePageKey)))
                              + ", in " + juce::String (intProperty (dev, CFSTR (kIOHIDMaxInputReportSizeKey))) + " / out " + juce::String (out) + ")");
                    if (pid == mikro::kProductId && out > bestOut) { best = dev; bestOut = out; }
                }
            }
            hidSeen = seen.joinIntoString ("; ");
            if (best == nullptr)
            {
                if (set != nullptr) CFRelease (set);
                error = "no Maschine Mikro MK3 found. HID: " + (seen.isEmpty() ? juce::String ("none from NI") : hidSeen)
                        + ". USB: " + usbDevicesFromNI()
                        + (mo != kIOReturnSuccess ? ". HID manager: " + ioReturnText (mo) : juce::String());
                return false;
            }
            device = best;
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
            if (manager != nullptr)
            {
                if (loop != nullptr) IOHIDManagerUnscheduleFromRunLoop (manager, loop, kCFRunLoopDefaultMode);
                if (managerOpen) IOHIDManagerClose (manager, kIOHIDOptionsTypeNone);
                managerOpen = false;
                CFRelease (manager);
                manager = nullptr;
            }
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
        juce::String product, error, hidSeen;
        bool managerOpen = false;
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
               + ", reports in " + juce::String (impl->maxIn) + " / out " + juce::String (impl->maxOut) + " bytes"
               + " [NI HID: " + impl->hidSeen + "]";
    }

#elif JUCE_WINDOWS
    /*
        Windows: the Mikro is a plain USB HID device. SetupAPI finds it, a
        shared CreateFile opens it (NI's service may hold it: that shows as a
        sharing violation), an overlapped ReadFile loop on our own thread
        takes the input reports (Windows always puts the report id first),
        and WriteFile sends output reports padded to the device's report size.
    */
    namespace
    {
        juce::String winError (DWORD e)
        {
            juce::String what;
            switch (e)
            {
                case ERROR_SHARING_VIOLATION:   what = "held by another app (Native Instruments' background service or Maschine?)"; break;
                case ERROR_ACCESS_DENIED:       what = "access denied (held by another app?)"; break;
                case ERROR_DEVICE_NOT_CONNECTED:
                case ERROR_BAD_COMMAND:
                case ERROR_GEN_FAILURE:         what = "device gone"; break;
                case ERROR_INVALID_PARAMETER:
                case ERROR_INVALID_USER_BUFFER: what = "bad argument (report size?)"; break;
                case WAIT_TIMEOUT:              what = "timed out"; break;
                default:                        what = "error"; break;
            }
            return what + " (" + juce::String ((int) e) + ")";
        }

        struct HidInfo
        {
            juce::String path, product;
            int vid = 0, pid = 0, usagePage = 0, inLen = 0, outLen = 0;
        };

        /** Every HID interface Windows lists from Native Instruments. */
        std::vector<HidInfo> niHidDevices()
        {
            std::vector<HidInfo> found;
            GUID guid;
            HidD_GetHidGuid (&guid);
            HDEVINFO set = SetupDiGetClassDevsW (&guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
            if (set == INVALID_HANDLE_VALUE) return found;

            SP_DEVICE_INTERFACE_DATA ifd {};
            ifd.cbSize = sizeof (ifd);
            for (DWORD i = 0; SetupDiEnumDeviceInterfaces (set, nullptr, &guid, i, &ifd); ++i)
            {
                DWORD need = 0;
                SetupDiGetDeviceInterfaceDetailW (set, &ifd, nullptr, 0, &need, nullptr);
                if (need == 0) continue;
                juce::HeapBlock<char> mem (need, true);
                auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*> (mem.get());
                detail->cbSize = sizeof (SP_DEVICE_INTERFACE_DETAIL_DATA_W);
                if (! SetupDiGetDeviceInterfaceDetailW (set, &ifd, detail, need, nullptr, nullptr)) continue;

                // No access rights needed just to read the ids and report sizes.
                HANDLE h = CreateFileW (detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
                if (h == INVALID_HANDLE_VALUE) continue;
                HIDD_ATTRIBUTES attr {};
                attr.Size = sizeof (attr);
                if (HidD_GetAttributes (h, &attr) && attr.VendorID == mikro::kVendorId)
                {
                    HidInfo info;
                    info.path = juce::String (detail->DevicePath);
                    info.vid = attr.VendorID;
                    info.pid = attr.ProductID;
                    PHIDP_PREPARSED_DATA pp = nullptr;
                    if (HidD_GetPreparsedData (h, &pp))
                    {
                        HIDP_CAPS caps {};
                        if (HidP_GetCaps (pp, &caps) == HIDP_STATUS_SUCCESS)
                        {
                            info.usagePage = caps.UsagePage;
                            info.inLen = caps.InputReportByteLength;
                            info.outLen = caps.OutputReportByteLength;
                        }
                        HidD_FreePreparsedData (pp);
                    }
                    wchar_t name[128] = {};
                    if (HidD_GetProductString (h, name, sizeof (name))) info.product = juce::String (name);
                    found.push_back (info);
                }
                CloseHandle (h);
            }
            SetupDiDestroyDeviceInfoList (set);
            return found;
        }
    }

    struct MikroUsb::Impl final : juce::Thread
    {
        explicit Impl (MikroUsb& o) : juce::Thread ("Mikro MK3 USB"), owner (o) {}
        ~Impl() override { close(); }

        void run() override
        {
            OVERLAPPED ov {};
            ov.hEvent = CreateEventW (nullptr, TRUE, FALSE, nullptr);
            std::vector<uint8_t> buf ((size_t) juce::jmax (64, maxIn), 0);
            while (! threadShouldExit() && handle != INVALID_HANDLE_VALUE)
            {
                ResetEvent (ov.hEvent);
                DWORD got = 0;
                if (! ReadFile (handle, buf.data(), (DWORD) buf.size(), &got, &ov))
                {
                    const DWORD e = GetLastError();
                    if (e != ERROR_IO_PENDING) { lostDevice (e); break; }
                    HANDLE waits[] = { ov.hEvent, stopEvent };
                    const DWORD w = WaitForMultipleObjects (2, waits, FALSE, INFINITE);
                    if (w != WAIT_OBJECT_0) { CancelIo (handle); WaitForSingleObject (ov.hEvent, 500); break; }
                    if (! GetOverlappedResult (handle, &ov, &got, FALSE)) { lostDevice (GetLastError()); break; }
                }
                if (got > 0 && owner.onReport) owner.onReport (buf.data(), (int) got);
            }
            CloseHandle (ov.hEvent);
        }

        void lostDevice (DWORD e)
        {
            if (threadShouldExit()) return;
            error = "lost the Mikro: " + winError (e);
            removed = true;
            if (owner.onRemoved) owner.onRemoved();
        }

        bool open()
        {
            const auto devs = niHidDevices();
            juce::StringArray seen;
            const HidInfo* best = nullptr;
            for (const auto& d : devs)
            {
                seen.add (d.product + " (0x" + juce::String::toHexString (d.pid).paddedLeft ('0', 4)
                          + ", usage page 0x" + juce::String::toHexString (d.usagePage)
                          + ", in " + juce::String (d.inLen) + " / out " + juce::String (d.outLen) + ")");
                if (d.pid == mikro::kProductId && (best == nullptr || d.outLen > best->outLen)) best = &d;
            }
            hidSeen = seen.joinIntoString ("; ");
            if (best == nullptr)
            {
                error = "no Maschine Mikro MK3 found. HID: " + (seen.isEmpty() ? juce::String ("none from NI") : hidSeen);
                return false;
            }
            handle = CreateFileW (best->path.toWideCharPointer(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
            if (handle == INVALID_HANDLE_VALUE)
            {
                error = "found the Mikro, but couldn't open it: " + winError (GetLastError());
                return false;
            }
            maxIn = best->inLen;
            maxOut = best->outLen;
            product = best->product;
            removed = false;
            stopEvent = CreateEventW (nullptr, TRUE, FALSE, nullptr);
            writeEvent = CreateEventW (nullptr, TRUE, FALSE, nullptr);
            startThread (juce::Thread::Priority::high);
            error = {};
            return true;
        }

        void close()
        {
            if (stopEvent != nullptr) SetEvent (stopEvent);
            signalThreadShouldExit();
            stopThread (1000);
            if (handle != INVALID_HANDLE_VALUE) { CloseHandle (handle); handle = INVALID_HANDLE_VALUE; }
            if (stopEvent != nullptr)  { CloseHandle (stopEvent);  stopEvent = nullptr; }
            if (writeEvent != nullptr) { CloseHandle (writeEvent); writeEvent = nullptr; }
        }

        bool write (const std::vector<uint8_t>& rep)
        {
            if (handle == INVALID_HANDLE_VALUE || removed || rep.empty()) { error = "not open"; return false; }
            // Windows wants the full output report length (the longest the device declares).
            std::vector<uint8_t> out (rep);
            if (maxOut > 0 && (int) out.size() < maxOut) out.resize ((size_t) maxOut, 0);

            const juce::ScopedLock sl (writeLock);
            OVERLAPPED ov {};
            ov.hEvent = writeEvent;
            ResetEvent (writeEvent);
            DWORD done = 0;
            BOOL ok = WriteFile (handle, out.data(), (DWORD) out.size(), &done, &ov);
            DWORD e = ok ? ERROR_SUCCESS : GetLastError();
            if (! ok && e == ERROR_IO_PENDING)
            {
                if (WaitForSingleObject (writeEvent, 1000) != WAIT_OBJECT_0) { CancelIo (handle); e = WAIT_TIMEOUT; }
                else if (GetOverlappedResult (handle, &ov, &done, FALSE)) e = ERROR_SUCCESS;
                else e = GetLastError();
            }
            if (e != ERROR_SUCCESS)
            {
                // Some HID stacks take output reports only as a SET_REPORT control transfer.
                if (HidD_SetOutputReport (handle, out.data(), (ULONG) out.size())) return true;
                error = "write of report 0x" + juce::String::toHexString ((int) rep[0]) + " (" + juce::String ((int) rep.size())
                        + " bytes) refused: " + winError (e);
                return false;
            }
            return true;
        }

        MikroUsb& owner;
        HANDLE handle = INVALID_HANDLE_VALUE, stopEvent = nullptr, writeEvent = nullptr;
        juce::CriticalSection writeLock;
        std::atomic<bool> removed { false };
        int maxIn = 0, maxOut = 0;
        juce::String product, error, hidSeen;
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
    bool MikroUsb::isOpen() const noexcept { return impl->handle != INVALID_HANDLE_VALUE && ! impl->removed; }
    bool MikroUsb::write (const std::vector<uint8_t>& r) { return impl->write (r); }
    juce::String MikroUsb::getLastError() const { return impl->error; }
    juce::String MikroUsb::describe() const
    {
        return (impl->product.isNotEmpty() ? impl->product : juce::String ("Maschine Mikro MK3"))
               + ", reports in " + juce::String (impl->maxIn) + " / out " + juce::String (impl->maxOut) + " bytes"
               + " [NI HID: " + impl->hidSeen + "]";
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
    juce::String MikroUsb::getLastError() const { return "the Mikro MK3 USB connection is Mac and Windows only"; }
    juce::String MikroUsb::describe() const { return {}; }
#endif
}
