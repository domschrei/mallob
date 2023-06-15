#include <algorithm>
#include <iostream>
#include "util/assert.hpp"
#include <mutex>
#include <vector>
#include <string>
#include <fstream>
#include <unistd.h>

#include "util/random.hpp"
#include "app/qbf/execution/bloqqer_caller.hpp"
#include "util/logger.hpp"
#include "util/sys/timer.hpp"

static void test_write() {
    Logger logger = Logger::getMainInstance().copy("<BloqqerCaller>", ".bloqqercaller");
    
    BloqqerCaller bq(logger);
    const char* filename = "test_bloqqer_caller.qdimacs";
    FILE* tgt = fopen(filename, "w");
    std::vector<int> formula = {1, -2, -3, 4, -5, 0, 2, 3, 0, 1, 5, 0};
    bq.writeQDIMACS(formula, tgt, 5);
    fclose(tgt);
    std::ifstream t(filename);
    std::string written((std::istreambuf_iterator<char>(t)),
                        std::istreambuf_iterator<char>());
    unlink(filename);
    std::string expected = R"(p cnf 5 2
e 1 0
a 2 3 0
e 4 0
a 5 0
2 3 0
1 5 0
)";
    if(written != expected) {
      std::cout << "Written: " << written << std::endl;
      std::cout << "Expected: " << expected << std::endl;
    }
    assert(written == expected);
}

static void test_read() {
    FILE* f = fopen("instances/qbf/microtest01.qdimacs", "r");
    assert(f);

    Logger logger = Logger::getMainInstance().copy("<BloqqerCaller>", ".bloqqercaller");
    BloqqerCaller bq(logger);
    // Dummy data, has to be cleared by readQDIMACS!
    std::vector<int> tgt = { 1, 2, 3};
    bq.readQDIMACS(f, tgt);

    std::vector<int> expected_tgt = {-1, 2, 0, 1, 2, 0};

    bool are_equal = std::equal(tgt.begin(), tgt.end(),
                                expected_tgt.begin(), expected_tgt.end());

    if(!are_equal) {
      std::cout << "Read: ";
      for(int i : tgt) std::cout << i << " ";
      std::cout << std::endl << "Expected: ";
      for(int i : expected_tgt) std::cout << i << " ";
      std::cout << std::endl;
    }
    assert(are_equal);
}

static void test_bloqqer_interact() {
    std::vector<int> trivial_unsat = {-1, -2, 0, -1, -2, 0, 1, -2, 0, -1, 2, 0, 1, 2, 0 };

    Logger logger = Logger::getMainInstance().copy("<BloqqerCaller>", ".bloqqercaller");
    BloqqerCaller bq(logger);
    int res = bq.process(trivial_unsat, 2, 1, 1);

    assert(res == 20);
}

int main(int argc, char* argv[]) {
    Timer::init();
    Random::init(rand(), rand());
    Logger::init(0, V6_DEBGV);

    test_write();
    test_read();
    test_bloqqer_interact();
}
