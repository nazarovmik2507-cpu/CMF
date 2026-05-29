#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

// -----------------------------
// MarketDataEvent
// -----------------------------
struct MarketDataEvent {
    std::string timestamp;       // ts_recv if present, otherwise ts_event. Kept as raw string for output.
    std::string sort_key;        // normalized key used for chronological comparison.
    std::string ts_event;
    std::string ts_recv;
    std::uint64_t instrument_id = 0;
    std::uint64_t order_id = 0;
    char side = 'N';
    std::string price;           // kept as string because Databento JSON may use pretty_px decimals or integer px.
    std::uint64_t size = 0;
    char action = 'N';
    std::string symbol;
    std::uint64_t sequence = 0;
    std::string source_file;
    std::uint64_t source_line = 0;
};

std::ostream& operator<<(std::ostream& os, const MarketDataEvent& e) {
    os << "ts=" << e.timestamp
       << " instrument_id=" << e.instrument_id;
    if (!e.symbol.empty()) os << " symbol=" << e.symbol;
    os << " order_id=" << e.order_id
       << " side=" << e.side
       << " price=" << e.price
       << " size=" << e.size
       << " action=" << e.action
       << " seq=" << e.sequence
       << " file=" << fs::path(e.source_file).filename().string()
       << ":" << e.source_line;
    return os;
}

// This function is intentionally simple for now. In a real backtester it would update the LOB.
void processMarketDataEvent(const MarketDataEvent& event) {
    std::cout << event << '\n';
}

// -----------------------------
// Minimal JSON field extraction for flat Databento NDJSON lines.
// It also works when common fields are inside nested objects, because it searches the full line for "key".
// -----------------------------
namespace mini_json {

std::optional<std::string> raw_value(const std::string& line, const std::string& key) {
    const std::string quoted_key = "\"" + key + "\"";
    std::size_t pos = line.find(quoted_key);
    if (pos == std::string::npos) return std::nullopt;

    pos = line.find(':', pos + quoted_key.size());
    if (pos == std::string::npos) return std::nullopt;
    ++pos;

    while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos]))) ++pos;
    if (pos >= line.size()) return std::nullopt;

    if (line[pos] == '"') {
        ++pos;
        std::string out;
        bool escape = false;
        for (; pos < line.size(); ++pos) {
            char c = line[pos];
            if (escape) {
                // Enough for these data files; we preserve escaped chars in a readable form.
                out.push_back(c);
                escape = false;
            } else if (c == '\\') {
                escape = true;
            } else if (c == '"') {
                return out;
            } else {
                out.push_back(c);
            }
        }
        return std::nullopt;
    }

    std::size_t end = pos;
    while (end < line.size() && line[end] != ',' && line[end] != '}' && line[end] != ']') ++end;
    while (end > pos && std::isspace(static_cast<unsigned char>(line[end - 1]))) --end;
    std::string value = line.substr(pos, end - pos);
    if (value == "null") return std::string{};
    return value;
}

std::string get_string(const std::string& line, const std::string& key, const std::string& def = "") {
    auto v = raw_value(line, key);
    return v ? *v : def;
}

std::uint64_t get_u64(const std::string& line, const std::string& key, std::uint64_t def = 0) {
    auto v = raw_value(line, key);
    if (!v || v->empty()) return def;
    try {
        // Some JSON encoders can emit integer-looking values as strings.
        return static_cast<std::uint64_t>(std::stoull(*v));
    } catch (...) {
        return def;
    }
}

char get_char(const std::string& line, const std::string& key, char def = 'N') {
    auto v = raw_value(line, key);
    if (!v || v->empty()) return def;
    return (*v)[0];
}

} // namespace mini_json


std::string normalizeTimestampKey(const std::string& ts) {
    if (ts.empty()) return ts;
    const bool numeric = std::all_of(ts.begin(), ts.end(), [](unsigned char c) { return std::isdigit(c); });
    if (numeric) {
        if (ts.size() >= 20) return ts;
        return std::string(20 - ts.size(), '0') + ts;
    }

    // Pretty timestamps are normally ISO-8601 UTC with 9 fractional digits.
    // Normalize fractional seconds to 9 digits so lexicographic order is chronological.
    std::size_t dot = ts.find('.');
    if (dot == std::string::npos) return ts;
    std::size_t frac_start = dot + 1;
    std::size_t frac_end = frac_start;
    while (frac_end < ts.size() && std::isdigit(static_cast<unsigned char>(ts[frac_end]))) ++frac_end;

    std::string frac = ts.substr(frac_start, frac_end - frac_start);
    if (frac.size() < 9) frac.append(9 - frac.size(), '0');
    if (frac.size() > 9) frac.resize(9);

    return ts.substr(0, frac_start) + frac + ts.substr(frac_end);
}

std::optional<MarketDataEvent> parseMarketDataEvent(
    const std::string& line,
    const std::string& source_file,
    std::uint64_t source_line
) {
    if (line.empty()) return std::nullopt;

    MarketDataEvent e;
    e.ts_recv = mini_json::get_string(line, "ts_recv");
    e.ts_event = mini_json::get_string(line, "ts_event");
    e.timestamp = !e.ts_recv.empty() ? e.ts_recv : e.ts_event;
    e.sort_key = normalizeTimestampKey(e.timestamp);

    // A valid event needs at least one timestamp and an action. Other fields can be zero/empty for rare records.
    if (e.timestamp.empty()) return std::nullopt;

    e.instrument_id = mini_json::get_u64(line, "instrument_id");
    e.order_id = mini_json::get_u64(line, "order_id");
    e.side = mini_json::get_char(line, "side", 'N');
    e.price = mini_json::get_string(line, "price", "");
    e.size = mini_json::get_u64(line, "size");
    e.action = mini_json::get_char(line, "action", 'N');
    e.symbol = mini_json::get_string(line, "symbol", "");
    e.sequence = mini_json::get_u64(line, "sequence");
    e.source_file = source_file;
    e.source_line = source_line;

    return e;
}

// Databento pretty_ts timestamps are ISO-like UTC strings, so lexicographic order matches time order.
// If timestamps are numeric strings with equal format, lexicographic also matches; for safety we compare by length first.
struct EventTimeLess {
    bool operator()(const MarketDataEvent& a, const MarketDataEvent& b) const {
        const bool a_num = !a.timestamp.empty() && std::all_of(a.timestamp.begin(), a.timestamp.end(), ::isdigit);
        const bool b_num = !b.timestamp.empty() && std::all_of(b.timestamp.begin(), b.timestamp.end(), ::isdigit);
        (void)a_num;
        (void)b_num;
        if (a.sort_key != b.sort_key) return a.sort_key < b.sort_key;
        if (a.sequence != b.sequence) return a.sequence < b.sequence;
        if (a.source_file != b.source_file) return a.source_file < b.source_file;
        return a.source_line < b.source_line;
    }
};

struct EventTimeGreater {
    bool operator()(const MarketDataEvent& a, const MarketDataEvent& b) const {
        return EventTimeLess{}(b, a);
    }
};

// -----------------------------
// Stats collector: count + first/last timestamp + first 10 and last 10 events.
// -----------------------------
struct ProcessingStats {
    std::uint64_t total = 0;
    std::uint64_t invalid = 0;
    std::string first_timestamp;
    std::string last_timestamp;
    std::vector<MarketDataEvent> first10;
    std::deque<MarketDataEvent> last10;

    void add(const MarketDataEvent& e) {
        if (total == 0) first_timestamp = e.timestamp;
        last_timestamp = e.timestamp;
        ++total;

        if (first10.size() < 10) first10.push_back(e);
        last10.push_back(e);
        if (last10.size() > 10) last10.pop_front();
    }
};

void printStats(const std::string& title, const ProcessingStats& stats, double seconds) {
    std::cout << "\n================ " << title << " ================\n";
    std::cout << "Total valid messages processed: " << stats.total << "\n";
    std::cout << "Invalid/skipped lines: " << stats.invalid << "\n";
    std::cout << "First timestamp: " << stats.first_timestamp << "\n";
    std::cout << "Last timestamp:  " << stats.last_timestamp << "\n";
    std::cout << std::fixed << std::setprecision(3);
    std::cout << "Wall-clock time, seconds: " << seconds << "\n";
    if (seconds > 0.0) {
        std::cout << "Throughput, msg/sec: " << static_cast<double>(stats.total) / seconds << "\n";
    }

    std::cout << "\nFirst 10 MarketDataEvent objects:\n";
    for (const auto& e : stats.first10) processMarketDataEvent(e);

    std::cout << "\nLast 10 MarketDataEvent objects:\n";
    for (const auto& e : stats.last10) processMarketDataEvent(e);
    std::cout << "================================================\n";
}

// -----------------------------
// Bounded blocking queue for producer/merger/dispatcher pipeline.
// -----------------------------
template <typename T>
class BlockingQueue {
public:
    explicit BlockingQueue(std::size_t capacity = 4096) : capacity_(capacity) {}

    bool push(T item) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_not_full_.wait(lock, [&] { return closed_ || queue_.size() < capacity_; });
        if (closed_) return false;
        queue_.push_back(std::move(item));
        cv_not_empty_.notify_one();
        return true;
    }

    bool pop(T& out) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_not_empty_.wait(lock, [&] { return closed_ || !queue_.empty(); });
        if (queue_.empty()) return false;
        out = std::move(queue_.front());
        queue_.pop_front();
        cv_not_full_.notify_one();
        return true;
    }

    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        cv_not_empty_.notify_all();
        cv_not_full_.notify_all();
    }

private:
    std::size_t capacity_;
    std::deque<T> queue_;
    std::mutex mutex_;
    std::condition_variable cv_not_empty_;
    std::condition_variable cv_not_full_;
    bool closed_ = false;
};

using EventQueue = BlockingQueue<MarketDataEvent>;

void producerFileReader(const fs::path& file_path, EventQueue& out, std::atomic<std::uint64_t>& invalid_count) {
    std::ifstream in(file_path);
    if (!in) {
        std::cerr << "Cannot open input file: " << file_path << "\n";
        out.close();
        return;
    }

    std::string line;
    std::uint64_t line_no = 0;
    while (std::getline(in, line)) {
        ++line_no;
        auto maybe_event = parseMarketDataEvent(line, file_path.string(), line_no);
        if (maybe_event) {
            if (!out.push(std::move(*maybe_event))) break;
        } else {
            ++invalid_count;
        }
    }
    out.close();
}

ProcessingStats dispatcherConsume(EventQueue& in) {
    ProcessingStats stats;
    MarketDataEvent e;
    while (in.pop(e)) {
        stats.add(e);
        // Keep output manageable. processMarketDataEvent is called for first/last 10 in printStats().
    }
    return stats;
}

// -----------------------------
// Standard task: one file, line-by-line streaming.
// -----------------------------
ProcessingStats runStandardFile(const fs::path& file_path) {
    std::ifstream in(file_path);
    if (!in) throw std::runtime_error("Cannot open input file: " + file_path.string());

    ProcessingStats stats;
    std::string line;
    std::uint64_t line_no = 0;
    while (std::getline(in, line)) {
        ++line_no;
        auto maybe_event = parseMarketDataEvent(line, file_path.string(), line_no);
        if (maybe_event) stats.add(*maybe_event);
        else ++stats.invalid;
    }
    return stats;
}

// -----------------------------
// Flat merger: one priority queue holding next pending event from each file.
// -----------------------------
struct HeapItem {
    MarketDataEvent event;
    std::size_t stream_index;
};

struct HeapItemGreater {
    bool operator()(const HeapItem& a, const HeapItem& b) const {
        return EventTimeGreater{}(a.event, b.event);
    }
};

ProcessingStats runFlatMerger(const std::vector<fs::path>& files) {
    std::vector<std::unique_ptr<EventQueue>> queues;
    queues.reserve(files.size());
    for (std::size_t i = 0; i < files.size(); ++i) queues.push_back(std::make_unique<EventQueue>(4096));

    std::atomic<std::uint64_t> invalid_count{0};
    std::vector<std::thread> producers;
    producers.reserve(files.size());
    for (std::size_t i = 0; i < files.size(); ++i) {
        producers.emplace_back(producerFileReader, files[i], std::ref(*queues[i]), std::ref(invalid_count));
    }

    EventQueue merged_out(8192);
    ProcessingStats stats;
    std::thread dispatcher([&] { stats = dispatcherConsume(merged_out); });

    std::priority_queue<HeapItem, std::vector<HeapItem>, HeapItemGreater> heap;
    for (std::size_t i = 0; i < queues.size(); ++i) {
        MarketDataEvent e;
        if (queues[i]->pop(e)) heap.push(HeapItem{std::move(e), i});
    }

    while (!heap.empty()) {
        HeapItem item = heap.top();
        heap.pop();
        merged_out.push(std::move(item.event));

        MarketDataEvent next;
        if (queues[item.stream_index]->pop(next)) {
            heap.push(HeapItem{std::move(next), item.stream_index});
        }
    }

    merged_out.close();
    dispatcher.join();
    for (auto& t : producers) if (t.joinable()) t.join();
    stats.invalid = invalid_count.load();
    return stats;
}

// -----------------------------
// Hierarchical merger: binary tree of streaming 2-way merge threads.
// -----------------------------
void mergeTwoQueues(EventQueue& a, EventQueue& b, EventQueue& out) {
    MarketDataEvent ea, eb;
    bool has_a = a.pop(ea);
    bool has_b = b.pop(eb);

    EventTimeLess less;
    while (has_a && has_b) {
        if (less(ea, eb)) {
            out.push(std::move(ea));
            has_a = a.pop(ea);
        } else {
            out.push(std::move(eb));
            has_b = b.pop(eb);
        }
    }
    while (has_a) {
        out.push(std::move(ea));
        has_a = a.pop(ea);
    }
    while (has_b) {
        out.push(std::move(eb));
        has_b = b.pop(eb);
    }
    out.close();
}

ProcessingStats runHierarchyMerger(const std::vector<fs::path>& files) {
    std::atomic<std::uint64_t> invalid_count{0};

    std::vector<std::shared_ptr<EventQueue>> current_level;
    current_level.reserve(files.size());
    std::vector<std::shared_ptr<EventQueue>> all_queues;
    all_queues.reserve(files.size() * 2 + 2);
    std::vector<std::thread> threads;

    for (const auto& file : files) {
        auto q = std::make_shared<EventQueue>(4096);
        all_queues.push_back(q);
        current_level.push_back(q);
        threads.emplace_back(producerFileReader, file, std::ref(*q), std::ref(invalid_count));
    }

    while (current_level.size() > 1) {
        std::vector<std::shared_ptr<EventQueue>> next_level;
        for (std::size_t i = 0; i < current_level.size(); i += 2) {
            if (i + 1 == current_level.size()) {
                next_level.push_back(current_level[i]);
            } else {
                auto out = std::make_shared<EventQueue>(8192);
                all_queues.push_back(out);
                threads.emplace_back(mergeTwoQueues, std::ref(*current_level[i]), std::ref(*current_level[i + 1]), std::ref(*out));
                next_level.push_back(out);
            }
        }
        current_level = std::move(next_level);
    }

    ProcessingStats stats;
    if (!current_level.empty()) {
        stats = dispatcherConsume(*current_level[0]);
    }

    for (auto& t : threads) if (t.joinable()) t.join();
    stats.invalid = invalid_count.load();
    return stats;
}

std::vector<fs::path> findMarketDataFiles(const fs::path& folder) {
    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(folder)) {
        if (!entry.is_regular_file()) continue;
        const auto p = entry.path();
        const std::string name = p.filename().string();
        if (name.size() >= 9 && name.find(".mbo.json") != std::string::npos) {
            files.push_back(p);
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

void printUsage(const char* argv0) {
    std::cerr << "Usage:\n"
              << "  " << argv0 << " <path_to_single_mbo_json_file>\n"
              << "  " << argv0 << " <path_to_folder_with_mbo_json_files> [--mode flat|hierarchy|both]\n\n"
              << "Examples:\n"
              << "  " << argv0 << " sample_data/sample_1.mbo.json\n"
              << "  " << argv0 << " /content/drive/MyDrive/XEUR-20260409-HJTR7RCAKT --mode both\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printUsage(argv[0]);
        return 1;
    }

    fs::path input_path = argv[1];
    std::string mode = "both";
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--mode" && i + 1 < argc) {
            mode = argv[++i];
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            printUsage(argv[0]);
            return 1;
        }
    }

    try {
        if (fs::is_regular_file(input_path)) {
            const auto start = std::chrono::steady_clock::now();
            auto stats = runStandardFile(input_path);
            const auto end = std::chrono::steady_clock::now();
            const double seconds = std::chrono::duration<double>(end - start).count();
            printStats("STANDARD TASK: single-file ingestion", stats, seconds);
            return 0;
        }

        if (!fs::is_directory(input_path)) {
            std::cerr << "Input path is not a file or directory: " << input_path << "\n";
            return 1;
        }

        auto files = findMarketDataFiles(input_path);
        if (files.empty()) {
            std::cerr << "No *.mbo.json files found in: " << input_path << "\n";
            return 1;
        }

        std::cout << "Found " << files.size() << " market data files:\n";
        for (const auto& f : files) std::cout << "  " << f.filename().string() << "\n";

        if (mode == "flat" || mode == "both") {
            const auto start = std::chrono::steady_clock::now();
            auto stats = runFlatMerger(files);
            const auto end = std::chrono::steady_clock::now();
            const double seconds = std::chrono::duration<double>(end - start).count();
            printStats("HARD TASK: flat k-way merger", stats, seconds);
        }

        if (mode == "hierarchy" || mode == "both") {
            const auto start = std::chrono::steady_clock::now();
            auto stats = runHierarchyMerger(files);
            const auto end = std::chrono::steady_clock::now();
            const double seconds = std::chrono::duration<double>(end - start).count();
            printStats("HARD TASK: hierarchical binary-tree merger", stats, seconds);
        }

        if (!(mode == "flat" || mode == "hierarchy" || mode == "both")) {
            std::cerr << "Invalid mode: " << mode << ". Use flat, hierarchy, or both.\n";
            return 1;
        }

    } catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << "\n";
        return 1;
    }

    return 0;
}
