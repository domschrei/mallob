
#pragma once

#include <fstream>
#include <vector>
#include <list>
#include <cstring>

#include "util/assert.hpp"
#include "util/logger.hpp"

template <typename T>
class CategorizedExternalMemory {

private:
    std::fstream _disk;
    int _blocksize;
    size_t _num_blocks_in_disk = 0;

    struct Block {
        int address;
        int size = 0;
        std::vector<uint8_t> buffer;
        Block(int address, int capacity) : address(address) {
            buffer.reserve(capacity);
        }
        
        void append(const T& obj) {
            buffer.resize(size + sizeof(T));
            memcpy(buffer.data()+size, &obj, sizeof(T));
            size += sizeof(T);
        }
    };
    std::vector<std::list<Block>> _blocks_per_address;
    std::list<int> _free_addresses;

    std::vector<uint8_t> _empty_buffer;

    unsigned long long _num_elems = 0;

public:
    CategorizedExternalMemory(const std::string& diskFile, int blockSize) : _blocksize(blockSize) {
        _disk = std::fstream(diskFile, std::fstream::in | std::fstream::out | std::fstream::binary | std::fstream::trunc);
        assert(_disk.good());
        _empty_buffer = std::vector<uint8_t>(_blocksize, 0);
    }

    void add(size_t address, const T& object) {

        // Create address as necessary
        if (address >= _blocks_per_address.size()) {
            _blocks_per_address.resize(address+1);
        }
        // Access block list at address
        auto& blocks = _blocks_per_address.at(address);

        // Add a first block at address as necessary
        if (blocks.empty()) {
            blocks.push_back(fetchFreshBlock());
        }
        // If the last block is now full, add a fresh block
        {
            auto& block = blocks.back();
            if (_blocksize - block.size < sizeof(T)) {
                // Block is full! Write to disk and fetch a new one.
                sync(block);
                blocks.push_back(fetchFreshBlock());
            }
        }
        // Append object to the current, non-full block
        auto& block = blocks.back();
        block.append(object);
        _num_elems++;
    }

    void fetchAndRemove(size_t address, std::vector<T>& result) {
        assert(result.empty());
        if (address >= _blocks_per_address.size())
            return; 
        // Access block list at address
        auto& blocks = _blocks_per_address.at(address);
        while (!blocks.empty()) {
            auto block = std::move(blocks.front());
            blocks.pop_front();
            result.resize(result.size() + (block.size / sizeof(T)));
            auto insertionPoint = result.data() + result.size() - (block.size / sizeof(T));
            //LOG(V2_INFO, "block #%i: size %i\n", block.address, block.size);
            if (!block.buffer.empty()) {
                // Read directly from internal buffer
                //LOG(V2_INFO, "- fetch from int. buffer\n");
                memcpy(insertionPoint, block.buffer.data(), block.size);
            } else {
                // Read from disk
                //LOG(V2_INFO, "- fetch from disk\n");
                _disk.seekg(_blocksize*block.address, std::ios::beg);
                _disk.read((char*) insertionPoint, block.size);
            }
            _free_addresses.push_back(block.address);
        }
        _num_elems -= result.size();
    }

    unsigned long long size() const {
        return _num_elems;
    }

private:

    void sync(Block& block) {
        assert(block.buffer.size() == block.size);
        _disk.seekp(_blocksize*block.address, std::ios::beg);
        _disk.write((const char*) block.buffer.data(), block.buffer.size());
        _disk.write((const char*) _empty_buffer.data(), _blocksize - block.buffer.size());
        block.buffer = std::vector<uint8_t>();
    }

    Block fetchFreshBlock() {
        if (_free_addresses.empty()) {
            appendBlockToDisk();
        }
        auto address = _free_addresses.back();
        _free_addresses.pop_back();
        return Block(address, _blocksize);
    }

    void appendBlockToDisk() {
        _disk.seekp(_blocksize*_num_blocks_in_disk, std::ios::beg);
        _disk.write((const char*) _empty_buffer.data(), _empty_buffer.size());
        _free_addresses.push_back(_num_blocks_in_disk);
        _num_blocks_in_disk++;
    }
};
