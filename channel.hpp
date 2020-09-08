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



class Channel{
public:
    struct Sudog{
        using value_type = int;
        value_type & val;
        std::shared_ptr<Goroutine> task;
        std::size_t wakeup_id;
        bool is_select;
    };
    using value_type = int;
    using sudog_iter = std::list<Sudog>::iterator;
    Channel(std::size_t limit):queue_limit(limit){}

    struct SendAwaiter {
        Channel&ch;
        value_type val;
        bool await_ready() const noexcept { return false; } // 何回もlock取得するわけにいかないので常にawait_suspendで処理する
        bool await_suspend(std::experimental::coroutine_handle<>) {
            std::scoped_lock lock{ch.mutex};
            auto task = current_goroutine;
            bool r = ch.nonblock_send(val);
            if(r)return false;
            ch.block_send(task,val,0 ,false);
            return true;
        }
        void await_resume() const noexcept {}

        // select用
        
        struct SelectCase{
            Channel&ch;
            value_type val;
            std::function<void()> f;
            sudog_iter it;

            bool try_nonblock_exec(){
                return ch.nonblock_send(val);
            }
            void block_exec(std::size_t wakeup_id){
                auto task = current_goroutine;
                it = ch.block_send(task,val,wakeup_id ,true);
            }
            void release_sudog(){
                ch.send_queue.erase(it);
            }
            void invoke_callback(){
                f();
            }
        };
        SelectCase to_select(std::function<void()> f){
            return {ch,val,f,{}};
        }
    };
    struct RecvAwaiter {
        Channel&ch;
        value_type val;
        bool await_ready() const noexcept { return false; } // 何回もlock取得するわけにいかないので常にawait_suspendで処理する
        bool await_suspend(std::experimental::coroutine_handle<>) {
            std::scoped_lock lock{ch.mutex};
            auto task = current_goroutine;
            bool r = ch.nonblock_recv(val);
            if(r)return false;
            ch.block_recv(task,val,0 ,false);
            return true;
        }
        auto await_resume() const noexcept {return val; }
        struct SelectCase{
            Channel&ch;
            value_type val;
            std::function<void(value_type)> f;
            sudog_iter it;

            bool try_nonblock_exec(){
                return ch.nonblock_recv(val);
            }
            void block_exec(std::size_t wakeup_id){
                auto task = current_goroutine;
                it = ch.block_recv(task,val,wakeup_id ,true);
            }
            void release_sudog(){
                ch.recv_queue.erase(it);
            }
            void invoke_callback(){
                f(val);
            }
        };
        SelectCase to_select(std::function<void(value_type)> f){
            return {ch,val,f,{}};
        }
    };

    SendAwaiter send(value_type val){
        return SendAwaiter{*this, val};
    }
    RecvAwaiter recv(){
        return RecvAwaiter{*this, 0};
    }

    void close(){ 
        //C++的にはこの関数は存在自体に疑問がある
        std::scoped_lock lock{mutex};
        closed = true;
        while(auto opt = dequeueSudog(send_queue)){
                assert(false);
        }
        while(auto opt = dequeueSudog(recv_queue)){
            auto sdg = *opt;
            sdg.val = 0;
            Scheduler::push_g(sdg.task, sdg.wakeup_id);
        }
    }

//private:
    std::mutex mutex;
    std::size_t queue_limit;
    std::deque<value_type> value_queue;

    std::list<Sudog> recv_queue;
    std::list<Sudog> send_queue;
    bool closed;
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
                return *it;
            }
        }
        return {};
    }

    bool nonblock_send(value_type &val){
        if(closed){
            assert(false);
        }
        else if(auto opt = dequeueSudog(recv_queue)){
            auto sdg = *opt;
            sdg.val = val;
            Scheduler::push_g(sdg.task, sdg.wakeup_id);
            return true;
        }
        else if(value_queue.size() < queue_limit){
            value_queue.push_back(val);
            return true;
        }
        
        return false;
    }
    sudog_iter block_send(std::shared_ptr<Goroutine> task, value_type &val, std::size_t wakeup_id, bool is_select){
        send_queue.push_back(Sudog{val,task,wakeup_id,is_select});
        auto it = send_queue.end();
        it--;
        return it;
    }

    bool nonblock_recv(value_type &val){
        if(value_queue.size() > 0){
            val = value_queue.front();
            value_queue.pop_front();
            if(auto opt = dequeueSudog(send_queue)){
                auto sdg = *opt;
                value_queue.push_back(sdg.val);
                Scheduler::push_g(sdg.task, sdg.wakeup_id);
            }
            return true;
        }
        else if(closed){
            val = 0;
            return true;
        }
        else if(auto opt = dequeueSudog(send_queue)){
            auto sdg = *opt;
            val = sdg.val;
            Scheduler::push_g(sdg.task, sdg.wakeup_id);
            return true;
        }
        return false;
    }
    sudog_iter block_recv(std::shared_ptr<Goroutine> task, value_type &val, std::size_t wakeup_id, bool is_select){
        recv_queue.push_back(Sudog{val,task,wakeup_id,is_select});
        auto it = recv_queue.end();
        it--;
        return it;
    }
    void release_recv( std::list<Sudog>::iterator it){
        recv_queue.erase(it);
    }
};
namespace detail{
    template<std::size_t ...I, class F>
    void integral_constant_each(std::index_sequence<I...>,F f){
        int v[] = { (f(std::integral_constant<std::size_t,I>{}),0)...};
        (void)v;
    }
}


template<class...T>
struct SelectAwaiter{
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
                    if(r)
                    {
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