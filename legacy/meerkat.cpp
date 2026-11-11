#include <iostream>
#include <fstream>
#include <string>
#include <cstdio>
#include <memory>
#include <array>
#include <chrono>
#include <thread>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <regex>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// Global variables for clean shutdown
volatile sig_atomic_t keep_running = 1;
pid_t inhibit_pid = -1;

void signal_handler(int signum) {
    keep_running = 0;
    if (inhibit_pid > 0) {
        kill(inhibit_pid, SIGTERM);
    }
}

// Execute a shell command and capture its standard output
std::string exec_cmd(const char* cmd) {
    std::array<char, 256> buffer;
    std::string result;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd, "r"), pclose);
    if (!pipe) {
        return "";
    }
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        result += buffer.data();
    }
    return result;
}

// Get current timestamp in YYYY-MM-DD HH:MM:SS format
std::string get_timestamp() {
    auto now = std::chrono::system_clock::now();
    std::time_t now_c = std::chrono::system_clock::to_time_t(now);
    std::tm now_tm = *std::localtime(&now_c);
    std::ostringstream oss;
    oss << std::put_time(&now_tm, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

// Spawn a background systemd-inhibit process tied to this program's lifecycle
pid_t start_sleep_inhibitor() {
    pid_t pid = fork();
    if (pid == 0) {
        // Child process: block sleep and idle while running 'cat' (waits indefinitely)
        execlp("systemd-inhibit", "systemd-inhibit",
               "--what=idle:sleep",
               "--who=NetMonitor",
               "--why=Monitoring network and internet performance",
               "--mode=block",
               "sleep", "infinity", NULL);
        // If execlp fails, exit child silently
        _exit(1);
    }
    return pid;
}

struct WlanStats {
    std::string interface = "N/A";
    double tx_bitrate_mbps = 0.0;
    double rx_bitrate_mbps = 0.0;
    double gateway_ping_ms = 0.0;
};

struct InternetStats {
    double download_mbps = 0.0;
    double upload_mbps = 0.0;
    double ping_ms = 0.0;
};

WlanStats get_wlan_stats() {
    WlanStats stats;

    // 1. Identify wireless interface
    std::string dev_out = exec_cmd("iw dev 2>/dev/null | awk '$1==\"Interface\"{print $2; exit}'");
    if (!dev_out.empty() && dev_out.back() == '\n') {
        dev_out.pop_back();
    }
    if (dev_out.empty()) {
        return stats; // No active wireless interface found
    }
    stats.interface = dev_out;

    // 2. Get WLAN Link Speeds (Tx/Rx Bitrate)
    std::string link_cmd = "iw dev " + stats.interface + " link 2>/dev/null";
    std::string link_out = exec_cmd(link_cmd.c_str());

    std::regex tx_regex(R"(tx bitrate:\s+([0-9.]+)\s+MBit/s)");
    std::regex rx_regex(R"(rx bitrate:\s+([0-9.]+)\s+MBit/s)");
    std::smatch match;

    if (std::regex_search(link_out, match, tx_regex)) {
        stats.tx_bitrate_mbps = std::stod(match[1].str());
    }
    if (std::regex_search(link_out, match, rx_regex)) {
        stats.rx_bitrate_mbps = std::stod(match[1].str());
    }

    // 3. Get Default Gateway IP for the wireless interface and Ping it
    std::string gw_cmd = "ip route show dev " + stats.interface + " 2>/dev/null | awk '/default/ {print $3; exit}'";
    std::string gw_ip = exec_cmd(gw_cmd.c_str());
    if (!gw_ip.empty() && gw_ip.back() == '\n') {
        gw_ip.pop_back();
    }

    if (!gw_ip.empty()) {
        std::string ping_cmd = "ping -c 3 -W 2 -q " + gw_ip + " 2>/dev/null";
        std::string ping_out = exec_cmd(ping_cmd.c_str());
        // Extract average ping from rtt min/avg/max/mdev = 1.123/2.345/3.456/0.123 ms
        std::regex rtt_regex(R"(=\s+[0-9.]+/([0-9.]+)/[0-9.]+/[0-9.]+\s+ms)");
        if (std::regex_search(ping_out, match, rtt_regex)) {
            stats.gateway_ping_ms = std::stod(match[1].str());
        }
    }

    return stats;
}

InternetStats get_internet_stats() {
    InternetStats stats;
    std::string json_out = exec_cmd("speedtest-cli --json 2>/dev/null");
    if (json_out.empty()) {
        return stats;
    }

    try {
        auto parsed = json::parse(json_out);
        // speedtest-cli outputs bits per second; convert to Mbps (divide by 1,000,000)
        if (parsed.contains("download") && !parsed["download"].is_null()) {
            stats.download_mbps = parsed["download"].get<double>() / 1e6;
        }
        if (parsed.contains("upload") && !parsed["upload"].is_null()) {
            stats.upload_mbps = parsed["upload"].get<double>() / 1e6;
        }
        if (parsed.contains("ping") && !parsed["ping"].is_null()) {
            stats.ping_ms = parsed["ping"].get<double>();
        }
    } catch (const std::exception& e) {
        std::cerr << "[Warning] Failed to parse speedtest-cli JSON output.\n";
    }

    return stats;
}

void print_usage(const char* prog_name) {
    std::cout << "Usage: " << prog_name << " [-i interval_minutes] [-o output_csv]\n"
              << "Options:\n"
              << "  -i <minutes>  Frequency of checks in minutes (default: 5)\n"
              << "  -o <file>     Output CSV file path (default: network_stats.csv)\n"
              << "  -h            Show this help message\n";
}

int main(int argc, char* argv[]) {
    int interval_minutes = 5;
    std::string csv_filename = "network_stats.csv";

    int opt;
    while ((opt = getopt(argc, argv, "i:o:h")) != -1) {
        switch (opt) {
            case 'i':
                try {
                    interval_minutes = std::stoi(optarg);
                    if (interval_minutes <= 0) throw std::invalid_argument("must be positive");
                } catch (...) {
                    std::cerr << "Error: Interval (-i) must be a positive integer.\n";
                    return 1;
                }
                break;
            case 'o':
                csv_filename = optarg;
                break;
            case 'h':
                print_usage(argv[0]);
                return 0;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }

    // Register signal handlers for graceful cleanup
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Prevent system from sleeping
    inhibit_pid = start_sleep_inhibitor();
    std::cout << "[Info] Sleep inhibitor started (PID: " << inhibit_pid << ").\n";
    std::cout << "[Info] Logging to '" << csv_filename << "' every " << interval_minutes << " minute(s). Press Ctrl+C to stop.\n";

    // Write CSV header if file does not exist or is empty
    bool write_header = false;
    std::ifstream check_file(csv_filename);
    if (!check_file.good() || check_file.peek() == std::ifstream::traits_type::eof()) {
        write_header = true;
    }
    check_file.close();

    if (write_header) {
        std::ofstream csv_file(csv_filename, std::ios::app);
        if (!csv_file.is_open()) {
            std::cerr << "Error: Unable to open '" << csv_filename << "' for writing.\n";
            if (inhibit_pid > 0) kill(inhibit_pid, SIGTERM);
            return 1;
        }
        csv_file << "Timestamp,WLAN_Interface,WLAN_Tx_Mbps,WLAN_Rx_Mbps,WLAN_Gateway_Ping_ms,Internet_Download_Mbps,Internet_Upload_Mbps,Internet_Ping_ms\n";
    }

    // Main monitoring loop
    while (keep_running) {
        std::string timestamp = get_timestamp();
        std::cout << "[" << timestamp << "] Running network checks...\n";

        WlanStats wlan = get_wlan_stats();
        InternetStats inet = get_internet_stats();

        if (!keep_running) break; // Check if interrupted during command execution

        std::ofstream csv_file(csv_filename, std::ios::app);
        if (csv_file.is_open()) {
            csv_file << std::fixed << std::setprecision(2)
                     << timestamp << ","
                     << wlan.interface << ","
                     << wlan.tx_bitrate_mbps << ","
                     << wlan.rx_bitrate_mbps << ","
                     << wlan.gateway_ping_ms << ","
                     << inet.download_mbps << ","
                     << inet.upload_mbps << ","
                     << inet.ping_ms << "\n";
            csv_file.close();
        } else {
            std::cerr << "[Error] Could not write to CSV file.\n";
        }

        std::cout << "[" << get_timestamp() << "] Recorded -> "
                  << "WLAN (" << wlan.interface << "): Tx " << wlan.tx_bitrate_mbps << " Mbps, Ping " << wlan.gateway_ping_ms << " ms | "
                  << "Internet: Down " << inet.download_mbps << " Mbps, Up " << inet.upload_mbps << " Mbps, Ping " << inet.ping_ms << " ms\n";

        // Wait for the next cycle in 1-second increments so Ctrl+C responds immediately
        int total_seconds = interval_minutes * 60;
        for (int s = 0; s < total_seconds && keep_running; ++s) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    // Clean up inhibitor process
    if (inhibit_pid > 0) {
        kill(inhibit_pid, SIGTERM);
        waitpid(inhibit_pid, nullptr, 0);
    }
    std::cout << "\n[Info] Sleep inhibitor released. Exiting cleanly.\n";
    return 0;
}
