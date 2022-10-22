
#pragma once

#include <string>
#include <fstream>

class JobIdAllocator {

private:
    int _client_rank;
    int _running_id;
    std::string _id_file;

public:
    JobIdAllocator() {}
    JobIdAllocator(int clientRank, const std::string& apiDirectory) 
        : _client_rank(clientRank) {

        _running_id  = _client_rank * 100000 + 1;
        _id_file = apiDirectory + "/.last_id";
        std::ifstream idFileIfs(_id_file);
        if (idFileIfs.is_open()) {
            idFileIfs >> _running_id;
            _running_id++;
        }
    }

    int getNext() {
        int nextId = _running_id++;
        {
            std::ofstream ofs(_id_file);
            if (ofs.is_open()) ofs << nextId;
        }
        return nextId;
    }
};
