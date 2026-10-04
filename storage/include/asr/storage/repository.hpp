#pragma once
#include <asr/core/types.hpp>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>

namespace asr {
nlohmann::json event_json(const RecognitionEvent &event);
nlohmann::json environment_json(const std::string &engine = "mock");
std::string new_run_id(const std::string &prefix = "mock");
class IResultRepository {
  public:
    virtual ~IResultRepository() = default;
    virtual void begin(const std::string &run_id, const nlohmann::json &config,
                       const nlohmann::json &environment) = 0;
    virtual void append(const RecognitionEvent &event) = 0;
    virtual void append_audio(const nlohmann::json &timing) = 0;
    virtual void append_record(const std::string &stream, const nlohmann::json &record) = 0;
    virtual void finish(const std::string &status, const nlohmann::json &summary) = 0;
};
class MemoryResultRepository final : public IResultRepository {
    bool started_ = false, sealed_ = false;

  public:
    std::string run_id, status;
    nlohmann::json config, environment, summary;
    std::vector<RecognitionEvent> events;
    std::vector<nlohmann::json> audio_timings;
    std::map<std::string, std::vector<nlohmann::json>> records;
    void begin(const std::string &, const nlohmann::json &, const nlohmann::json &) override;
    void append(const RecognitionEvent &) override;
    void append_audio(const nlohmann::json &) override;
    void append_record(const std::string &, const nlohmann::json &) override;
    void finish(const std::string &, const nlohmann::json &) override;
};
class FileResultRepository final : public IResultRepository {
    mutable std::mutex mutex_;
    std::filesystem::path root_, directory_;
    std::ofstream events_, audio_timings_;
    std::map<std::string, std::ofstream> records_;
    bool started_ = false, sealed_ = false;
    bool is_mock_ = true;
    std::string run_id_;
    void write_status(const std::string &status);

  public:
    explicit FileResultRepository(std::filesystem::path root) : root_(std::move(root)) {}
    void begin(const std::string &, const nlohmann::json &, const nlohmann::json &) override;
    void append(const RecognitionEvent &) override;
    void append_audio(const nlohmann::json &) override;
    void append_record(const std::string &, const nlohmann::json &) override;
    void finish(const std::string &, const nlohmann::json &) override;
    const std::filesystem::path &directory() const { return directory_; }
};
} // namespace asr
