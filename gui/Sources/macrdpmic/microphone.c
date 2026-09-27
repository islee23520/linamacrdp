#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreAudio/AudioHardwareBase.h>
#include <CoreAudio/AudioHardware.h>
#include <CoreFoundation/CFPlugInCOM.h>
#include <mach/mach_time.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <unistd.h>

// Producer contract: listen on 127.0.0.1:49228; send continuous 48000 Hz,
// mono signed 16-bit little-endian PCM, without headers or packet framing.
// This plug-in is a TCP client. Socket reads never run on a HAL IO thread.
enum { DEVICE = 2, STREAM = 3, RATE = 48000, RING = 16384, PORT = 49228, PERIOD = 12000 };
static _Atomic int16_t samples[RING];
static _Atomic uint64_t written, consumed;
static _Atomic uint32_t running;
static _Atomic uint32_t refs = 1;
static uint64_t origin, ticks_per_period;

static void *receiver(void *unused) {
    (void)unused;
    for (;;) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) { sleep(1); continue; }
        struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(PORT) };
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        // A missing listener must not stall initialization or the IO callback.
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            atomic_store_explicit(&consumed, atomic_load_explicit(&written, memory_order_acquire), memory_order_release);
            uint8_t bytes[4096];
            uint8_t low = 0;
            int partial = 0;
            for (;;) {
                ssize_t count = recv(fd, bytes, sizeof(bytes), 0);
                if (count <= 0) break;
                for (ssize_t i = 0; i < count; ++i) {
                    if (!partial) { low = bytes[i]; partial = 1; continue; }
                    int16_t value = (int16_t)((uint16_t)low | ((uint16_t)bytes[i] << 8));
                    partial = 0;
                    uint64_t w = atomic_load_explicit(&written, memory_order_relaxed);
                    uint64_t r = atomic_load_explicit(&consumed, memory_order_acquire);
                    if (w - r >= RING) continue; // drop newest instead of adding latency
                    atomic_store_explicit(&samples[w % RING], value, memory_order_relaxed);
                    atomic_store_explicit(&written, w + 1, memory_order_release);
                }
            }
            atomic_store_explicit(&consumed, atomic_load_explicit(&written, memory_order_acquire), memory_order_release);
        }
        close(fd);
        sleep(1);
    }
    return NULL;
}

static HRESULT query(void *driver, REFIID iid, LPVOID *out);
static ULONG retain(void *driver);
static ULONG release(void *driver);
static OSStatus initialize(AudioServerPlugInDriverRef driver, AudioServerPlugInHostRef host);
static OSStatus create(AudioServerPlugInDriverRef driver, CFDictionaryRef desc, const AudioServerPlugInClientInfo *client, AudioObjectID *id);
static OSStatus destroy(AudioServerPlugInDriverRef driver, AudioObjectID id);
static OSStatus client_change(AudioServerPlugInDriverRef driver, AudioObjectID id, const AudioServerPlugInClientInfo *client);
static OSStatus configure(AudioServerPlugInDriverRef driver, AudioObjectID id, UInt64 action, void *info);
static Boolean has(AudioServerPlugInDriverRef driver, AudioObjectID id, pid_t pid, const AudioObjectPropertyAddress *address);
static OSStatus settable(AudioServerPlugInDriverRef driver, AudioObjectID id, pid_t pid, const AudioObjectPropertyAddress *address, Boolean *out);
static OSStatus size(AudioServerPlugInDriverRef driver, AudioObjectID id, pid_t pid, const AudioObjectPropertyAddress *address, UInt32 qs, const void *qualifier, UInt32 *out);
static OSStatus get(AudioServerPlugInDriverRef driver, AudioObjectID id, pid_t pid, const AudioObjectPropertyAddress *address, UInt32 qs, const void *qualifier, UInt32 capacity, UInt32 *actual, void *out);
static OSStatus set(AudioServerPlugInDriverRef driver, AudioObjectID id, pid_t pid, const AudioObjectPropertyAddress *address, UInt32 qs, const void *qualifier, UInt32 capacity, const void *data);
static OSStatus start(AudioServerPlugInDriverRef driver, AudioObjectID id, UInt32 client);
static OSStatus stop(AudioServerPlugInDriverRef driver, AudioObjectID id, UInt32 client);
static OSStatus timestamp(AudioServerPlugInDriverRef driver, AudioObjectID id, UInt32 client, Float64 *sample, UInt64 *host, UInt64 *seed);
static OSStatus will(AudioServerPlugInDriverRef driver, AudioObjectID id, UInt32 client, UInt32 operation, Boolean *do_it, Boolean *in_place);
static OSStatus begin_end(AudioServerPlugInDriverRef driver, AudioObjectID id, UInt32 client, UInt32 operation, UInt32 frames, const AudioServerPlugInIOCycleInfo *cycle);
static OSStatus io(AudioServerPlugInDriverRef driver, AudioObjectID id, AudioObjectID stream, UInt32 client, UInt32 operation, UInt32 frames, const AudioServerPlugInIOCycleInfo *cycle, void *main, void *secondary);

static AudioServerPlugInDriverInterface interface = {
    NULL, query, retain, release, initialize, create, destroy, client_change,
    client_change, configure, configure, has, settable, size, get, set,
    start, stop, timestamp, will, begin_end, io, begin_end
};
static AudioServerPlugInDriverInterface *driver_interface = &interface;

void *AudioServerPlugInFactory(CFAllocatorRef allocator, CFUUIDRef type) {
    (void)allocator;
    return CFEqual(type, kAudioServerPlugInTypeUUID) ? &driver_interface : NULL;
}

static HRESULT query(void *driver, REFIID iid, LPVOID *out) {
    if (driver != &driver_interface || !out) return E_NOINTERFACE;
    CFUUIDRef uuid = CFUUIDCreateFromUUIDBytes(NULL, iid);
    Boolean match = CFEqual(uuid, kAudioServerPlugInDriverInterfaceUUID) || CFEqual(uuid, IUnknownUUID);
    CFRelease(uuid);
    *out = match ? driver : NULL;
    if (match) retain(driver);
    return match ? S_OK : E_NOINTERFACE;
}
static ULONG retain(void *driver) { (void)driver; return atomic_fetch_add(&refs, 1) + 1; }
static ULONG release(void *driver) { (void)driver; return atomic_fetch_sub(&refs, 1) - 1; }
static OSStatus initialize(AudioServerPlugInDriverRef driver, AudioServerPlugInHostRef host) {
    (void)driver; (void)host;
    mach_timebase_info_data_t scale;
    mach_timebase_info(&scale);
    ticks_per_period = (uint64_t)((long double)PERIOD * 1000000000.0L * scale.denom / (RATE * scale.numer));
    origin = mach_absolute_time();
    pthread_t thread;
    if (pthread_create(&thread, NULL, receiver, NULL) != 0) return kAudioHardwareUnspecifiedError;
    pthread_detach(thread);
    return noErr;
}
static OSStatus create(AudioServerPlugInDriverRef d, CFDictionaryRef desc, const AudioServerPlugInClientInfo *c, AudioObjectID *id) {
    (void)d; (void)desc; (void)c; (void)id; return kAudioHardwareUnsupportedOperationError;
}
static OSStatus destroy(AudioServerPlugInDriverRef d, AudioObjectID id) {
    (void)d; (void)id; return kAudioHardwareUnsupportedOperationError;
}
static OSStatus client_change(AudioServerPlugInDriverRef d, AudioObjectID id, const AudioServerPlugInClientInfo *c) {
    (void)d; (void)c; return id == DEVICE ? noErr : kAudioHardwareBadObjectError;
}
static OSStatus configure(AudioServerPlugInDriverRef d, AudioObjectID id, UInt64 action, void *info) {
    (void)d; (void)id; (void)action; (void)info; return kAudioHardwareUnsupportedOperationError;
}

static AudioStreamBasicDescription format(void) {
    return (AudioStreamBasicDescription){ RATE, kAudioFormatLinearPCM,
        kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked, 2, 1, 2, 1, 16, 0 };
}
static Boolean scope_ok(AudioObjectID id, AudioObjectPropertyScope scope) {
    return scope == kAudioObjectPropertyScopeGlobal ||
        (scope == kAudioObjectPropertyScopeInput && id == DEVICE);
}
static UInt32 property_size(AudioObjectID id, AudioObjectPropertySelector selector, AudioObjectPropertyScope scope) {
    if (!scope_ok(id, scope)) {
        if (id == DEVICE && scope == kAudioObjectPropertyScopeOutput && selector == kAudioDevicePropertyStreams) return 0;
        return UINT32_MAX;
    }
    switch (selector) {
        case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner: case kAudioObjectPropertyOwnedObjects:
        case kAudioDevicePropertyStreams: case kAudioObjectPropertyControlList:
        case kAudioPlugInPropertyDeviceList: case kAudioPlugInPropertyTranslateUIDToDevice:
        case kAudioDevicePropertyTransportType: case kAudioDevicePropertyDeviceIsAlive:
        case kAudioDevicePropertyDeviceIsRunning: case kAudioDevicePropertyDeviceCanBeDefaultDevice:
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: case kAudioDevicePropertyLatency:
        case kAudioDevicePropertySafetyOffset: case kAudioDevicePropertyZeroTimeStampPeriod:
        case kAudioDevicePropertyClockDomain: case kAudioDevicePropertyBufferFrameSize:
        case kAudioStreamPropertyIsActive: case kAudioStreamPropertyDirection:
        case kAudioStreamPropertyTerminalType: case kAudioStreamPropertyStartingChannel:
            return sizeof(UInt32);
        case kAudioObjectPropertyName: case kAudioObjectPropertyManufacturer:
        case kAudioDevicePropertyDeviceUID: case kAudioDevicePropertyModelUID:
        case kAudioPlugInPropertyResourceBundle: case kAudioPlugInPropertyBundleID:
            return sizeof(CFStringRef);
        case kAudioDevicePropertyNominalSampleRate: return sizeof(Float64);
        case kAudioDevicePropertyAvailableNominalSampleRates: return sizeof(AudioValueRange);
        case kAudioStreamPropertyVirtualFormat: case kAudioStreamPropertyPhysicalFormat:
            return sizeof(AudioStreamBasicDescription);
        case kAudioStreamPropertyAvailableVirtualFormats: case kAudioStreamPropertyAvailablePhysicalFormats:
            return sizeof(AudioStreamRangedDescription);
        case kAudioDevicePropertyStreamConfiguration: return sizeof(AudioBufferList);
        default: return UINT32_MAX;
    }
}
static Boolean has(AudioServerPlugInDriverRef d, AudioObjectID id, pid_t pid, const AudioObjectPropertyAddress *a) {
    (void)d; (void)pid;
    if (!a || (id != kAudioObjectPlugInObject && id != DEVICE && id != STREAM)) return false;
    UInt32 s = a->mSelector;
    if (s == kAudioObjectPropertyBaseClass || s == kAudioObjectPropertyClass || s == kAudioObjectPropertyOwner || s == kAudioObjectPropertyName || s == kAudioObjectPropertyManufacturer || s == kAudioObjectPropertyOwnedObjects)
        return scope_ok(id, a->mScope);
    if (id == kAudioObjectPlugInObject)
        return a->mScope == kAudioObjectPropertyScopeGlobal &&
            (s == kAudioPlugInPropertyDeviceList || s == kAudioPlugInPropertyTranslateUIDToDevice ||
             s == kAudioPlugInPropertyResourceBundle || s == kAudioPlugInPropertyBundleID);
    if (id == DEVICE) {
        if (a->mScope == kAudioObjectPropertyScopeOutput)
            return s == kAudioDevicePropertyStreams || s == kAudioDevicePropertyStreamConfiguration;
        if (!scope_ok(id, a->mScope)) return false;
        return s == kAudioDevicePropertyDeviceUID || s == kAudioDevicePropertyModelUID ||
            s == kAudioDevicePropertyTransportType || s == kAudioDevicePropertyClockDomain ||
            s == kAudioDevicePropertyDeviceIsAlive || s == kAudioDevicePropertyDeviceIsRunning ||
            s == kAudioDevicePropertyDeviceCanBeDefaultDevice || s == kAudioDevicePropertyDeviceCanBeDefaultSystemDevice ||
            s == kAudioDevicePropertyLatency || s == kAudioDevicePropertyStreams ||
            s == kAudioObjectPropertyControlList || s == kAudioDevicePropertySafetyOffset ||
            s == kAudioDevicePropertyNominalSampleRate || s == kAudioDevicePropertyAvailableNominalSampleRates ||
            s == kAudioDevicePropertyZeroTimeStampPeriod || s == kAudioDevicePropertyBufferFrameSize ||
            s == kAudioDevicePropertyStreamConfiguration;
    }
    return a->mScope == kAudioObjectPropertyScopeGlobal && property_size(id,s,a->mScope) != UINT32_MAX &&
        (s == kAudioStreamPropertyIsActive || s == kAudioStreamPropertyDirection || s == kAudioStreamPropertyTerminalType || s == kAudioStreamPropertyStartingChannel || s == kAudioStreamPropertyLatency || s == kAudioStreamPropertyVirtualFormat || s == kAudioStreamPropertyPhysicalFormat || s == kAudioStreamPropertyAvailableVirtualFormats || s == kAudioStreamPropertyAvailablePhysicalFormats);
}
static OSStatus settable(AudioServerPlugInDriverRef d, AudioObjectID id, pid_t pid, const AudioObjectPropertyAddress *a, Boolean *out) {
    if (!out || !a) return kAudioHardwareIllegalOperationError;
    if (!has(d,id,pid,a)) return kAudioHardwareUnknownPropertyError;
    *out = false;
    return noErr;
}
static OSStatus size(AudioServerPlugInDriverRef d, AudioObjectID id, pid_t pid, const AudioObjectPropertyAddress *a, UInt32 qs, const void *q, UInt32 *out) {
    (void)qs; (void)q;
    if (!out || !a) return kAudioHardwareIllegalOperationError;
    if (!has(d,id,pid,a)) return kAudioHardwareUnknownPropertyError;
    UInt32 s = a->mSelector;
    if (s == kAudioObjectPropertyOwnedObjects) { *out = (id == kAudioObjectPlugInObject || (id == DEVICE && a->mScope != kAudioObjectPropertyScopeOutput)) ? sizeof(AudioObjectID) : 0; return noErr; }
    if (s == kAudioPlugInPropertyDeviceList || s == kAudioDevicePropertyStreams) { *out = a->mScope == kAudioObjectPropertyScopeOutput ? 0 : sizeof(AudioObjectID); return noErr; }
    if (s == kAudioObjectPropertyControlList) { *out = 0; return noErr; }
    if (s == kAudioDevicePropertyStreamConfiguration && a->mScope == kAudioObjectPropertyScopeOutput) { *out = sizeof(UInt32); return noErr; }
    *out = property_size(id,s,a->mScope);
    return *out == UINT32_MAX ? kAudioHardwareUnknownPropertyError : noErr;
}
static OSStatus get(AudioServerPlugInDriverRef d, AudioObjectID id, pid_t pid, const AudioObjectPropertyAddress *a, UInt32 qs, const void *q, UInt32 capacity, UInt32 *actual, void *out) {
    if (!a || !actual || !out) return kAudioHardwareIllegalOperationError;
    UInt32 n;
    OSStatus status = size(d,id,pid,a,qs,q,&n);
    if (status) return status;
    if (capacity < n) return kAudioHardwareBadPropertySizeError;
    *actual = n;
    UInt32 s = a->mSelector;
    if (s == kAudioObjectPropertyOwnedObjects || s == kAudioPlugInPropertyDeviceList || s == kAudioDevicePropertyStreams) {
        if (n) *(AudioObjectID *)out = id == kAudioObjectPlugInObject ? DEVICE : STREAM;
    } else if (s == kAudioPlugInPropertyTranslateUIDToDevice) {
        *(AudioObjectID *)out = qs == sizeof(CFStringRef) && q && CFEqual(*(CFStringRef *)q, CFSTR("com.macrdp.microphone")) ? DEVICE : kAudioObjectUnknown;
    } else if (s == kAudioObjectPropertyBaseClass) {
        *(UInt32 *)out = id == kAudioObjectPlugInObject ? kAudioObjectClassID : (id == DEVICE ? kAudioObjectClassID : kAudioObjectClassID);
    } else if (s == kAudioObjectPropertyClass) {
        *(UInt32 *)out = id == kAudioObjectPlugInObject ? kAudioPlugInClassID : (id == DEVICE ? kAudioDeviceClassID : kAudioStreamClassID);
    } else if (s == kAudioObjectPropertyOwner) {
        *(UInt32 *)out = id == STREAM ? DEVICE : (id == DEVICE ? kAudioObjectPlugInObject : kAudioObjectSystemObject);
    } else if (s == kAudioObjectPropertyName || s == kAudioObjectPropertyManufacturer || s == kAudioPlugInPropertyResourceBundle || s == kAudioPlugInPropertyBundleID || s == kAudioDevicePropertyDeviceUID || s == kAudioDevicePropertyModelUID) {
        CFStringRef text = s == kAudioObjectPropertyName ? CFSTR("macrdp Microphone") :
            (s == kAudioDevicePropertyDeviceUID ? CFSTR("com.macrdp.microphone") :
            (s == kAudioDevicePropertyModelUID ? CFSTR("com.macrdp.microphone.model") :
            (s == kAudioPlugInPropertyResourceBundle ? CFSTR("") :
            (s == kAudioPlugInPropertyBundleID ? CFSTR("com.macrdp.microphone") : CFSTR("macrdp")))));
        *(CFStringRef *)out = CFRetain(text);
    } else if (s == kAudioDevicePropertyNominalSampleRate) {
        *(Float64 *)out = RATE;
    } else if (s == kAudioDevicePropertyAvailableNominalSampleRates) {
        *(AudioValueRange *)out = (AudioValueRange){RATE,RATE};
    } else if (s == kAudioStreamPropertyVirtualFormat || s == kAudioStreamPropertyPhysicalFormat) {
        *(AudioStreamBasicDescription *)out = format();
    } else if (s == kAudioStreamPropertyAvailableVirtualFormats || s == kAudioStreamPropertyAvailablePhysicalFormats) {
        *(AudioStreamRangedDescription *)out = (AudioStreamRangedDescription){format(), {RATE,RATE}};
    } else if (s == kAudioDevicePropertyStreamConfiguration) {
        AudioBufferList *list = out;
        list->mNumberBuffers = a->mScope == kAudioObjectPropertyScopeOutput ? 0 : 1;
        if (list->mNumberBuffers) list->mBuffers[0] = (AudioBuffer){1,0,NULL};
    } else if (n == sizeof(UInt32)) {
        UInt32 value = 0;
        switch (s) {
            case kAudioDevicePropertyTransportType: value = kAudioDeviceTransportTypeVirtual; break;
            case kAudioDevicePropertyDeviceIsAlive: case kAudioDevicePropertyDeviceCanBeDefaultDevice:
            case kAudioStreamPropertyIsActive: case kAudioStreamPropertyDirection:
            case kAudioStreamPropertyStartingChannel: value = 1; break;
            case kAudioDevicePropertyDeviceIsRunning: value = atomic_load(&running) != 0; break;
            case kAudioDevicePropertyZeroTimeStampPeriod: value = PERIOD; break;
            case kAudioDevicePropertyBufferFrameSize: value = 480; break;
            case kAudioStreamPropertyTerminalType: value = kAudioStreamTerminalTypeMicrophone; break;
        }
        *(UInt32 *)out = value;
    }
    return noErr;
}
static OSStatus set(AudioServerPlugInDriverRef d, AudioObjectID id, pid_t pid, const AudioObjectPropertyAddress *a, UInt32 qs, const void *q, UInt32 capacity, const void *data) {
    (void)d;(void)id;(void)pid;(void)a;(void)qs;(void)q;(void)capacity;(void)data;
    return kAudioHardwareUnsupportedOperationError;
}
static OSStatus start(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client) {
    (void)d;(void)client;
    if (id != DEVICE) return kAudioHardwareBadObjectError;
    atomic_fetch_add(&running,1); return noErr;
}
static OSStatus stop(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client) {
    (void)d;(void)client;
    if (id != DEVICE) return kAudioHardwareBadObjectError;
    uint32_t count = atomic_load(&running);
    while (count && !atomic_compare_exchange_weak(&running,&count,count-1)) {}
    return noErr;
}
static OSStatus timestamp(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client, Float64 *sample, UInt64 *host, UInt64 *seed) {
    (void)d;(void)client;
    if (id != DEVICE) return kAudioHardwareBadObjectError;
    if (!sample || !host || !seed) return kAudioHardwareIllegalOperationError;
    uint64_t period = (mach_absolute_time() - origin) / ticks_per_period;
    *sample = (Float64)(period * PERIOD);
    *host = origin + period * ticks_per_period;
    *seed = 1;
    return noErr;
}
static OSStatus will(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client, UInt32 operation, Boolean *do_it, Boolean *in_place) {
    (void)d;(void)client;
    if (id != DEVICE) return kAudioHardwareBadObjectError;
    if (!do_it || !in_place) return kAudioHardwareIllegalOperationError;
    *do_it = operation == kAudioServerPlugInIOOperationReadInput;
    *in_place = *do_it;
    return noErr;
}
static OSStatus begin_end(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client, UInt32 operation, UInt32 frames, const AudioServerPlugInIOCycleInfo *cycle) {
    (void)d;(void)client;(void)operation;(void)frames;(void)cycle;
    return id == DEVICE ? noErr : kAudioHardwareBadObjectError;
}
static OSStatus io(AudioServerPlugInDriverRef d, AudioObjectID id, AudioObjectID stream, UInt32 client, UInt32 operation, UInt32 frames, const AudioServerPlugInIOCycleInfo *cycle, void *main, void *secondary) {
    (void)d;(void)client;(void)cycle;(void)secondary;
    if (id != DEVICE || stream != STREAM || operation != kAudioServerPlugInIOOperationReadInput || !main) return kAudioHardwareIllegalOperationError;
    int16_t *output = main;
    uint64_t r = atomic_load_explicit(&consumed, memory_order_relaxed);
    uint64_t w = atomic_load_explicit(&written, memory_order_acquire);
    for (UInt32 i=0; i<frames; ++i) output[i] = r < w ? atomic_load_explicit(&samples[(r++) % RING], memory_order_relaxed) : 0;
    uint64_t previous = atomic_load_explicit(&consumed, memory_order_relaxed);
    while (previous < r && !atomic_compare_exchange_weak_explicit(&consumed, &previous, r, memory_order_release, memory_order_relaxed)) {}
    return noErr;
}
