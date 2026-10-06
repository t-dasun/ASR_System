#include <asr/engines/prefix_pool.hpp>
#include <atomic>
#include <future>
#include <stdexcept>
using namespace asr;
struct Sink : IRecognitionSink {
    std::string call;
    std::atomic<int> finals{0}, failures{0};
    void on_event(const RecognitionEvent &e) override {
        if (e.call_id != call)
            throw std::runtime_error("cross-call event");
        if (e.kind == EventKind::final) {
            if (e.text != call)
                throw std::runtime_error("cross-call text");
            ++finals;
        }
        if (e.kind == EventKind::failed)
            ++failures;
    }
};
void check(bool v) {
    if (!v)
        throw std::runtime_error("prefix process pool contract failed");
}
int main(int argc, char **argv) {
    if (argc != 2)
        return 1;
    SteadyClock clock;
    {
        PrefixProcessPool pool(argv[1], "stub", 2, 2, 4000, 4, 30000, 60000, 1000);
        std::vector<std::unique_ptr<Sink>> sinks;
        std::vector<std::unique_ptr<IASRSession>> sessions;
        for (int i = 0; i < 4; ++i) {
            auto sink = std::make_unique<Sink>();
            sink->call = "call_" + std::to_string(i);
            SessionConfig c;
            c.run_id = "test";
            c.call_id = sink->call;
            auto r = pool.create_session(c, *sink, clock);
            check(bool(r));
            sessions.push_back(std::move(r.value));
            sinks.push_back(std::move(sink));
        }
        check(sessions[0]->snapshot().worker_id != sessions[1]->snapshot().worker_id);
        check(sessions[0]->snapshot().worker_id == sessions[2]->snapshot().worker_id);
        SessionConfig extra;
        extra.run_id = "test";
        extra.call_id = "extra";
        Sink sink;
        sink.call = "extra";
        check(!pool.create_session(extra, sink, clock));
        auto pending = std::async(std::launch::async, [&] { return sessions[0]->finish_input(); });
        AudioChunk c;
        c.call_id = "call_2";
        c.run_id = "test";
        c.pcm = std::make_shared<const std::vector<std::int16_t>>(3200, 1);
        auto invalid = c;
        invalid.call_id = "call_3";
        check(!sessions[2]->submit(invalid));
        auto start = std::chrono::steady_clock::now();
        check(bool(sessions[2]->submit(c)));
        check(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(150));
        check(bool(pending.get()));
        check(sinks[0]->finals == 1);
        for (int i = 1; i < 4; ++i)
            check(bool(sessions[i]->finish_input()));
        auto workers = pool.workers();
        check(workers.size() == 2 && workers[0].healthy && workers[0].process_id != workers[1].process_id);
        sessions.clear();
        auto r = pool.create_session(extra, sink, clock);
        check(bool(r));
        check(bool(r.value->cancel(CancelReason::user_request)));
        r.value.reset();
        pool.begin_draining();
        check(!pool.create_session(extra, sink, clock));
    }
    {
        PrefixProcessPool pool(argv[1], "crash", 2, 1, 4000, 4, 30000, 60000, 1000);
        Sink sink;
        sink.call = "crash";
        SessionConfig c;
        c.run_id = "test";
        c.call_id = sink.call;
        auto r = pool.create_session(c, sink, clock);
        check(bool(r));
        AudioChunk chunk;
        chunk.run_id = c.run_id;
        chunk.call_id = c.call_id;
        chunk.pcm = std::make_shared<const std::vector<std::int16_t>>(3200, 1);
        check(!r.value->submit(chunk));
        r.value.reset();
        check(sink.failures == 1);
        Sink survivor;
        survivor.call = "survivor";
        c.call_id = survivor.call;
        auto next = pool.create_session(c, survivor, clock);
        check(bool(next));
        check(bool(next.value->finish_input()));
        check(survivor.finals == 1);
    }
}
