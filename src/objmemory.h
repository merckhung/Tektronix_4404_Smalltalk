
#pragma once

#include "realwordmemory.h"
#include "hal.h"
#include "filesystem.h"
#include <cstdint>
#include <map>
#include <vector>
#include <iostream>

#define RUNTIME_CHECK(cond) if (!(cond)) { fprintf(stderr, "RUNTIME_CHECK failed: %s at %s:%d (oop=%d, fetchWordLengthOf=%d, index=%d)\n", #cond, __FILE__, __LINE__, objectPointer, fetchWordLengthOf(objectPointer), fieldIndex); }

class IGCNotification
{
public:
    virtual void prepareForCollection() = 0;
    virtual void collectionCompleted() = 0;
};

class ObjectMemory
{
public:
    static const int ObjectTableSize = 131072;
    static const int ObjectTableSegment = 0;
    static const int ObjectTableStart = 0;
    
    static const int FirstHeapSegment = 2;
    static const int LastHeapSegment = 4095;
    static const int HeapSegmentCount = 4096;
    
    static const int HeaderSize = 2;
    static const int NonPointer = 0;
    
    // Standard Smalltalk-80 oops (placeholders, will be calibrated)
    int NilPointer = 2;
    int FalsePointer = 4;
    int TruePointer = 6;
    int SchedulerAssociationPointer = 8;
    int ClassSmallInteger = 10; // Placeholder
    int ClassCompiledMethod = 20; // Placeholder

    ObjectMemory(IHardwareAbstractionLayer *halInterface);
    ObjectMemory(IHardwareAbstractionLayer *halInterface, IGCNotification *notification);

    void setImageType(ImageType type) { imageType = type; }
    ImageType getImageType() { return imageType; }
    std::uint32_t getObjectTableLength() { return objectTableLength; }
    bool loadSnapshot(IFileSystem *fileSystem, const char *fileName);
    bool saveSnapshot(IFileSystem *fileSystem, const char *fileName);

    int fetchPointer_ofObject(int fieldIndex, int objectPointer);
    int storePointer_ofObject_withValue(int fieldIndex, int objectPointer, int valuePointer);
    int fetchWord_ofObject(int wordIndex, int objectPointer);
    int storeWord_ofObject_withValue(int wordIndex, int objectPointer, int valueWord);
    int fetchByte_ofObject(int byteIndex, int objectPointer);
    int storeByte_ofObject_withValue(int byteIndex, int objectPointer, int valueByte);
    
    inline int readRawHeapWord(int objectPointer, int offset) {
        return heapChunkOf_word(objectPointer, offset);
    }
    
    int fetchWordLengthOf(int objectPointer);
    int fetchByteLengthOf(int objectPointer);
    bool isDisplayBitmap(int objectPointer);


    inline bool isIntegerObject(int objectPointer) { 
        return (objectPointer & 1) != 0; 
    }
    inline int integerValueOf(int objectPointer) { 
        return (std::int16_t)(objectPointer & 0xfffe)/2; 
    }
    inline int integerObjectOf(int value) { 
        return (std::uint16_t) ((value << 1) | 1); 
    }
    inline bool isIntegerValue(int value) { return value >= -16384 && value <= 16383; }

    inline int fetchClassOf(int objectPointer) {
        if (isIntegerObject(objectPointer)) return ClassSmallInteger;
        return classBitsOf(objectPointer);
    }

    bool hasObject(int objectPointer);
    void increaseReferencesTo(int objectPointer);
    void decreaseReferencesTo(int objectPointer);

    // OT bits access
    inline int sizeBitsOf(int objectPointer) { return heapChunkOf_word(objectPointer, 0); }
    inline int classBitsOf(int objectPointer) { 
        int cls = heapChunkOf_word(objectPointer, 1); 
        if (imageType == ImageType::Tektronix) return cls ^ 1;
        return cls;
    }
    inline int classBitsOf_put(int objectPointer, int value) { 
        if (imageType == ImageType::Tektronix) value ^= 1;
        return heapChunkOf_word_put(objectPointer, 1, value); 
    }
    
    inline int segmentBitsOf(int objectPointer) { return objectSegments[objectPointer & ~1]; }
    inline int locationBitsOf(int objectPointer) { return objectLocations[objectPointer & ~1]; }
    
    inline int freeBitOf(int objectPointer) { 
        return ot_bits_to(objectPointer, 10, 10); 
    }
    inline int pointerBitOf(int objectPointer) { 
        return ot_bits_to(objectPointer, 9, 9); 
    }
    inline int oddBitOf(int objectPointer) { return ot_bits_to(objectPointer, 8, 8); }
    inline int countBitsOf(int objectPointer) { return ot_bits_to(objectPointer, 0, 7); }

    int allocateChunk(int classPointer, int length, int allocationType);
    int instantiateClass_withWords(int classPointer, int length);
    int instantiateClass_withBytes(int classPointer, int length);
    int instantiateClass_withPointers(int classPointer, int length);

    void addRoot(int rootObjectPointer);

    int ot(int objectPointer);
    void garbageCollect();
    int oopsLeft();
    int coreLeft();
    int initialInstanceOf(int classPointer);
    int instanceAfter(int objectPointer);
    void swapPointersOf_and(int p1, int p2);

private:
    IHardwareAbstractionLayer *hal;
    IGCNotification *gcNotification;
    RealWordMemory wordMemory;
    ImageType imageType;
    long ot_offset;
    uint32_t objectTableLength;

    std::uint16_t objectSegments[ObjectTableSize];
    std::uint16_t objectLocations[ObjectTableSize];

    int lastAllocatedSegment;
    int lastAllocatedWord;
    std::map<int, std::vector<std::uint16_t>> virtualBitmaps;
    std::vector<int> gcRoots;
    std::vector<bool> gcMarked;



    inline int heapChunkOf_word(int objectPointer, int offset) {
        return wordMemory.segment_word(segmentBitsOf(objectPointer), locationBitsOf(objectPointer) + offset);
    }
    inline int heapChunkOf_word_put(int objectPointer, int offset, int value) {
        return wordMemory.segment_word_put(segmentBitsOf(objectPointer), locationBitsOf(objectPointer) + offset, value);
    }
    // Byte order within a word differs by image: the Tektronix (68K, big-endian)
    // image keeps byte 0 in the high half of each word, while the Xerox
    // (little-endian) image keeps byte 0 in the low half. RealWordMemory numbers
    // the high half as byte 0, so flip the selector for Xerox.
    inline int heapChunkByteNumber(int offset) {
        int byteNumber = offset % 2;
        if (imageType == ImageType::Xerox) byteNumber ^= 1;
        return byteNumber;
    }
    inline int heapChunkOf_byte(int objectPointer, int offset) {
        return wordMemory.segment_word_byte(segmentBitsOf(objectPointer), locationBitsOf(objectPointer) + offset/2, heapChunkByteNumber(offset));
    }
    inline int heapChunkOf_byte_put(int objectPointer, int offset, int value) {
        return wordMemory.segment_word_byte_put(segmentBitsOf(objectPointer), locationBitsOf(objectPointer) + offset/2, heapChunkByteNumber(offset), value);
    }

    int ot_bits_to(int objectPointer, int first, int last);
    int ot_bits_to_put(int objectPointer, int first, int last, int value);
    void ot_put(int objectPointer, int value);
    
    bool loadObjectTable(IFileSystem *fileSystem, int fd);
    bool loadObjects(IFileSystem *fileSystem, int fd);

    void headOfFreePointerListPut(int oop);
    void toFreePointerListAdd(int oop);
    void countUp(int oop);
    void countDown(int oop);
};

inline int ObjectMemory::fetchPointer_ofObject(int fieldIndex, int objectPointer) {
    if (!(fieldIndex >= 0 && fieldIndex < fetchWordLengthOf(objectPointer))) {
        char buf[256];
        snprintf(buf, sizeof(buf), "fetchPointer out of bounds: oop=%d len=%d index=%d", objectPointer, fetchWordLengthOf(objectPointer), fieldIndex);
        hal->error(buf);
    }
    int val = heapChunkOf_word(objectPointer, HeaderSize + fieldIndex);
    if (imageType == ImageType::Tektronix) return val ^ 1;
    return val;
}

inline int ObjectMemory::fetchWord_ofObject(int wordIndex, int objectPointer) {
    if (!(wordIndex >= 0 && wordIndex < fetchWordLengthOf(objectPointer))) { 
        char buf[256];
        snprintf(buf, sizeof(buf), "fetchWord out of bounds: oop=%d len=%d index=%d", objectPointer, fetchWordLengthOf(objectPointer), wordIndex);
        hal->error(buf);
    }
    if (isDisplayBitmap(objectPointer)) {
        auto& vec = virtualBitmaps[objectPointer];
        if (vec.empty()) {
            vec.resize(65536, 0);
            int sizeInHeap = sizeBitsOf(objectPointer) - HeaderSize;
            for (int i=0; i<sizeInHeap; i++) {
                vec[i] = heapChunkOf_word(objectPointer, HeaderSize + i);
            }
        }
        uint16_t val = vec[wordIndex];
#ifdef VM_DEBUG
        if (wordIndex >= 30000 && val != 0) {
             static int read_count = 0;
             if (read_count++ % 100 == 0) {
                 std::cerr << "fetchWord DisplayBitmap: index=" << wordIndex << " val=0x" << std::hex << val << std::dec << " count=" << read_count << std::endl;
             }
        }
#endif
        return val;
    }
    return heapChunkOf_word(objectPointer, HeaderSize + wordIndex);
}

inline int ObjectMemory::fetchByte_ofObject(int byteIndex, int objectPointer) {
    if (isDisplayBitmap(objectPointer)) {
        // Fallback to fetchWord if accessed as byte
        int wordIndex = byteIndex / 2;
        int byteOffset = byteIndex % 2;
        uint16_t word = fetchWord_ofObject(wordIndex, objectPointer);
        if (byteOffset == 0) return word >> 8;
        return word & 0xFF;
    }
    return heapChunkOf_byte(objectPointer, (HeaderSize*2 + byteIndex));
}

inline int ObjectMemory::storeByte_ofObject_withValue(int byteIndex, int objectPointer, int valueByte) {
    if (isDisplayBitmap(objectPointer)) {
        // Fallback to storeWord
        int wordIndex = byteIndex / 2;
        int byteOffset = byteIndex % 2;
        uint16_t word = fetchWord_ofObject(wordIndex, objectPointer);
        if (byteOffset == 0) {
            word = (valueByte << 8) | (word & 0xFF);
        } else {
            word = (word & 0xFF00) | valueByte;
        }
        // We need storeWord_ofObject_withValue to handle virtual bitmap too.
        // It is defined in objmemory.cpp, we will modify it there.
        storeWord_ofObject_withValue(wordIndex, objectPointer, word);
        return valueByte;
    }
    return heapChunkOf_byte_put(objectPointer, HeaderSize*2 + byteIndex, valueByte);
}

inline bool ObjectMemory::isDisplayBitmap(int objectPointer) {
    if (isIntegerObject(objectPointer)) return false;
    return imageType == ImageType::Tektronix && classBitsOf(objectPointer) == 30;
}

inline int ObjectMemory::fetchWordLengthOf(int objectPointer) { 
    if (isDisplayBitmap(objectPointer)) {
        return 65536; // 1024x1024 / 16
    }
    int sz = sizeBitsOf(objectPointer);
#ifdef VM_DEBUG
    if (objectPointer == 56) fprintf(stderr, "fetchWordLengthOf(56)=%d\n", sz);
#endif
    return sz - HeaderSize;
}


inline int ObjectMemory::fetchByteLengthOf(int objectPointer) { 
    return fetchWordLengthOf(objectPointer)*2 - oddBitOf(objectPointer); 
}

