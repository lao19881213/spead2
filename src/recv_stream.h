/**
 * @file
 */

#ifndef SPEAD_RECV_STREAM
#define SPEAD_RECV_STREAM

#include <cstddef>
#include <deque>
#include <memory>
#include <boost/asio.hpp>
#include "recv_heap.h"
#include "recv_reader.h"
#include "common_mempool.h"

namespace spead
{

class thread_pool;

namespace recv
{

class packet_header;

/**
 * Encapsulation of a SPEAD stream. Packets are fed in through @ref add_packet.
 * The base class does nothing with heaps; subclasses will typically override
 * @ref heap_ready and @ref stop to do further processing.
 *
 * A collection of partial heaps is kept. Heaps are removed from this collection
 * and passed to @ref heap_ready when
 * - They are known to be complete (a heap length header is present and all the
 *   corresponding payload has been received); or
 * - Too many heaps are live: the one with the lowest ID is aged out, even if
 *   incomplete
 * - The stream is stopped
 *
 * This class is @em not thread-safe. Almost all use cases (possibly excluding
 * testing) will derive from @ref stream.
 */
class stream_base
{
private:
    /**
     * Maximum number of live heaps permitted. Temporarily one more might be
     * present immediate prior to one being ejected.
     */
    std::size_t max_heaps;
    /// Live heaps, ordered by heap ID
    std::deque<heap> heaps;
    /// @ref stop has been called, either externally or by stream control
    bool stopped = false;
    /// Protocol bugs to be compatible with
    bug_compat_mask bug_compat;
    /// Memory pool used by heaps
    std::shared_ptr<mempool> pool;

    /**
     * Callback called when a heap is being ejected from the live list.
     * The heap might or might not be complete.
     */
    virtual void heap_ready(heap &&) {}

public:
    /**
     * Constructor.
     *
     * @param bug_compat   Protocol bugs to have compatibility with
     * @param max_heaps    Maximum number of live (in-flight) heaps held in the stream
     */
    explicit stream_base(bug_compat_mask bug_compat = 0, std::size_t max_heaps = 4);
    virtual ~stream_base() = default;

    /**
     * Change the maximum heap count. This will not immediately cause heaps to
     * be ejected if over the limit, but will prevent any increase until the
     * number if back under the limit.
     */
    void set_max_heaps(std::size_t max_heaps);

    /**
     * Set a pool to use for allocating heap memory.
     */
    void set_mempool(std::shared_ptr<mempool> pool);

    /**
     * Add a packet that was received, and which has been examined by @a
     * decode_packet, and returns @c true if it is consumed. Even though @a
     * decode_packet does some basic sanity-checking, it may still be rejected
     * by @ref heap::add_packet e.g., because it is a duplicate.
     *
     * It is an error to call this after the stream has been stopped.
     */
    bool add_packet(const packet_header &packet);
    /**
     * Shut down the stream. This calls @ref flush.  Subclasses may override
     * this to achieve additional effects, but must chain to the base
     * implementation.
     *
     * It is undefined what happens if @ref add_packet is called after a stream
     * is stopped.
     *
     * @todo Record whether the stream has been stopped and give a well-defined
     * error; also detect packets that request stop.
     */
    virtual void stop();

    bool is_stopped() const { return stopped; }

    bug_compat_mask get_bug_compat() const { return bug_compat; }

    /// Flush the collection of live heaps, passing them to @ref heap_ready.
    void flush();
};

class stream : public stream_base
{
private:
    /**
     * Serialization of access.
     */
    boost::asio::io_service::strand strand;
    /**
     * Readers providing the stream data.
     */
    std::vector<std::unique_ptr<reader> > readers;

public:
    boost::asio::io_service::strand &get_strand() { return strand; }

    // TODO: introduce constant for default max_heaps
    explicit stream(boost::asio::io_service &service, bug_compat_mask bug_compat = 0, std::size_t max_heaps = 4);
    explicit stream(thread_pool &pool, bug_compat_mask bug_compat = 0, std::size_t max_heaps = 4);
    ~stream() override;

    /**
     * Add a new reader by passing its constructor arguments, excluding
     * the initial @a stream argument.
     */
    template<typename T, typename... Args>
    void emplace_reader(Args&&... args)
    {
        reader *r = new T(*this, std::forward<Args>(args)...);
        std::unique_ptr<reader> ptr(r);
        readers.push_back(std::move(ptr));
        r->start();
    }

    virtual void stop() override;
};

/**
 * Push packets found in a block of memory to a stream. Returns a pointer to
 * after the last packet found in the stream. Processing stops as soon as
 * after @ref decode_packet fails (because there is no way to find the next
 * packet after a corrupt one), but packets may still be rejected by the stream.
 *
 * The stream is @em not stopped.
 */
const std::uint8_t *mem_to_stream(stream_base &s, const std::uint8_t *ptr, std::size_t length);

} // namespace recv
} // namespace spead

#endif // SPEAD_RECV_STREAM
