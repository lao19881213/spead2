/**
 * @file
 */

#include <cstddef>
#include <utility>
#include <cassert>
#include "recv_stream.h"
#include "recv_heap.h"
#include "common_thread_pool.h"

namespace spead
{
namespace recv
{

stream_base::stream_base(bug_compat_mask bug_compat, std::size_t max_heaps)
    : max_heaps(max_heaps), bug_compat(bug_compat)
{
}

void stream_base::set_max_heaps(std::size_t max_heaps)
{
    this->max_heaps = max_heaps;
}

void stream_base::set_mempool(std::shared_ptr<mempool> pool)
{
    this->pool = std::move(pool);
}

bool stream_base::add_packet(const packet_header &packet)
{
    assert(!stopped);
    // Look for matching heap
    auto insert_before = heaps.begin();
    bool result = false;
    bool end_of_stream = false;
    bool found = false;
    for (auto it = heaps.begin(); it != heaps.end(); ++it)
    {
        heap &h = *it;
        if (h.cnt() == packet.heap_cnt)
        {
            found = true;
            if (h.add_packet(packet))
            {
                result = true;
                end_of_stream = h.is_end_of_stream();
                if (h.is_complete())
                {
                    heap_ready(std::move(h));
                    heaps.erase(it);
                }
            }
            break;
        }
        else if (h.cnt() < packet.heap_cnt)
            insert_before = next(it);
    }

    if (!found)
    {
        // Doesn't match any previously seen heap, so create a new one
        heap h(packet.heap_cnt, bug_compat);
        h.set_mempool(pool);
        if (h.add_packet(packet))
        {
            result = true;
            end_of_stream = h.is_end_of_stream();
            if (h.is_complete())
            {
                heap_ready(std::move(h));
            }
            else
            {
                heaps.insert(insert_before, std::move(h));
                if (heaps.size() > max_heaps)
                {
                    // Too many active heaps: pop the lowest ID, even if incomplete
                    heap_ready(std::move(heaps[0]));
                    heaps.pop_front();
                }
            }
        }
    }
    if (end_of_stream)
        stop();
    return result;
}

void stream_base::flush()
{
    for (heap &h : heaps)
    {
        heap_ready(std::move(h));
    }
    heaps.clear();
}

void stream_base::stop()
{
    stopped = true;
    flush();
}


stream::stream(boost::asio::io_service &io_service, bug_compat_mask bug_compat, std::size_t max_heaps)
    : stream_base(bug_compat, max_heaps), strand(io_service)
{
}

stream::stream(thread_pool &thread_pool, bug_compat_mask bug_compat, std::size_t max_heaps)
    : stream(thread_pool.get_io_service(), bug_compat, max_heaps)
{
}

void stream::stop()
{
    /* This can be called either by the user or as a result of reader action,
     * so it needs to be serialised. We also need to use @c post rather than
     * @c dispatch, because we may be in the middle of executing code in one
     * of the readers and we do not want to re-enter the reader in stopping
     * it.
     *
     * The promise and future are used to block until the callback finishes.
     */
    std::promise<void> promise;
    std::future<void> future = promise.get_future();
    get_strand().post([this, &promise] ()
    {
        while (!readers.empty())
        {
            readers.back()->stop();
            readers.pop_back();
        }
        stream_base::stop();
        promise.set_value();
    });
    // Wait for the callback
    future.get();
}

stream::~stream()
{
    stream::stop();
}


const std::uint8_t *mem_to_stream(stream_base &s, const std::uint8_t *ptr, std::size_t length)
{
    while (length > 0 && !s.is_stopped())
    {
        packet_header packet;
        std::size_t size = decode_packet(packet, ptr, length);
        if (size > 0)
        {
            s.add_packet(packet);
            ptr += size;
            length -= size;
        }
        else
            length = 0; // causes loop to exit
    }
    return ptr;
}

} // namespace recv
} // namespace spead
