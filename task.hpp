#pragma once
#include <experimental/coroutine>
#include<mutex>
#include<iostream>
namespace sei{

struct Goroutine;
thread_local inline std::shared_ptr<Goroutine> current_goroutine = nullptr;

template<class value_type = void>
struct Task {
    struct promise_type;
    using handle_type = std::experimental::coroutine_handle<promise_type>;
    struct FinalSuspend{
        std::experimental::coroutine_handle<> parent = nullptr;
        bool await_ready() const noexcept { return false; }
        void await_suspend(std::experimental::coroutine_handle<>)noexcept;
        void await_resume()noexcept{}
    };
    struct Awaiter{
        handle_type self;
        bool await_ready() const noexcept { return false; }
        void await_suspend(std::experimental::coroutine_handle<> )noexcept;
        value_type await_resume(){return self.promise().get(); }
    };

    template<class T>
    struct promise_type1{
        void return_value(value_type v) {val = v;}
        void unhandled_exception() { std::terminate(); }
        value_type val;
        auto get(){return val;}
    };
    template<>
    struct promise_type1<void>{
        void return_void() {}
        void unhandled_exception() { std::terminate(); }
        auto get(){}
    };
    struct promise_type : promise_type1<value_type>{
        promise_type(){}
        auto get_return_object(){ return Task{handle_type::from_promise(*this)}; }
        std::experimental::suspend_always initial_suspend() { return {}; }
        FinalSuspend final_suspend() noexcept{ return {parent}; }
        std::experimental::coroutine_handle<> parent = nullptr;
    };
    auto operator co_await(){
        return Awaiter{coro_};
    }

    explicit Task(handle_type c):coro_(c){}
    Task(Task const & src) = delete;
    Task(Task && src):coro_(src.coro_){
        src.coro_ = nullptr;
    }
    ~Task(){
        if(coro_){
            coro_.destroy();
        }
    }
    handle_type coro_;
};

struct Goroutine
    :std::enable_shared_from_this<Goroutine>
{
    Goroutine(Task<> && t)
        :task(std::move(t))
        
        {
            current_handle = task.coro_;
        }
    Task<> task;
    std::experimental::coroutine_handle<> current_handle;
    bool resume = false;
    std::size_t wakeup_id = 0;
    std::atomic<bool> on_select_detect_wait ;
    // mutex 何とかなくせないか。
    // たとえばgoroutineのpushやchのmutexの解除を https://github.com/golang/go/blob/5c2c6d3fbf4f0a1299b5e41463847d242eae19ca/src/runtime/proc.go#L306 みたいな方法にするとか
    std::mutex mutex;
    void execute(){
        std::scoped_lock lock{mutex};  
        resume = true;
        current_goroutine = shared_from_this();
        while(resume && current_handle){
            resume = false;
            current_handle.resume();
        }
    }
};

template<class value_type>
void Task<value_type>::FinalSuspend::await_suspend(std::experimental::coroutine_handle<>)noexcept{
    current_goroutine->current_handle = parent;
    current_goroutine->resume = true;
}
template<class value_type>
void Task<value_type>::Awaiter::await_suspend(std::experimental::coroutine_handle<> h)noexcept{
    self.promise().parent = h;
    current_goroutine->current_handle = self;
    current_goroutine->resume = true;
}

}
// もしかするとこっちのほうが速いかもしれないが文法がアレになってしまう
#define CFN_INLINE_SUBTASK(TASK, RETURN_VALUE_REF) do { auto && t = TASK; while(!t.coro_.done()){t.coro_.resume(); co_await std::experimental::suspend_always{};} RETURN_VALUE_REF = t.coro_.promise().val; }while(0)
