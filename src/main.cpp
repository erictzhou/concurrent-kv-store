#include <iostream>

#include "common/version.h"
#include "parser/cli_parser.h"
#include "persistence/snapshot.h"
#include "persistence/wal.h"
#include "server/cli_server.h"
#include "store/kv_store.h"

/**
 * @brief Creates the application components and starts the CLI loop.
 *
 * @return Exit status code for the operating system.
 */
int main(int argc, char* argv[]) {
    if (argc > 1) {
        std::cerr << "usage: " << argv[0] << '\n';
        return 1;
    }

    std::cout << "Concurrent KV Store v" << kv::common::kProjectVersion
              << '\n';
    std::cout << "Concurrency: " << kv::common::kConcurrencyModel << '\n';

    // Wire buffered WAL persistence into the store before replay. Buffered
    // acknowledgements do not guarantee stable-storage persistence.
    kv::persistence::WriteAheadLog wal;
    kv::persistence::Snapshot snapshot;
    kv::store::KVStore store(&wal, &snapshot);

    std::cout << "Loading snapshot...\n";

    const kv::persistence::SnapshotLoadResult snapshot_result =
        store.LoadSnapshot(snapshot);
    std::cout << "Loaded " << snapshot_result.entry_count
              << " snapshot entrie(s)\n";

    std::cout << "Replaying WAL...\n";

    const std::size_t recovered_operations =
        store.ReplayFromWal(wal, snapshot_result.wal_offset);
    std::cout << "Recovered " << recovered_operations << " operation(s)\n";

    kv::parser::CliParser parser;
    kv::server::CliServer server(parser, store);

    server.Run(std::cin, std::cout);
    return 0;
}
