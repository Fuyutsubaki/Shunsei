#pragma once
#include<deque>
#include<list>
#include<optional>
#include <experimental/coroutine>
#include<iostream>
#include<cassert>
#include<functional>
#include "task.hpp"
#include<vector>
#include<mutex>

namespace sei{

struct Scheduler{
    static inline std::deque<std::shared_ptr<Goroutine>> queue;
    static inline std::mutex mutex;
    static void spown_task(Task<>&&task){
        std::scoped_lock lock{mutex};
        auto g = std::make_shared<Goroutine>(std::move(task));
        queue.push_back(g);
    }
    static void push_g(std::shared_ptr<Goroutine> g, std::size_t wakeup_id){
        std::scoped_lock lock{mutex};
        g->wakeup_id = wakeup_id;
        queue.push_back(g);
    }
    static std::shared_ptr<Goroutine> dequeue_task(){
        std::scoped_lock lock{mutex};
        if(queue.empty()){
            return nullptr;
        }
        auto p = queue.front();
        queue.pop_front();
        return p;
    }
};

namespace detail{
    template<class value_type>
    struct Sudog{
        value_type *val;
        std::shared_ptr<Goroutine> task;
        std::size_t wakeup_id;
        bool is_select;
        using Iter = typename std::list<Sudog>::iterator;
    };

    template<class Sudog>
    static std::optional<Sudog> dequeueSudog(std::list<Sudog>&queue){
        for(auto it = queue.begin();it != queue.end();++it){
            if(it->is_select){
                bool expected = false;
                bool r = it->task->on_select_detect_wait.compare_exchange_strong(expected, true); //TODO
                if(r){
                    return *it; // selectの場合、要素の開放はselectの解放処理に任せる
                }
            }else{
                auto x = *it;
                queue.erase(it);
                return x;
            }
        }
        return {};
    }

    template<class Sudog>
    static typename Sudog::Iter enqueueSudog(std::list<Sudog>&queue, Sudog sdg){
        queue.push_back(sdg);
        auto it = queue.end();
        it--;
        return it;
    }
    template<std::size_t ...I, class F>
    void integral_constant_each(std::index_sequence<I...>,F f){
        int v[] = { (f(std::integral_constant<std::size_t,I>{}),0)...};
        (void)v;
    }
}

template<class value_type>
class Channel{
public:
    Channel(std::size_t limit):queue_limit(limit){}
    using SendSudog = detail::Sudog<value_type>;
    using RecvSudog = detail::Sudog<std::optional<value_type>>;
    struct [[nodiscard]] SendAwaiter {
        Channel&ch;
        value_type val;
        std::function<void()> f;
        typename SendSudog::Iter it = {};
        bool await_ready() const noexcept { return false; } // 何回もlock取得するわけにいかないので常にawait_suspendで処理する
        bool await_suspend(std::experimental::coroutine_handle<>) {
            std::scoped_lock lock{ch.mutex};
            auto task = current_goroutine;
            bool r = nonblock_send();
            if(r)return false;
            enqueueSudog(ch.send_queue, SendSudog{&val,task,0,false});
            return true;
        }

        void await_resume() const noexcept {}

        // select用
        bool try_nonblock_exec(){
            return nonblock_send();
        }
        void block_exec(std::size_t wakeup_id){
            auto task = current_goroutine;
            it = enqueueSudog(ch.send_queue, SendSudog{&val,task,wakeup_id,true});
        }
        void release_sudog(){
            ch.send_queue.erase(it);
        }
        void invoke_callback(){
            f();
        }

        bool nonblock_send()
        {
            if(ch.closed){
                assert(false);
            }
            else if(auto opt = dequeueSudog(ch.recv_queue)){
                auto sdg = *opt;
                *sdg.val = std::move(val);
                Scheduler::push_g(sdg.task, sdg.wakeup_id);
                return true;
            }
            else if(ch.value_queue.size() < ch.queue_limit){
                ch.value_queue.push_back(std::move(val));
                return true;
            }
            return false;
        }
    };

    struct [[nodiscard]] RecvAwaiter {
        Channel&ch;
        std::optional<value_type> val;
        std::function<void(std::optional<value_type>)> f;
        typename RecvSudog::Iter it = {};
        bool await_ready() const noexcept { return false; } // 何回もlock取得するわけにいかないので常にawait_suspendで処理する
        bool await_suspend(std::experimental::coroutine_handle<>) {
            std::scoped_lock lock{ch.mutex};
            auto task = current_goroutine;
            bool r = nonblock_recv();
            if(r)return false;
            enqueueSudog(ch.recv_queue, RecvSudog{&val,task,0,false});
            return true;
        }
        auto await_resume() const noexcept {return val; }

        bool try_nonblock_exec(){
            return nonblock_recv();
        }
        void block_exec(std::size_t wakeup_id){
            auto task = current_goroutine;
            it = enqueueSudog(ch.recv_queue, RecvSudog{&val,task,wakeup_id ,true});
        }
        void release_sudog(){
            ch.recv_queue.erase(it);
        }
        void invoke_callback(){
            f(val);
        }

        bool nonblock_recv(){
            if(ch.value_queue.size() > 0){
                val = ch.value_queue.front();
                ch.value_queue.pop_front();
                if(auto opt = dequeueSudog(ch.send_queue)){
                    auto sdg = *opt;
                    ch.value_queue.push_back(*sdg.val);
                    Scheduler::push_g(sdg.task, sdg.wakeup_id);
                }
                return true;
            }
            else if(ch.closed){
                val = std::nullopt;
                return true;
            }
            else if(auto opt = dequeueSudog(ch.send_queue)){
                auto sdg = *opt;
                val = std::move(*sdg.val);
                Scheduler::push_g(sdg.task, sdg.wakeup_id);
                return true;
            }
            return false;
        }
    };

    template<class T>
    SendAwaiter send(T && val, std::function<void()> f = [](auto...){}){
        return SendAwaiter{*this, std::forward<T>(val), f};
    }

    RecvAwaiter recv(std::function<void(std::optional<value_type>)> f = [](auto...){}){
        return RecvAwaiter{*this, std::nullopt, f};
    }

    void close(){ 
        std::scoped_lock lock{mutex};
        closed = true;
        while(auto opt = dequeueSudog(send_queue)){
            assert(false);
        }
        while(auto opt = dequeueSudog(recv_queue)){
            auto sdg = *opt;
            *sdg.val = std::nullopt;
            Scheduler::push_g(sdg.task, sdg.wakeup_id);
        }
    }

    std::mutex mutex;
    std::size_t queue_limit;
    std::deque<value_type> value_queue;

    std::list<RecvSudog> recv_queue;
    std::list<SendSudog> send_queue;
    bool closed = false;

};

template<class...T>
struct [[nodiscard]] SelectAwaiter{
    std::tuple<T...> sc_list;
    static constexpr std::size_t N = sizeof...(T);

    bool require_pass3 = false;
    std::size_t wakeup_id = 0;
    bool await_ready() const noexcept { return false; } // 何回もlock取得するわけにいかないので常にawait_suspendで処理する
    bool await_suspend(std::experimental::coroutine_handle<>) noexcept {
        auto lock = std::apply([](auto&...t){return std::scoped_lock{t.ch.mutex...};}, sc_list);
        bool r = false;
        detail::integral_constant_each(
            std::make_index_sequence<N>{}, 
            [&](auto I){
                if(!r){
                    r = std::get<I()>(sc_list).try_nonblock_exec();
                    if(r){
                        wakeup_id = I();
                    }
                }
            }
        );
        if(r)return false;
        require_pass3 = true;
        current_goroutine->on_select_detect_wait.store(false); // 初期化
        detail::integral_constant_each(std::make_index_sequence<N>{},[&](auto I){
            std::get<I()>(sc_list).block_exec(I());
            }
        );
        return true;
    }
    void await_resume() noexcept {
        // pass 3
        if(require_pass3){
            auto lock = std::apply([](auto&...t){return std::scoped_lock{t.ch.mutex...};}, sc_list);
            wakeup_id = current_goroutine->wakeup_id;
            detail::integral_constant_each(std::make_index_sequence<N>{},[&](auto I){
                std::get<I()>(sc_list).release_sudog();
                }
            );
        }
        detail::integral_constant_each(std::make_index_sequence<N>{},[&](auto I){
                if(I()==wakeup_id){
                    std::get<I()>(sc_list).invoke_callback();
                }
            }
        );
    }
};

template<class...T>
SelectAwaiter<T...> select(T...select_case){
    return {{select_case...}};
}


}