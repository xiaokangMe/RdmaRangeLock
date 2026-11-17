#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
int main() {
    std::mutex mutex;std::condition_variable cv;bool ready=false,waiting=false;
    std::thread worker([&] {
        std::unique_lock<std::mutex> lock(mutex);waiting=true;cv.notify_all();
#ifdef USE_SYSTEM_CLOCK
        cv.wait_until(lock,std::chrono::system_clock::now()+std::chrono::seconds(2),[&]{return ready;});
#else
        cv.wait_until(lock,std::chrono::steady_clock::now()+std::chrono::seconds(2),[&]{return ready;});
#endif
    });
    {std::unique_lock<std::mutex> lock(mutex);cv.wait(lock,[&]{return waiting;});ready=true;cv.notify_all();}
    worker.join();
}
