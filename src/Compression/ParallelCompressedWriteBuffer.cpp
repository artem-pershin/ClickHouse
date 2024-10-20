#include <city.h>
#include <cstring>

#include <base/types.h>
#include <base/defines.h>

#include <IO/WriteHelpers.h>
#include <Common/setThreadName.h>
#include <Common/scope_guard_safe.h>
#include <Common/CurrentThread.h>
#include <Common/CurrentMetrics.h>

#include <Compression/ParallelCompressedWriteBuffer.h>


namespace CurrentMetrics
{
    extern const Metric ParallelCompressedWriteBufferThreads;
    extern const Metric ParallelCompressedWriteBufferWait;
}

namespace DB
{

ParallelCompressedWriteBuffer::ParallelCompressedWriteBuffer(
    WriteBuffer & out_,
    CompressionCodecPtr codec_,
    size_t buf_size_,
    size_t num_threads_,
    ThreadPool & pool_)
    : WriteBuffer(nullptr, 0), out(out_), codec(codec_), buf_size(buf_size_), num_threads(num_threads_), pool(pool_)
{
    buffers.emplace_back(buf_size);
    current_buffer = buffers.begin();
    BufferBase::set(current_buffer->uncompressed.data(), buf_size, 0);
}

void ParallelCompressedWriteBuffer::nextImpl()
{
    if (!offset())
        return;

    std::unique_lock lock(mutex);

    /// The buffer will be compressed and processed in the thread.
    current_buffer->busy = true;
    pool.trySchedule([this, my_current_buffer = current_buffer, thread_group = CurrentThread::getGroup()]
    {
        SCOPE_EXIT_SAFE(
            if (thread_group)
                CurrentThread::detachFromGroupIfNotDetached();
        );

        if (thread_group)
            CurrentThread::attachToGroupIfDetached(thread_group);
        setThreadName("ParallelCompres");

        compress(my_current_buffer);
    });

    const BufferPair * previous_buffer = &*current_buffer;
    ++current_buffer;
    if (current_buffer == buffers.end())
    {
        if (buffers.size() < num_threads)
        {
            /// If we didn't use all num_threads buffers yet, create a new one.
            current_buffer = buffers.emplace(current_buffer, buf_size);
        }
        else
        {
            /// Otherwise, wrap around to the first buffer in the list.
            current_buffer = buffers.begin();
        }
    }

    /// Wait while the buffer becomes not busy
    {
        CurrentMetrics::Increment metric_increment(CurrentMetrics::ParallelCompressedWriteBufferWait);
        cond.wait(lock, [&]{ return !current_buffer->busy; });
    }

    /// Now this buffer can be used.
    current_buffer->previous = previous_buffer;
    BufferBase::set(current_buffer->uncompressed.data(), buf_size, 0);
}

void ParallelCompressedWriteBuffer::compress(Iterator buffer)
{
    CurrentMetrics::Increment metric_increment(CurrentMetrics::ParallelCompressedWriteBufferThreads);

    chassert(offset() <= INT_MAX);
    UInt32 decompressed_size = static_cast<UInt32>(offset());
    UInt32 compressed_reserve_size = codec->getCompressedReserveSize(decompressed_size);

    buffer->compressed.resize(compressed_reserve_size);
    UInt32 compressed_size = codec->compress(working_buffer.begin(), decompressed_size, buffer->compressed.data());

    CityHash_v1_0_2::uint128 checksum = CityHash_v1_0_2::CityHash128(buffer->compressed.data(), compressed_size);

    /// Wait while all previous buffers have been written.
    {
        CurrentMetrics::Increment metric_wait_increment(CurrentMetrics::ParallelCompressedWriteBufferWait);
        std::unique_lock lock(mutex);
        cond.wait(lock, [&]{ return !buffer->previous || !buffer->previous->busy; });
    }

    writeBinaryLittleEndian(checksum.low64, out);
    writeBinaryLittleEndian(checksum.high64, out);

    out.write(buffer->compressed.data(), compressed_size);

    std::unique_lock lock(mutex);
    buffer->busy = false;
    cond.notify_all();
}

}
