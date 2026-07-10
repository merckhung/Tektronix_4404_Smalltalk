
#include <cstdint>
#include <algorithm>
#include <iostream>
#include <vector>
#include "objmemory.h"
#include "oops.h"

#ifndef GC_REF_COUNT
#ifndef GC_MARK_SWEEP
#error "must define GC_REF_COUNT and/or GC_MARK_SWEEP"
#endif
#endif


ObjectMemory::ObjectMemory(IHardwareAbstractionLayer *halInterface) : hal(halInterface), gcNotification(nullptr)
{
}

ObjectMemory::ObjectMemory(IHardwareAbstractionLayer *halInterface, IGCNotification *notification) : hal(halInterface), gcNotification(notification)
{
}

bool ObjectMemory::loadSnapshot(IFileSystem *fileSystem, const char *fileName)
{
    int fd = fileSystem->open_file(fileName);
    if (fd == -1)
        return false;
     
    bool succeeded = loadObjectTable(fileSystem, fd) && loadObjects(fileSystem, fd);
    
    fileSystem->close_file(fd);
    
    return succeeded;
}

bool ObjectMemory::loadObjectTable(IFileSystem *fileSystem, int fd)
{
    long imageSize = fileSystem->file_size(fd);
    ot_offset = 0;
    
    if (imageType == ImageType::Tektronix) {
        fileSystem->seek_to(fd, 0);
        std::uint16_t header[4];
        fileSystem->read(fd, (char *)&header, 8);

        std::uint32_t dataWords = (__builtin_bswap16(header[0]) << 16) | __builtin_bswap16(header[1]);
        
        ot_offset = 512 + dataWords * 2;
        
        long imageSize = fileSystem->file_size(fd);
        objectTableLength = (imageSize - ot_offset) / 4;
        
        fileSystem->seek_to(fd, ot_offset);
#ifdef VM_DEBUG
        std::cerr << "Tektronix OT: entries=" << objectTableLength << " offset=" << ot_offset << std::endl;
#endif
    } else {
        // Xerox interchange format. The 512-byte header is little-endian:
        //   bytes 0-3: object space length in words
        //   bytes 4-7: object table length in words
        // The object space starts at offset 512; the object table lives at the
        // TAIL of the file (offset = fileSize - otWords*2), 2 words per entry.
        fileSystem->seek_to(fd, 0);
        std::uint8_t header[8];
        fileSystem->read(fd, (char *)header, 8);
        std::uint32_t otWords = (std::uint32_t)header[4] |
                                ((std::uint32_t)header[5] << 8) |
                                ((std::uint32_t)header[6] << 16) |
                                ((std::uint32_t)header[7] << 24);
        ot_offset = imageSize - (long)otWords * 2;
        objectTableLength = otWords / 2;
        fileSystem->seek_to(fd, ot_offset);
#ifdef VM_DEBUG
        std::cerr << "Xerox OT: entries=" << objectTableLength << " offset=" << ot_offset << std::endl;
#endif
    }

    for(int i = 0; i < (int)objectTableLength; i++)
    {
        int objectPointer = i << 1;
        if (objectPointer >= ObjectTableSize) break;
        
        if (imageType == ImageType::Tektronix) {
            std::uint8_t raw[4];
            if (fileSystem->read(fd, (char *)raw, 4) != 4) return false;
            std::uint8_t entry[4];
#ifdef VM_DEBUG
            if (objectPointer <= 64) {
                printf("RAW OT entry for %d: %02X %02X %02X %02X\n", objectPointer, raw[0], raw[1], raw[2], raw[3]);
            }
#endif
            entry[0] = raw[0];
            entry[1] = raw[1];
            entry[2] = raw[2];
            entry[3] = raw[3];
            
            std::uint32_t addr = ((std::uint32_t)entry[1] << 16) | ((std::uint32_t)entry[2] << 8) | entry[3];
            bool free = (addr >= ot_offset) || ((entry[0] & 0x20) != 0);
            
            // Do not subtract 2 here. We will dynamically find the header in loadObjects.
            objectSegments[objectPointer] = FirstHeapSegment + (std::uint16_t)(addr >> 16);
            objectLocations[objectPointer] = (std::uint16_t)(addr & 0xFFFF);
            
            bool pointer = (entry[0] & 0x80) != 0;
            bool odd = (entry[0] & 0x40) != 0;
            int otWord = ((entry[0] & 0x1F) << 8) | 
                         (odd ? (1 << 7) : 0) | 
                         (pointer ? (1 << 6) : 0) | 
                         (free ? (1 << 5) : 0);
            ot_put(objectPointer, otWord);
        } else {
            // Xerox OT entries are little-endian: [val_lo val_hi loc_lo loc_hi].
            // val holds the count/pointer/free flags plus the file segment in
            // its low nibble (G&R MSB bits 12-15); loc is the file word offset.
            std::uint8_t raw[4];
            if (fileSystem->read(fd, (char *)raw, 4) != 4) return false;
            std::uint16_t val = (std::uint16_t)(raw[0] | (raw[1] << 8));
            std::uint16_t loc = (std::uint16_t)(raw[2] | (raw[3] << 8));

            ot_put(objectPointer, val);
            objectSegments[objectPointer] = (std::uint16_t)(val & 0x0F);
            objectLocations[objectPointer] = loc;
        }
    }
    if (imageType == ImageType::Xerox) {
        // Mark every object-table slot past the loaded set as free so the
        // allocator and GC never treat an uninitialised entry as a live object.
        for (int oop = (int)(objectTableLength << 1); oop < ObjectTableSize; oop += 2) {
            ot_put(oop, 1 << 5);  // free bit (G&R MSB bit 10)
            objectSegments[oop] = 0;
            objectLocations[oop] = 0;
        }
    }
    if (imageType == ImageType::Tektronix) {
        struct EntryInfo {
            std::uint32_t addr;
            int oop;
        };
        std::vector<EntryInfo> candidates;
        for (int i = 0; i < (int)objectTableLength; i++) {
            int oop = i << 1;
            if (oop >= ObjectTableSize) break;
            if (!freeBitOf(oop)) {
                std::uint32_t addr = ((std::uint32_t)(objectSegments[oop] - FirstHeapSegment) << 16) | objectLocations[oop];
                candidates.push_back({addr, oop});
            }
        }
        std::sort(candidates.begin(), candidates.end(), [](const EntryInfo &a, const EntryInfo &b) {
            if (a.addr == b.addr) return a.oop < b.oop;
            return a.addr < b.addr;
        });
        
        std::uint32_t expected_next_addr = 0;
        int filteredCount = 0;
        for (const auto &info : candidates) {
            if (info.addr < expected_next_addr) {
                if (info.oop != NilPointer && info.oop != FalsePointer && info.oop != TruePointer && info.oop != SchedulerAssociationPointer) {
                    int otWord = ot(info.oop);
                    otWord |= (1 << 5);
                    ot_put(info.oop, otWord);
                    filteredCount++;
                    continue;
                } else {
#ifdef VM_DEBUG
                    std::cerr << "WARNING: Protected special Oop " << info.oop << " from being filtered!" << std::endl;
#endif
                }
            }
            
            fileSystem->seek_to(fd, 512 + info.addr);
            std::uint16_t header[2];
            if (fileSystem->read(fd, (char *)&header, 4) == 4) {
                std::uint16_t size = __builtin_bswap16(header[0]);
                size = size & 0x7FFF;
                expected_next_addr = info.addr + size * 2;
            } else {
                int otWord = ot(info.oop);
                otWord |= (1 << 5);
                ot_put(info.oop, otWord);
                filteredCount++;
            }
        }
#ifdef VM_DEBUG
        std::cerr << "Filtered " << filteredCount << " overlapping/invalid OT entries." << std::endl;
#endif
        (void)filteredCount;
    }
     
    return true;
}

bool ObjectMemory::loadObjects(IFileSystem *fileSystem, int fd)
{
    // Xerox object space is little-endian (matching an x86 host), so no
    // per-word byte swap is needed; Tektronix is handled explicitly below.
    bool swap = false;
    int loadedCount = 0;
    
#ifdef VM_DEBUG
    std::cout << "Loading objects... " << std::endl;
#endif

    std::vector<std::uint32_t> sortedAddrs;
    if (imageType == ImageType::Tektronix) {
        for (int i = 0; i < (int)objectTableLength; i++) {
            int objectPointer = i << 1;
            if (objectPointer >= ObjectTableSize) break;
            if (!freeBitOf(objectPointer)) {
                std::uint32_t addr = ((std::uint32_t)(objectSegments[objectPointer] - FirstHeapSegment) << 16) | (std::uint16_t)objectLocations[objectPointer];
                sortedAddrs.push_back(addr);
            }
        }
        std::sort(sortedAddrs.begin(), sortedAddrs.end());
    }

    int destinationSegment = FirstHeapSegment, destinationWord = 0;

    for(int i = 0; i < (int)objectTableLength; i++)
    {
        int objectPointer = i << 1;
        if (objectPointer >= ObjectTableSize) break;
        if (freeBitOf(objectPointer)) continue;

        std::uint16_t w[2];
        int dataWordSize = 0;
        int classOop = 0;

        if (imageType == ImageType::Tektronix) {
            // Addr in OT is a BYTE offset from the start of the data segment (which is at 512)
            // It points directly to the 2-word header (Size, Class).
            std::uint32_t byte_addr = ((std::uint32_t)(objectSegments[objectPointer] - FirstHeapSegment) << 16) | objectLocations[objectPointer];
            std::uint32_t seekAddr = 512 + byte_addr;
            
            if (seekAddr >= fileSystem->file_size(fd)) {
#ifdef VM_DEBUG
                std::cerr << "Skip loading Oop " << objectPointer << " seekAddr=" << seekAddr << " file_size=" << fileSystem->file_size(fd) << " seg=" << objectSegments[objectPointer] << " loc=" << objectLocations[objectPointer] << " FirstHeapSegment=" << FirstHeapSegment << std::endl;
#endif
                continue;
            }
            
            std::uint16_t headerWords[2];
            fileSystem->seek_to(fd, seekAddr);
            if (fileSystem->read(fd, (char*)&headerWords, 4) != 4) continue;
            
            w[0] = __builtin_bswap16(headerWords[0]);
            w[1] = __builtin_bswap16(headerWords[1]);
            
            dataWordSize = (int)(w[0] & 0x7FFF) - 2;
            if (dataWordSize < 0) dataWordSize = 0;
            
            classOop = w[1]; // Store raw ODD class tag. classBitsOf() will XOR it with 1.
            w[0] = w[0] & 0x7FFF; // Clear the high bit so sizeBitsOf() returns correct size.
            
            // Advance seekAddr to point to payload
            fileSystem->seek_to(fd, seekAddr + 4);
        } else {
            // Xerox: object space starts at offset 512; each OT entry's
            // (segment,location) gives the word offset of the 2-word header
            // (size, class). The data is little-endian, so read it directly.
            std::uint32_t addr = ((std::uint32_t)(objectSegments[objectPointer]) << 16) | (std::uint16_t)objectLocations[objectPointer];
            std::uint32_t seekAddr = 512 + addr * 2;
            if (fileSystem->seek_to(fd, seekAddr) == -1) continue;
            if (fileSystem->read(fd, (char *) &w, 4) != 4) continue;
            // The Xerox size word is the whole 16-bit size (no flag in bit 15),
            // so do not mask it -- large objects (bitmaps, big arrays) exceed
            // 0x7FFF and masking would under-count their words.
            dataWordSize = (int)w[0] - 2;
            if (dataWordSize < 0) dataWordSize = 0;
            classOop = w[1];
        }

        if (dataWordSize < 0) dataWordSize = 0;

        int space = dataWordSize + 2;
        if (imageType == ImageType::Xerox) {
            // Huge pointer objects (>= 256 words) reserve one extra header word
            // in the packed heap, matching the Blue Book memory layout.
            int objectSize = dataWordSize + 2;
            if (objectSize >= 256 && pointerBitOf(objectPointer) != 0) space += 1;
        }
        if (destinationWord + space > 65536)
        {
            destinationSegment++;
            if (destinationSegment > LastHeapSegment) { std::cerr << "Out of memory loading Oop " << objectPointer << " seg=" << destinationSegment << std::endl; return false; }
            destinationWord = 0;
        }

        objectSegments[objectPointer] = destinationSegment;
        objectLocations[objectPointer] = destinationWord;

        heapChunkOf_word_put(objectPointer, 0, w[0]);
        heapChunkOf_word_put(objectPointer, 1, classOop);
        
#ifdef VM_DEBUG
        if (objectPointer == 36196) {
            std::cout << "Loading Oop 36196: size=" << w[0] << " class=" << classOop << " at segment=" << destinationSegment << " word=" << destinationWord << std::endl;
        }
#endif
        
        if (dataWordSize > 0) {
            std::vector<std::uint16_t> buffer(dataWordSize);
            if (fileSystem->read(fd, (char *)buffer.data(), dataWordSize * 2) == dataWordSize * 2) {
                for (int j=0; j<dataWordSize; j++) {
                    int val;
                    if (imageType == ImageType::Tektronix) {
                        val = __builtin_bswap16(buffer[j]);
                    } else if (swap) {
                        val = __builtin_bswap16(buffer[j]);
                    } else {
                        val = buffer[j];
                    }
                    heapChunkOf_word_put(objectPointer, 2 + j, val);
                }
            }
        }
        
        destinationWord += space;
        loadedCount++;
    }
#ifdef VM_DEBUG
    std::cerr << "Loaded " << loadedCount << " objects." << std::endl;
#endif
    (void)loadedCount;
    lastAllocatedSegment = destinationSegment;
    lastAllocatedWord = destinationWord;
    return true;
}

int ObjectMemory::ot(int objectPointer) {
    return wordMemory.segment_word(ObjectTableSegment, ObjectTableStart + objectPointer);
}

bool ObjectMemory::hasObject(int objectPointer) { return !freeBitOf(objectPointer); }
void ObjectMemory::increaseReferencesTo(int objectPointer) {}
void ObjectMemory::decreaseReferencesTo(int objectPointer) {}
void ObjectMemory::addRoot(int rootObjectPointer) {
    if (!isIntegerObject(rootObjectPointer)) {
        // Prevent duplicate roots
        for (int r : gcRoots) {
            if (r == rootObjectPointer) return;
        }
        gcRoots.push_back(rootObjectPointer);
    }
}

int ObjectMemory::ot_bits_to(int objectPointer, int first, int last) {
    int val = ot(objectPointer);
    int mask = (1 << (last - first + 1)) - 1;
    return (val >> (15 - last)) & mask;
}
int ObjectMemory::ot_bits_to_put(int objectPointer, int first, int last, int value) {
    int val = ot(objectPointer);
    int mask = (1 << (last - first + 1)) - 1;
    val = (val & ~(mask << (15 - last))) | ((value & mask) << (15 - last));
    ot_put(objectPointer, val);
    return value;
}
void ObjectMemory::ot_put(int objectPointer, int value) {
    wordMemory.segment_word_put(ObjectTableSegment, ObjectTableStart + objectPointer, value);
}

int ObjectMemory::allocateChunk(int classPointer, int length, int allocationType) {
    // allocationType: 0=Words, 1=Bytes, 2=Pointers
    // Pointer fields are stored in 16-bit heap words, so an oop must fit in
    // 16 bits: only OT indices below 32768 (oop <= 65534) are usable.
    int usableIndexLimit = std::min((int)objectTableLength, 32768);
    int newOop = -1;
    for (int i = 1; i < usableIndexLimit; i++) {
        int oop = i << 1;
        if (freeBitOf(oop)) {
            newOop = oop;
            break;
        }
    }
    if (newOop == -1) {
        std::cerr << "OUT OF OOPS! Triggering GC..." << std::endl;
        garbageCollect();
        for (int i = 1; i < usableIndexLimit; i++) {
            int oop = i << 1;
            if (freeBitOf(oop)) {
                newOop = oop;
                break;
            }
        }
        if (newOop == -1) {
            std::cerr << "OUT OF OOPS AFTER GC!" << std::endl;
            return NilPointer;
        }
    }


    int dataWordSize = length;
    bool isOdd = false;
    if (allocationType == 1) { // Bytes
        dataWordSize = (length + 1) / 2;
        isOdd = (length % 2) != 0;
    }

    int space = dataWordSize + 2;
    if (lastAllocatedWord + space > 65536) {
        lastAllocatedSegment++;
        if (lastAllocatedSegment > LastHeapSegment) {
            std::cerr << "OUT OF HEAP MEMORY" << std::endl;
            return NilPointer;
        }
        lastAllocatedWord = 0;
    }

    objectSegments[newOop] = lastAllocatedSegment;
    objectLocations[newOop] = lastAllocatedWord;
    
    int otWord = ot(newOop);
    otWord &= ~(1 << 5); // clear free bit (bit 10)
    
    if (allocationType == 2) { // Pointers
        otWord |= (1 << 6); // set pointer bit (bit 9)
    } else {
        otWord &= ~(1 << 6);
    }
    
    if (isOdd) {
        otWord |= (1 << 7); // set odd bit (bit 8)
    } else {
        otWord &= ~(1 << 7);
    }
    
    ot_put(newOop, otWord);

    heapChunkOf_word_put(newOop, 0, dataWordSize + 2);
    
    int rawClass = classPointer;
    if (imageType == ImageType::Tektronix) rawClass ^= 1;
    heapChunkOf_word_put(newOop, 1, rawClass);

    int initialValue = (allocationType == 2) ? NilPointer : 0;
    if (imageType == ImageType::Tektronix && allocationType == 2) {
        initialValue ^= 1;
    }
    for (int i = 0; i < dataWordSize; i++) {
        heapChunkOf_word_put(newOop, 2 + i, initialValue);
    }

    lastAllocatedWord += space;
#ifdef VM_DEBUG
    std::cerr << "Allocated " << newOop << " class " << classPointer << std::endl;
#endif
    return newOop;
}

int ObjectMemory::instantiateClass_withWords(int classPointer, int length) { return allocateChunk(classPointer, length, 0); }
int ObjectMemory::instantiateClass_withBytes(int classPointer, int length) { return allocateChunk(classPointer, length, 1); }
int ObjectMemory::instantiateClass_withPointers(int classPointer, int length) { return allocateChunk(classPointer, length, 2); }
void ObjectMemory::headOfFreePointerListPut(int oop) {}
void ObjectMemory::toFreePointerListAdd(int oop) {}
void ObjectMemory::countUp(int oop) {}
void ObjectMemory::countDown(int oop) {}

void ObjectMemory::garbageCollect() {
    std::cerr << "GC starting... (OOPs used before: " << (objectTableLength - oopsLeft()) << ")" << std::endl;
    
    // 1. Reset roots and marked list
    gcRoots.clear();
    gcMarked.assign(ObjectTableSize, false);

    // 2. Add special protected OOPs as roots
    addRoot(NilPointer);
    addRoot(FalsePointer);
    addRoot(TruePointer);

    // 3. Populate roots from Interpreter
    if (gcNotification) {
        gcNotification->prepareForCollection();
    }

    // 4. Mark phase (using a stack to avoid recursion)
    std::vector<int> todo;
    for (int root : gcRoots) {
        if (root >= 0 && root < ObjectTableSize) {
            if (!gcMarked[root] && !freeBitOf(root)) {
                gcMarked[root] = true;
                todo.push_back(root);
            }
        }
    }

    int marked_count = 0;
    while (!todo.empty()) {
        int oop = todo.back();
        todo.pop_back();
        marked_count++;

        // Mark the class of the object!
        int cls = fetchClassOf(oop);
        if (!isIntegerObject(cls) && cls != NilPointer) {
            if (cls >= 0 && cls < ObjectTableSize) {
                if (!gcMarked[cls] && !freeBitOf(cls)) {
                    gcMarked[cls] = true;
                    todo.push_back(cls);
                }
            }
        }

        // If it is a pointer object, mark its fields
        if (pointerBitOf(oop) == 1) {
            int len = fetchWordLengthOf(oop);
            for (int i = 0; i < len; i++) {
                int field = fetchPointer_ofObject(i, oop);
                if (!isIntegerObject(field) && field != NilPointer) {
                    if (field >= 0 && field < ObjectTableSize) {
                        if (!gcMarked[field] && !freeBitOf(field)) {
                            gcMarked[field] = true;
                            todo.push_back(field);
                        }
                    }
                }
            }
        }
        else if (cls == ClassCompiledMethod) {
            int header = fetchPointer_ofObject(0, oop);
            int litCount = (header >> 1) & 63;
            for (int i = 0; i < litCount; i++) {
                int lit = fetchPointer_ofObject(1 + i, oop);
                if (!isIntegerObject(lit) && lit != NilPointer) {
                    if (lit >= 0 && lit < ObjectTableSize) {
                        if (!gcMarked[lit] && !freeBitOf(lit)) {
                            gcMarked[lit] = true;
                            todo.push_back(lit);
                        }
                    }
                }
            }
        }
    }


    // 5. Sweep phase: free unmarked OOPs
    int reclaimed_count = 0;
    for (int i = 1; i < (int)objectTableLength; i++) {
        int oop = i << 1;
        if (!freeBitOf(oop)) {
            if (!gcMarked[oop]) {
                // Free this OOP!
                ot_bits_to_put(oop, 10, 10, 1); // Set free bit (bit 10) to 1
                reclaimed_count++;
                
                // Remove virtual bitmap if it had one
                virtualBitmaps.erase(oop);
            }
        }
    }

    // 6. Notify Interpreter that GC is done
    if (gcNotification) {
        gcNotification->collectionCompleted();
    }

    std::cerr << "GC finished. Marked: " << marked_count 
              << ", Reclaimed OOPs: " << reclaimed_count 
              << ", OOPs in use now: " << (objectTableLength - oopsLeft()) << std::endl;
}

int ObjectMemory::oopsLeft() {
    int free_count = 0;
    // Only oops that fit in a 16-bit pointer field are usable.
    int usableIndexLimit = std::min((int)objectTableLength, 32768);
    for (int i = 1; i < usableIndexLimit; i++) {
        int oop = i << 1;
        if (freeBitOf(oop)) {
            free_count++;
        }
    }
    return free_count;
}

int ObjectMemory::coreLeft() {
    long total = (long)(LastHeapSegment - FirstHeapSegment + 1) * 65536;
    long used = (long)(lastAllocatedSegment - FirstHeapSegment) * 65536 + lastAllocatedWord;
    return (int)(total - used);
}

int ObjectMemory::initialInstanceOf(int classPointer) {
    for(int i = 2; i < ObjectTableSize; i += 2) {
        if (hasObject(i) && fetchClassOf(i) == classPointer) return i;
    }
    return NilPointer;
}
int ObjectMemory::instanceAfter(int objectPointer) {
    int classPointer = fetchClassOf(objectPointer);
    for(int i = objectPointer + 2; i < ObjectTableSize; i += 2) {
        if (hasObject(i) && fetchClassOf(i) == classPointer) return i;
    }
    return NilPointer;
}
void ObjectMemory::swapPointersOf_and(int p1, int p2) {
    int s1 = objectSegments[p1];
    int l1 = objectLocations[p1];
    int o1 = ot(p1);
    
    objectSegments[p1] = objectSegments[p2];
    objectLocations[p1] = objectLocations[p2];
    ot_put(p1, ot(p2));
    
    objectSegments[p2] = s1;
    objectLocations[p2] = l1;
    ot_put(p2, o1);
}

int ObjectMemory::storePointer_ofObject_withValue(int fieldIndex, int objectPointer, int valuePointer) {
#ifdef VM_DEBUG
    if (imageType == ImageType::Tektronix) {
        if (objectPointer == 65568 && fieldIndex == 0) {
            std::cerr << "DEBUG storePointer: oop=65568 field=0 val=" << valuePointer << " (will write " << (valuePointer ^ 1) << ")" << std::endl;
        }
    }
#endif
    if (imageType == ImageType::Tektronix) valuePointer ^= 1;
    return heapChunkOf_word_put(objectPointer, HeaderSize + fieldIndex, valuePointer);
}

int ObjectMemory::storeWord_ofObject_withValue(int wordIndex, int objectPointer, int valueWord) {
    if (isDisplayBitmap(objectPointer)) {
        auto& vec = virtualBitmaps[objectPointer];
        if (vec.empty()) {
            vec.resize(65536, 0);
            int sizeInHeap = sizeBitsOf(objectPointer) - HeaderSize;
            for (int i=0; i<sizeInHeap; i++) {
                vec[i] = heapChunkOf_word(objectPointer, HeaderSize + i);
            }
        }
        vec[wordIndex] = valueWord;
#ifdef VM_DEBUG
        if (wordIndex >= 30000) {
             std::cerr << "storeWord DisplayBitmap: index=" << wordIndex << " val=0x" << std::hex << valueWord << std::dec << " oop=" << objectPointer << std::endl;
        }
#endif
        return valueWord;
    }

#ifdef VM_DEBUG
    if (imageType == ImageType::Tektronix) {
        if (objectPointer == 65568 && wordIndex == 0) {
            std::cerr << "DEBUG storeWord: oop=65568 word=0 val=" << valueWord << std::endl;
        }
    }
#endif
    return heapChunkOf_word_put(objectPointer, HeaderSize + wordIndex, valueWord);
}


bool ObjectMemory::saveSnapshot(IFileSystem *fileSystem, const char *fileName) {
    return false;
}
