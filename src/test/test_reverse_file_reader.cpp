
#include <stdio.h>
#include <string>

#include "util/reverse_file_reader.hpp"

void test() {
    ReverseFileReader reader("test.txt");
    std::string out;
    char c;
    while (reader.next(c)) out += c;
    printf("%s\n", out.c_str());
}

int main() {
    test();
}
