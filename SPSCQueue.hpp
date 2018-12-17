#include <atomic>
#include <array>
#include <thread>
#include <vector>
#include <mutex>

namespace Utils::Concurrent
{

  template<typename T>
  using Padding128 = std::array<char, 128 - sizeof(T)>;

  template <typename T, size_t RingBufSize>
  class SPSCQueue
  {
  private:
    using AtomicIndex = std::atomic<size_t>;
    using Buffer     = std::array<T, RingBufSize>;
    static const constexpr size_t ElementSize = sizeof(T);

  public:

    SPSCQueue()
    {
      readIdx_.store(0, std::memory_order_relaxed);
      writeIdx_.store(0, std::memory_order_relaxed);
    }

    ~SPSCQueue()
    {
      T unused;
      while(try_pop(unused));
    }

    template<typename U>
    bool try_push (U&& newEl)
    {
      auto writePos = writeIdx_.load(std::memory_order_relaxed);
      auto nextWritePos = (writePos + 1) % RingBufSize;
      auto readPos = readIdx_.load(std::memory_order_acquire);

      if(nextWritePos == readPos)
      {
        return false;
      }

      buffer_[writePos] = std::forward<U>(newEl);
      writeIdx_.store( nextWritePos, std::memory_order_release);
      return true;
    }

    template<typename U>
    void push(U&& newEl)
    {
      while (!try_push(std::forward<U>(newEl)))
      {
        std::this_thread::yield();
      }
    }

    bool try_pop (T& newEl)
    {
      auto writePos = writeIdx_.load(std::memory_order_acquire);
      auto readPos = readIdx_.load(std::memory_order_relaxed);

      if(readPos == writePos)
      {
        return false;
      }

      newEl = std::move(buffer_[readPos]);
      readIdx_.store( (readPos + 1 ) % RingBufSize, std::memory_order_release);
    }

  private:
    Buffer buffer_;
    Padding128<AtomicIndex> padding0_;
    AtomicIndex readIdx_;
    Padding128<AtomicIndex> padding1_;
    AtomicIndex writeIdx_;
    Padding128<AtomicIndex> padding2_;
  };

   template < typename T,
              size_t RingBufSize,
              typename SPSCQueueType = SPSCQueue<T,RingBufSize>
             ,size_t MaxNumberOfQueues = 48>
   class MPSC_CombinedQueue
   {
   public:
     using SPSCQueues = std::vector<std::unique_ptr<SPSCQueueType>>;

     MPSC_CombinedQueue()
     {
       queues_.reserve(MaxNumberOfQueues);
       fairReadIdx_ = 0;
     }

     SPSCQueueType& reserveQueue()
     {
       std::unique_lock guard(reserveQueueSync_);

       queues_.push_back(std::make_unique<SPSCQueueType> ());
       return *(*queues_.rbegin());
     }

     bool try_pop(T& t)
     {
       for( size_t i = 1 ; i <= queues_.size(); ++i )
       {
         fairReadIdx_ = (fairReadIdx_ + i)  % queues_.size();
         if((*queues_[fairReadIdx_]).try_pop(t))
         {
           return true;
         }
       }
       return false;
     }

  private:
     SPSCQueues queues_;
     size_t fairReadIdx_;
     std::mutex reserveQueueSync_;
   };


   template<typename T
           , size_t RingBufSize
           , size_t MaxNumberOfBuffers = 48
           >
   class MPSC_VyukhovQueue
   {
   public:
     struct Node
     {
       Node()
       {
         next_.store(nullptr, std::memory_order_relaxed);
       }
       void releaseNode()
       {
         auto  cached = releasedElementsCounter_->load(std::memory_order_relaxed);
         releasedElementsCounter_->store(cached + 1, std::memory_order_release);
       }
     public:
       std::atomic<size_t>* releasedElementsCounter_;
       std::atomic<Node*> next_;
       T data_;
     };

     struct NodeBuffer
     {
       NodeBuffer()
       {
          numReleased_.store(0, std::memory_order_relaxed);
          numAcquired_.store(0, std::memory_order_relaxed);

          for (Node& node : nodeBuffer_)
          {
            node.releasedElementsCounter_ = &numReleased_;
          }
       }

      Node* tryAcquireNode()
      {
         auto numReleasedCached = numReleased_.load(std::memory_order_acquire);
         auto numAcquiredCached = numAcquired_.load(std::memory_order_relaxed);

         if (numAcquiredCached - numReleasedCached >= RingBufSize)
         {
           return nullptr;
         }

         Node* toRet = &nodeBuffer_[numAcquiredCached % RingBufSize];
         toRet->next_.store(nullptr,std::memory_order_relaxed);
         numAcquired_.store(numAcquiredCached + 1, std::memory_order_release);
         return toRet;
      }

       std::atomic<size_t> numAcquired_;
       Padding128<std::atomic<size_t>> padding0_;
       std::atomic<size_t> numReleased_;
       Padding128<std::atomic<size_t>> padding1_;
       std::array<Node, RingBufSize> nodeBuffer_;
     };

     using NodeBuffers = std::vector<std::unique_ptr<NodeBuffer> >;

     MPSC_VyukhovQueue()
     {
       dummyCounter_.store(0, std::memory_order_relaxed);
       dummy_.releasedElementsCounter_ = &dummyCounter_;
       dummy_.next_ = nullptr;
       head_ = tail_ = &dummy_;
     }

     template<typename U>
     bool try_push(U&& newEl, NodeBuffer& buffer)
     {
       auto* node = buffer.tryAcquireNode();
       if (node)
       {
         node->data_ = std::forward<U>(newEl);
         pushNode(node);
         return true;
       }
       return false;
     }

     template<typename U>
     void push(U&& newEl, NodeBuffer& buffer)
     {
       while (!try_push(std::forward<U>(newEl), buffer))
       {
         std::this_thread::yield();
       }
     }

     void pushNode(Node* node)
     {
        node->next_.store(nullptr, std::memory_order_relaxed);
        auto* prev = tail_.exchange(node,std::memory_order_acq_rel);
        prev->next_.store(node, std::memory_order_release);
     }

     bool try_pop(T& t)
     {
       auto* cachedHead = head_.load(std::memory_order_relaxed);
       Node* next = cachedHead->next_.load(std::memory_order_acquire);

       if (next == nullptr)
       {
         return false;
       }
       t = std::move(next->data_);
       head_.store(next, std::memory_order_release);
       cachedHead->releaseNode();
       return true;
     }

     NodeBuffer& reservePool()
     {
       std::unique_lock guard(reservePoolSync_);
       auto newBuffer = std::make_unique<NodeBuffer>();
       pools_.push_back(std::move(newBuffer));
       return *(*pools_.rbegin());
     }

   private:
     std::mutex         reservePoolSync_;
     NodeBuffers        pools_;
     Node               dummy_;
     std::atomic<size_t> dummyCounter_;
     std::atomic<Node*> head_;
     std::atomic<Node*> tail_;
   };
}
