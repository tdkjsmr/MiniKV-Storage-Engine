#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

#include "minikv/database.hpp"
#include "minikv/version.hpp"

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: minikv_install_consumer DATA_DIRECTORY\n";
        return 2;
    }

    const std::string directory = argv[1];
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        std::cerr << "cannot create data directory: " << error.message() << '\n';
        return 3;
    }

    minikv::Options options;
    std::unique_ptr<minikv::Database> database;
    minikv::DatabaseOpenResult open_result;
    auto status = minikv::Database::Open(
        directory,
        options,
        &database,
        &open_result
    );
    if (!status.ok()) {
        std::cerr << status.ToString() << '\n';
        return 4;
    }
    status = database->Put("installed-key", minikv::EngineVersion());
    if (!status.ok() ||
        database->Get("installed-key").value != minikv::EngineVersion()) {
        std::cerr << "installed library Put/Get failed\n";
        return 5;
    }
    status = database->Close();
    if (!status.ok()) {
        std::cerr << status.ToString() << '\n';
        return 6;
    }

    database.reset();
    status = minikv::Database::Open(
        directory,
        options,
        &database,
        &open_result
    );
    if (!status.ok() ||
        database->Get("installed-key").value != minikv::EngineVersion()) {
        std::cerr << "installed library restart failed\n";
        return 7;
    }
    std::cout << "MiniKV " << minikv::EngineVersion()
              << " install consumer passed\n";
    return 0;
}
