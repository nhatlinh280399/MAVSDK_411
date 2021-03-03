#include <iostream>
#include <future>
#include <sstream>
#include "mavsdk.h"
#include "integration_test_helper.h"
#include "plugins/log_files/log_files.h"

using namespace mavsdk;

TEST(HardwareTest, LogFiles)
{
    Mavsdk mavsdk;

    // ConnectionResult ret = mavsdk.add_serial_connection("/dev/ttyACM0");
    ConnectionResult ret = mavsdk.add_udp_connection();
    ASSERT_EQ(ret, ConnectionResult::Success);

    std::this_thread::sleep_for(std::chrono::seconds(2));

    auto system = mavsdk.systems().at(0);
    ASSERT_TRUE(system->has_autopilot());
    auto log_files = std::make_shared<LogFiles>(system);

    std::pair<LogFiles::Result, std::vector<LogFiles::Entry>> entry_result =
        log_files->get_entries();

    EXPECT_EQ(entry_result.first, LogFiles::Result::Success);

    unsigned num_downloaded_files = 0;

    if (entry_result.first == LogFiles::Result::Success) {
        for (auto& entry : entry_result.second) {
            float size_mib = entry.size_bytes / 1024.0f / 1024.0f;
            LogInfo() << "Entry " << entry.id << ": "
                      << " at " << entry.date << ", " << size_mib
                      << " MiB, bytes: " << entry.size_bytes;
            std::stringstream file_path_stream;
            file_path_stream << "/tmp/logfile_" << entry.id << ".ulog";

            auto prom = std::promise<void>();
            auto fut = prom.get_future();

            log_files->download_log_file_async(
                entry.id,
                file_path_stream.str(),
                [&prom](LogFiles::Result result, LogFiles::ProgressData progress_data) {
                    if (result == LogFiles::Result::Next) {
                        LogInfo() << "Download progress: " << 100.0f * progress_data.progress;
                    } else {
                        EXPECT_EQ(result, LogFiles::Result::Success);
                        prom.set_value();
                    }
                });

            fut.wait();

            if (++num_downloaded_files >= 2) {
                break;
            }
        }
    }
}
