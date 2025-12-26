#include <iostream>
#include <thread>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <memory>
#include <string>

#include "WebSocketClient.h"

// IMPORTANT!
// Make sure to match the number of cases as per fuzzingserver
constexpr int totalTestCases = 516;

// Small helper to wait for a predicate with timeout
static bool wait_for_pred(std::condition_variable& cv,
                          std::unique_lock<std::mutex>& lk,
                          std::chrono::milliseconds dur,
                          const std::function<bool()>& pred)
{
    return cv.wait_for(lk, dur, pred);
}

int main() {
    for (int i = 1; i <= totalTestCases; ++i) {
        std::cout << "\n--- Running test case " << i << " ---\n";

        auto client = std::make_shared<WebSocketClient>();
        std::weak_ptr<WebSocketClient> wclient = client;

        std::mutex              mtx;
        std::condition_variable cv;
        bool                    done = false;

        // Once closing begins, stop echoing to avoid sending after close handshake starts.
        std::atomic_bool closing{false};

        auto finish = [&]() {
            {
                std::lock_guard<std::mutex> lk(mtx);
                done = true;
            }
            cv.notify_one();
        };

        // Echo text exactly as received
        client->setMessageCallback([wclient, &closing](const std::string& message) {
            if (closing.load(std::memory_order_acquire)) return;
            if (auto c = wclient.lock()) {
                c->sendMessage(message);
            }
        });

        // Echo binary exactly as received
        client->setBinaryCallback([wclient, &closing](const void* data, size_t length) {
            if (closing.load(std::memory_order_acquire)) return;
            if (auto c = wclient.lock()) {
                c->sendBinary(data, length);
            }
        });

        client->setOpenCallback([i]() {
            std::cout << "Connected (case " << i << ")\n";
        });

        client->setCloseCallback([&](int code, const std::string& reason) {
            closing.store(true, std::memory_order_release);
            std::cout << "Closed by server: \"" << reason
                      << "\" (code=" << code << ")\n";
            finish();
        });

        client->setErrorCallback([&](int error_code, const std::string& error_message) {
            closing.store(true, std::memory_order_release);
            std::cout << "Error (" << error_code << "): " << error_message << "\n";
            finish();
        });

        // Start the test case
        std::string url =
            "ws://192.168.0.27:9001/runCase?case=" +
            std::to_string(i) + "&agent=libwsc";

        client->setUrl(url);
        client->connect();

        // Wait until the server closes the case (or we time out)
        {
            std::unique_lock<std::mutex> lk(mtx);
            const bool ok = wait_for_pred(
                cv, lk,
                std::chrono::seconds(20),
                [&]() { return done; }
            );

            if (!ok) {
                // Timeout: force full shutdown.
                closing.store(true, std::memory_order_release);
                std::cout << "⏳ Timeout waiting for case close; disconnecting\n";
            }
        }

        // Ensure the client is fully shut down before starting next case.
        // Call exactly once per case.
        client->disconnect();

        // Drop the strong ref explicitly before next iteration.
        client.reset();
    }

    // Final report
    std::cout << "\n--- Reporting results ---\n";
    {
        auto reportClient = std::make_shared<WebSocketClient>();
        std::weak_ptr<WebSocketClient> w = reportClient;

        std::mutex              mtx;
        std::condition_variable cv;
        bool                    done = false;

        auto finish = [&]() {
            {
                std::lock_guard<std::mutex> lk(mtx);
                done = true;
            }
            cv.notify_one();
        };

        reportClient->setOpenCallback([]() {
            std::cout << "Connected to report endpoint\n";
        });

        reportClient->setCloseCallback([&](int code, const std::string& reason) {
            std::cout << "Report closed: " << code << " \"" << reason << "\"\n";
            finish();
        });

        reportClient->setErrorCallback([&](int error_code, const std::string& err) {
            std::cout << "Report error (" << error_code << "): " << err << "\n";
            finish();
        });

        reportClient->setUrl("ws://192.168.0.27:9001/updateReports?agent=libwsc");
        reportClient->connect();

        // Wait for the report endpoint to close, or time out.
        {
            std::unique_lock<std::mutex> lk(mtx);
            wait_for_pred(
                cv, lk,
                std::chrono::seconds(5),
                [&]() { return done; }
            );
        }

        // Full shutdown once.
        reportClient->disconnect();
        reportClient.reset();
    }

    std::cout << "All tests + report complete.\n";
    return 0;
}