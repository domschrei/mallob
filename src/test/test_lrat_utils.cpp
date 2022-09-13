
#include "app/sat/proof/lrat_utils.hpp"
#include "util/assert.hpp"

void test() {

    LratLine line;
    {
        std::ofstream ofs("test.lrat", std::ios_base::binary);
        lrat_utils::WriteBuffer buf(ofs);

        line.id = 10;
        line.literals = {1, -2, 3};
        line.hints = {4, 5, 6};
        line.signsOfHints = {false, true, false};
        lrat_utils::writeLine(buf, line);

        line.id = 111111;
        line.literals.push_back(-100000);
        line.hints.push_back(99999);
        line.signsOfHints.push_back(false);
        lrat_utils::writeLine(buf, line);

        line.id = 111112;
        line.literals.clear();
        line.hints = {111111};
        line.signsOfHints = {true};
        lrat_utils::writeLine(buf, line);
    }
    {
        LratLine readLine;
        std::ifstream ifs("test.lrat", std::ios_base::binary);
        lrat_utils::ReadBuffer buf(ifs);
        
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
        assert(readLine.hints.back() == 99999 && readLine.signsOfHints.back() == false);

        success = lrat_utils::readLine(buf, readLine);
        assert(success);
        assert(readLine.id == 111112);
        assert(readLine.literals.size() == 0);
        assert(readLine.hints.size() == 1);
        assert(readLine.hints.back() == 111111 && readLine.signsOfHints.back() == true);

        LratLine noLine;
        assert(!lrat_utils::readLine(buf, noLine));
    }
}

int main() {
    test();
}

