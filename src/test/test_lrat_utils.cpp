
#include "app/sat/proof/lrat_utils.hpp"
#include "util/assert.hpp"
#include "util/sys/buffered_io.hpp"

void test() {

    LratLine line;
    {
        std::ofstream ofs("test.lrat", std::ios_base::binary);
        lrat_utils::WriteBuffer buf(ofs);

        line.id = 10;
        line.literals = {1, -2, 3};
        line.hints = {4, 5, 6};
        lrat_utils::writeLine(buf, line);

        line.id = 111111;
        line.literals.push_back(-100000);
        line.hints.push_back(99999);
        lrat_utils::writeLine(buf, line);

        line.id = 111112;
        line.literals.clear();
        line.hints = {111111};
        lrat_utils::writeLine(buf, line);
    }
    {
        LratLine readLine;
        std::ifstream ifs("test.lrat", std::ios_base::binary);
        BufferedFileReader bufread(ifs);
        lrat_utils::ReadBuffer buf(bufread);
        
        bool success = lrat_utils::readLine(buf, readLine);
        assert(success);
        assert(readLine.id == 10);
        assert(readLine.literals.size() == 3);
        assert(readLine.hints.size() == 3);

        success = lrat_utils::readLine(buf, readLine);
        assert(success);
        assert(readLine.id == 111111);
        assert(readLine.literals.size() == 4 || log_return_false("%i\n", readLine.literals.size()));
        assert(readLine.literals.back() == -100000);
        assert(readLine.hints.size() == 4);
        assert(readLine.hints.back() == 99999);

        success = lrat_utils::readLine(buf, readLine);
        assert(success);
        assert(readLine.id == 111112);
        assert(readLine.literals.size() == 0);
        assert(readLine.hints.size() == 1);
        assert(readLine.hints.back() == 111111);

        LratLine noLine;
        assert(!lrat_utils::readLine(buf, noLine));
    }
}

int main() {
    test();
}

