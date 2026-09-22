#include "utils/pipeline_metrics.h"
#include "utils/CounterRate.h"
#include <cassert>
#include <thread>
#include <chrono>
#include <cstdio>
int main()
{
    uint64_t values[PIPE_METRIC_COUNT];
    pipeline_metrics_read(values);
    for(auto count : values) assert(count==0);
    auto writer=[] {for(int i=0;i<100000;++i)pipeline_metric_hit(PIPE_CHAIN+3);};
    std::thread a(writer),b(writer);a.join();b.join();
    pipeline_metrics_read(values);assert(values[PIPE_CHAIN+3]==200000);
    for(int i=0;i<PIPE_METRIC_COUNT;++i)if(i!=PIPE_CHAIN+3)assert(values[i]==0);
    pipeline_metric_hit(PIPE_METRIC_COUNT); // invalid events cannot corrupt counters
    CounterRate rate;
    assert(rate.sample(5000,0)==0); // opening an editor does not count history
    assert(rate.sample(5525,.5)==1050);
    assert(rate.sample(6575,1)==1050); // delayed GUI tick uses actual duration
    assert(rate.sample(6575,.5)==0); // stopped pipeline must show zero
    assert(rate.sample(1,.5)==0); // restart, not a giant unsigned delta
    assert(rate.sample(6,.5)==10);
    CounterRate second;assert(second.sample(200000,0)==0); // independent reader
    auto start=std::chrono::steady_clock::now();
    for(int i=0;i<1000000;++i)pipeline_metric_hit(PIPE_RX);
    auto elapsed=std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-start).count();
    std::printf("PASS: concurrent counters, independent readers, idle/restart/delayed GUI; %.1f ns/event (this test build)\n",elapsed/1e6);
}
