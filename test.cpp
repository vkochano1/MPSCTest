
#include <iostream>
#include <SPSCQueue.hpp>
#include <Utils.hpp>

template<typename T, size_t Size>
class CombinedQueueAdapter : public Utils::Concurrent::MPSC_CombinedQueue<T, Size>
{
public:
  decltype(auto) createWriterContext()
  {
    return this->reserveQueue();
  }

  template<typename U, typename WriterQueue>
  void pushWithContext(U&& el, WriterQueue& wrQueue)
  {
    wrQueue.try_push(el);
  }
};

template<typename T, size_t Size>
class VyukhovQueueAdapter : public Utils::Concurrent::MPSC_VyukhovQueue<T, Size>
{
public:
  using Base = Utils::Concurrent::MPSC_VyukhovQueue<T, Size>;

  decltype(auto) createWriterContext()
  {
    return this->reservePool();
  }

  template<typename U, typename Pool>
  void pushWithContext(U&& el, Pool& pool)
  {
    Base::try_push(el, pool);
  }
};

template <typename QueueType>
class MPSC_QueueTest
{
public:
  MPSC_QueueTest(size_t recordsToReceive
               , size_t nrOfWriters
               , int publishDelayMS
               , int consumerDelayMS)
  {
    startWriters(nrOfWriters, publishDelayMS);
    std::this_thread::sleep_for(std::chrono::milliseconds(consumerDelayMS));
    startConsumer(recordsToReceive);
    stopAllWriters();
  }

private:
  void startWriters(size_t nrWriters, int publishDelayMS)
  {
    stoppers_.reserve(nrWriters);
    threads_.reserve(nrWriters);

    for (int i = 0; i < nrWriters; ++i )
    {
      auto& writerContext = queue_.createWriterContext();
      auto coreToPin = i + 1;
      stoppers_.push_back( std::make_unique<std::atomic<bool>>());
      auto& stopper = **stoppers_.rbegin();
      stopper = false;
      threads_.push_back
      (
        std::make_unique<std::thread>
        (
          [this, &writerContext, coreToPin, &stopper, publishDelayMS] ()
          {
            Utils::pinThreadToCore(coreToPin);
            uint64_t sum = 0;
            uint64_t count = 0;
            for (size_t x = 0; stopper.load() != true ;++x)
            {
                auto begin = Utils::rdtscp();
                this->queue_.pushWithContext(begin,writerContext);
                auto end = Utils::rdtscp();
                sum += end - begin;
                ++count;
                if(publishDelayMS > 0)
                {
                  std::this_thread::sleep_for(std::chrono::milliseconds(publishDelayMS));
                }
            }
            if (count)
              std::cout << "avgPushLatency:" << sum / count << std::endl;
          }
        )
      );
    }
  }

  void startConsumer(size_t maxElementsToProcess)
  {
    size_t numProcessed = 0;
    uint64_t sum = 0;
    uint64_t received;
    uint64_t receivedSum = 0;
    uint64_t receivedProcessed = 0;

    for (;numProcessed < maxElementsToProcess;)
    {
      auto begin = Utils::rdtscp();
      if(!queue_.try_pop(received))
      {
        continue;
      }
      auto end = Utils::rdtscp();
      ++numProcessed;
      sum += end - begin;
      if(end > received)
      {
        // need to use the same cpu socket
        ++receivedProcessed;
        receivedSum += end - received;
      }

    }
    std::cout << "AvgPopLatency: " << sum / maxElementsToProcess << std::endl;
    std::cout << "AvgQueueLatency: " << receivedSum / receivedProcessed << std::endl;

  }

  void stopAllWriters()
  {
    for (auto& stopper : stoppers_)
    {
      *stopper = true;
    }

    for (auto& thread : threads_)
    {
      thread->join();
    }
  }
protected:
  QueueType queue_;
  std::vector<std::unique_ptr<std::thread> > threads_;
  std::vector<std::unique_ptr<std::atomic<bool> > > stoppers_ ;
};


int main()
{
  //std::this_thread::sleep_for(std::chrono::seconds(1));
  {
    MPSC_QueueTest<VyukhovQueueAdapter<uint64_t, 1000> > test(10000,1,1, 1000);
  }

 //std::this_thread::sleep_for(std::chrono::seconds(1));
  {
    MPSC_QueueTest<CombinedQueueAdapter<uint64_t, 1000> > test(10000,1,1, 1000);
  }
  return  0;
}
