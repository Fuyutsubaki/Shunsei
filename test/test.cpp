//#include <gtest/gtest.h>
#include <shunsei/channel.hpp>
#include <shunsei/task.hpp>
#include <thread>

sei::Task<> send1(sei::Channel<int>&ch){
    for(int i=0;i<3;++i){
        co_await ch.send(i);
    }
}

sei::Task<> send2(sei::Channel<int>&ch, sei::Channel<int>&done_ch){
    co_await send1(ch);
    co_await send1(ch);
    co_await send1(ch);

    done_ch.close();
}

sei::Task<> recv(sei::Channel<int>&ch, sei::Channel<int>&done_ch, std::atomic<bool> & all_done, std::vector<int>&tmp){
    int  done = false;
    while(!done){
        co_await sei::select(
            done_ch.recv([&](auto){
                done = true;
            }),
            ch.recv([&](auto x){
                tmp.push_back(*x);
            })
        );
    }
    all_done.store(true);
}

//TEST(test_common, Ch)
int main()
{
    sei::Channel<int> ch(0);
    sei::Channel<int> done_ch(0);
    std::atomic<bool> all_done = false;
    std::vector<int> tmp;

    sei::Scheduler::spown_task(send2(ch, done_ch));
    sei::Scheduler::spown_task(recv(ch, done_ch, all_done, tmp));
    
    // ガバガバスレッドプール
    std::vector<std::thread> worker;
    for(int i=0;i<3;++i){
        std::thread th{[&]{
           while(!all_done.load()){
               auto task = sei::Scheduler::dequeue_task();
               if(task){
                   task->execute();
               }else{
                   std::this_thread::yield();
               }
           }
        }};
        worker.emplace_back(std::move(th));
    }
    for(auto&th:worker){
        th.join();
    }
    assert(tmp == (std::vector{0,1,2,0,1,2,0,1,2})); // 本当に？
}
