
#include "util/categorized_external_memory.hpp"

#include "util/sys/timer.hpp"
#include "util/random.hpp"

void testBasic() {
    auto blocksize = 4096;
    CategorizedExternalMemory<long> ext("test.bin", blocksize);
    
    ext.add(0, 1L);
    ext.add(0, 2L);
    ext.add(0, 3L);
    ext.add(1, -1L);
    ext.add(3, -1L);
    assert(ext.size() == 5);
    for (int rep = 1; rep <= 4097; rep++) {
        ext.add(10, (long) rep);
    }
    assert(ext.size() == 5+4097);

    {
        std::vector<long> data;
        ext.fetchAndRemove(0, data);
        assert(data.size() == 3);
        assert(data[0] == 1L);
        assert(data[1] == 2L);
        assert(data[2] == 3L);
        assert(ext.size() == 5+4097-3);
        data.clear(); ext.fetchAndRemove(0, data); assert(data.empty());
    }

    {
        std::vector<long> data;
        ext.fetchAndRemove(1, data);
        assert(data.size() == 1);
        assert(data[0] == -1L);
        assert(ext.size() == 5+4097-3-1);
        data.clear(); ext.fetchAndRemove(0, data); assert(data.empty());
    }

    {
        std::vector<long> data;
        ext.fetchAndRemove(10, data);
        assert(data.size() == 4097);
        assert(ext.size() == 1);
        for (int rep = 0; rep < 4097; rep++) {
            auto read = data[rep];
            assert(read == rep+1
                || log_return_false("i=%i : %lu != %lu!\n", rep, read, rep+1));
        }
        data.clear(); ext.fetchAndRemove(0, data); assert(data.empty());
    }
}

void testPerformance() {
    
    {
        auto blocksize = 4096;
        CategorizedExternalMemory<int> ext("test.bin", blocksize);

        auto t = Timer::elapsedSeconds();

        for (int i = 0; i < 1024*1024; i++) {
            ext.add(0, i+1);
        }

        t = Timer::elapsedSeconds()-t;
        LOG(V2_INFO, "ExternalMemory: inserted within %.3fs\n", t);

        t = Timer::elapsedSeconds();
        std::vector<int> data;
        ext.fetchAndRemove(0, data);
        assert(ext.size() == 0);
        t = Timer::elapsedSeconds()-t;
        assert(data.size() == 1024*1024);
        for (int i = 0; i < 1024*1024; i++) {
            auto read = data[i];
            assert(read == i+1);
        }
        LOG(V2_INFO, "ExternalMemory: flushed within %.3fs\n", t);
    }

    {
        std::ofstream output("test.bin", std::ios::binary);
        auto t = Timer::elapsedSeconds();
        for (int i = 0; i < 1024*1024; i++) {
            int n = i+1;
            output.write((const char*) (&n), sizeof(int));
        }
        t = Timer::elapsedSeconds()-t;
        LOG(V2_INFO, "std::ofstream: inserted within %.3fs\n", t);
    }
}

void testBuckets() {
    Random::init(1, 1);

    auto blocksize = 4096;
    CategorizedExternalMemory<int> ext("test.bin", blocksize);

    for (size_t i = 0; i < 1024*1024; i++) {
        int elem = (int) (1024*1024*Random::rand()) + 1;
        ext.add(elem%1024, elem);
    }

    int totalNumElems = 0;
    for (int bucket = 0; bucket < 1024; bucket++) {
        std::vector<int> data;
        ext.fetchAndRemove(bucket, data);
        int numElems = data.size();
        LOG(V2_INFO, "Bucket %i : %i elements\n", bucket, numElems);
        //std::string out = "";
        //for (size_t i = 0; i < data.size()/sizeof(int); i++) {
        //    out += " " + std::to_string(dataInts[i]);
        //}
        //LOG(V2_INFO, "%s\n", out.c_str());
        for (int i : data) {
            assert(i % 1024 == bucket || log_return_false("%i %% 1024 != %i\n", i, bucket));
        }
        totalNumElems += numElems;
    }
    assert(totalNumElems == 1024*1024);
    assert(ext.size() == 0);
}

int main() {
    testBasic();
    testPerformance();
    testBuckets();
}
