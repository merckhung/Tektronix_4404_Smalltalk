
#pragma once

#include <cstdint>
#include <cassert>
#include <cstdio>

// Segmented Memory Model as described in G&R pg. 656
class RealWordMemory
{
public:
    static const int SegmentCount = 8192;
    static const int SegmentSize = 65536; /* in words */
    
    RealWordMemory()
    {
    }
    
    inline int segment_word(int s, int w)
    {
        s += w / SegmentSize;
        w %= SegmentSize;
        if (s < 0 || s >= SegmentCount || w < 0 || w >= SegmentSize) {
            fprintf(stderr, "RealWordMemory::segment_word error: s=%d w=%d\n", s, w);
            fflush(stderr);
        }
        assert(s >= 0 && s < SegmentCount);
        assert(w >= 0 && w < SegmentSize);
        return memory[s][w];
    }
    
    inline int segment_word_put(int s, int w, int value)
    {
        s += w / SegmentSize;
        w %= SegmentSize;
        if (s < 0 || s >= SegmentCount || w < 0 || w >= SegmentSize) {
            fprintf(stderr, "RealWordMemory::segment_word_put error: s=%d w=%d\n", s, w);
            fflush(stderr);
        }
        assert(s >= 0 && s < SegmentCount);
        assert(w >= 0 && w < SegmentSize);
        return memory[s][w] = (std::uint16_t)value;
    }
    
    inline int segment_word_byte(int s, int w, int byteNumber)
    {
        s += w / SegmentSize;
        w %= SegmentSize;
        if (s < 0 || s >= SegmentCount || w < 0 || w >= SegmentSize || (byteNumber != 0 && byteNumber != 1)) {
            fprintf(stderr, "RealWordMemory::segment_word_byte error: s=%d w=%d byteNumber=%d\n", s, w, byteNumber);
            fflush(stderr);
        }
        assert(s >= 0 && s < SegmentCount);
        assert(w >= 0 && w < SegmentSize);
        assert(byteNumber == 0 || byteNumber == 1);
        
        if (byteNumber == 0) return memory[s][w] >> 8;
        return memory[s][w] & 0xFF;
    }
    
    inline int segment_word_byte_put(int s, int w, int byteNumber, int value)
    {
        s += w / SegmentSize;
        w %= SegmentSize;
        if (s < 0 || s >= SegmentCount || w < 0 || w >= SegmentSize || (byteNumber != 0 && byteNumber != 1)) {
            fprintf(stderr, "RealWordMemory::segment_word_byte_put error: s=%d w=%d byteNumber=%d\n", s, w, byteNumber);
            fflush(stderr);
        }
        assert(s >= 0 && s < SegmentCount);
        assert(w >= 0 && w < SegmentSize);
        assert(byteNumber == 0 || byteNumber == 1);
        
        if (byteNumber == 0) {
            memory[s][w] = (memory[s][w] & 0x00FF) | ((value & 0xFF) << 8);
        } else {
            memory[s][w] = (memory[s][w] & 0xFF00) | (value & 0xFF);
        }
        return value;
    }
    
    inline int segment_word_bits_to(int s, int w, int firstBitIndex, int lastBitIndex)
    {
        s += w / SegmentSize;
        w %= SegmentSize;
        assert(s >= 0 && s < SegmentCount);
        assert(w >= 0 && w < SegmentSize);
        
        std::uint16_t shift = memory[s][w] >> (15-lastBitIndex);
        std::uint16_t mask = (1 << (lastBitIndex - firstBitIndex + 1)) - 1;
        int res = shift & mask;
        if (s == 255 && w == 268) { fprintf(stderr, "bits_to(255, 268) val=%d bits[%d:%d]=%d\n", memory[s][w], firstBitIndex, lastBitIndex, res); fflush(stderr); }
        return res;
    }
    
    inline int segment_word_bits_to_put(int s, int w, int firstBitIndex, int lastBitIndex, int value)
    {
        s += w / SegmentSize;
        w %= SegmentSize;
        assert(s >= 0 && s < SegmentCount);
        assert(w >= 0 && w < SegmentSize);
        
        std::uint16_t mask = (1 << (lastBitIndex - firstBitIndex + 1)) - 1;
        memory[s][w] = (memory[s][w] & ~(mask << (15-lastBitIndex))) | ((value & mask) << (15 - lastBitIndex));
        return value;
    }

private:
    std::uint16_t memory[SegmentCount][SegmentSize];
};
