
#pragma once

#include <string>
#include <fstream>

class JobIdAllocator {

private:
    int _client_rank;
    int _num_clients;
    int _running_id {0};
    std::string _id_file;

public:
    JobIdAllocator() {}
    JobIdAllocator(int clientRank, int numClients, const std::string& apiDirectory) 
        : _client_rank(clientRank), _num_clients(numClients) {

        _id_file = apiDirectory + "/.last_id";
        std::ifstream idFileIfs(_id_file);
        if (idFileIfs.is_open()) {
            idFileIfs >> _running_id;
        }

        if (_running_id == 0) {
            _running_id = _client_rank;
        }
        
        int nextId = _running_id;
        // Ensure running ID to be different to all other client processes ...
        if (nextId % _num_clients != _client_rank) {
            nextId -= nextId % _num_clients;
            nextId += _client_rank;
        }
        // ... while also not reusing any job ID from a previous run
        while (nextId <= _running_id) nextId += _num_clients;
        _running_id = nextId;
    }

    int getNext() {
        int nextId = _running_id;
        _running_id += _num_clients;
        {
            std::ofstream ofs(_id_file);
            if (ofs.is_open()) ofs << nextId;
        }
        return nextId;
    }
};
