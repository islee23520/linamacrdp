#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreAudio/AudioHardware.h>
#include <assert.h>
#include <dlfcn.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdio.h>

static int accept_bounded(int listener) {
    fd_set ready;
    FD_ZERO(&ready);
    FD_SET(listener, &ready);
    struct timeval timeout = { 5, 0 };
    assert(select(listener + 1, &ready, NULL, NULL, &timeout) == 1);
    int client = accept(listener, NULL, NULL);
    assert(client >= 0);
    return client;
}

int main(int argc, char **argv) {
    assert(argc == 2);
    void *library = dlopen(argv[1], RTLD_NOW);
    assert(library);
    void *(*factory)(CFAllocatorRef, CFUUIDRef) = dlsym(library, "AudioServerPlugInFactory");
    assert(factory);
    assert(factory(NULL, kAudioServerPlugInDriverInterfaceUUID) == NULL);
    AudioServerPlugInDriverRef driver = factory(NULL, kAudioServerPlugInTypeUUID);
    assert(driver);
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    assert(listener >= 0);
    int reuse = 1;
    assert(setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == 0);
    struct sockaddr_in address = { .sin_family = AF_INET, .sin_port = htons(49228) };
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(listener, (struct sockaddr *)&address, sizeof(address)) == 0);
    assert(listen(listener, 1) == 0);
    AudioServerPlugInHostInterface host = {0};
    assert((*driver)->Initialize(driver, &host) == noErr);
    int client = accept_bounded(listener);
    const unsigned char old_audio[960] = {1, 2};
    assert(send(client, old_audio, sizeof(old_audio), 0) == sizeof(old_audio));
    close(client);
    client = accept_bounded(listener); // receiver discarded the previous connection's ring
    AudioObjectPropertyAddress a = {kAudioPlugInPropertyDeviceList, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    UInt32 bytes = 0, actual = 0;
    AudioObjectID id = 0;
    assert((*driver)->GetPropertyDataSize(driver, kAudioObjectPlugInObject, 0, &a, 0, NULL, &bytes) == noErr && bytes == sizeof(id));
    assert((*driver)->GetPropertyData(driver, kAudioObjectPlugInObject, 0, &a, 0, NULL, bytes, &actual, &id) == noErr && id == 2);
    a.mSelector = kAudioPlugInPropertyTranslateUIDToDevice;
    CFStringRef uid = CFSTR("com.macrdp.microphone");
    assert((*driver)->GetPropertyData(driver, kAudioObjectPlugInObject, 0, &a, sizeof(CFStringRef), &uid, sizeof(id), &actual, &id) == noErr && id == 2);
    a.mSelector = kAudioDevicePropertyStreams;
    a.mScope = kAudioObjectPropertyScopeInput;
    assert((*driver)->GetPropertyData(driver, 2, 0, &a, 0, NULL, sizeof(id), &actual, &id) == noErr && id == 3);
    a.mScope = kAudioObjectPropertyScopeOutput;
    assert((*driver)->GetPropertyDataSize(driver, 2, 0, &a, 0, NULL, &bytes) == noErr && bytes == 0);
    a.mScope = kAudioObjectPropertyScopeGlobal;
    a.mSelector = kAudioDevicePropertyNominalSampleRate;
    Float64 rate = 0;
    assert((*driver)->GetPropertyData(driver, 2, 0, &a, 0, NULL, sizeof(rate), &actual, &rate) == noErr && rate == 48000);
    a.mSelector = kAudioStreamPropertyDirection;
    UInt32 value = 0;
    assert((*driver)->GetPropertyData(driver, 3, 0, &a, 0, NULL, sizeof(value), &actual, &value) == noErr && value == 1);
    a.mSelector = kAudioStreamPropertyLatency;
    assert((*driver)->HasProperty(driver, 3, 0, &a));
    assert((*driver)->GetPropertyDataSize(driver, 3, 0, &a, 0, NULL, &bytes) == noErr && bytes == sizeof(value));
    a.mSelector = kAudioStreamPropertyPhysicalFormat;
    AudioStreamBasicDescription format;
    assert((*driver)->GetPropertyData(driver, 3, 0, &a, 0, NULL, sizeof(format), &actual, &format) == noErr);
    assert(format.mChannelsPerFrame == 1 && format.mBitsPerChannel == 16 && format.mSampleRate == 48000);
    Boolean do_it = false, in_place = false;
    assert((*driver)->WillDoIOOperation(driver, 2, 0, kAudioServerPlugInIOOperationReadInput, &do_it, &in_place) == noErr && do_it && in_place);
    int16_t buffer[480];
    AudioServerPlugInIOCycleInfo cycle = {0};
    for (int i = 0; i < 480; i++) buffer[i] = 123;
    assert((*driver)->DoIOOperation(driver, 2, 3, 0, kAudioServerPlugInIOOperationReadInput, 480, &cycle, buffer, NULL) == noErr);
    for (int i = 0; i < 480; i++) assert(buffer[i] == 0);
    close(client);
    close(listener);
    puts("factory, device/stream properties, format, reconnect discard, and silent underrun: OK");
    return 0;
}
