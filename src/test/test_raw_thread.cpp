
#include <thread>
#include <iostream>
#include <chrono>
#include <unistd.h>
#include <vector>
#include <sys/syscall.h>
#define gettid() syscall(SYS_gettid)

#include "util/sys/raw_thread.hpp"

using namespace std::chrono;

void program(int* memory, int numInts, volatile bool* terminate) {
    int result = 1;
    size_t i = 0;
    while (!terminate) {
        result++;
        memory[i % numInts] = result;
        i++;
    }
}

template <typename T>
void test() {
 
    std::cout << gettid() << std::endl;
    
    size_t numInts = 1000;// * 1000 * 1000;
    int* memory = (int*)malloc(numInts * sizeof(int));
    std::cout << "allocated memory" << std::endl;
    
    usleep(1000 * 1000 * 2);
    
    volatile bool terminate = false;
    
    std::vector<T> threads;
    for (size_t j = 0; j < 1000; j++) {
        threads.emplace_back([&]() {program(memory, numInts, &terminate);});
    }

    std::vector<T> movedThreads = std::move(threads);    
    
    std::cout << "join threads" << std::endl;
    terminate = true;

    size_t joined = 0;
    for (auto& thread : movedThreads) {
        thread.join();
        joined++;
        std::cout << "joined #" << joined << std::endl;
    }
}

int main() {
    //test<std::thread>();
    test<RawThread>();
}
