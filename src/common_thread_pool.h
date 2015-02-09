/**
 * @file
 */

#ifndef SPEAD_COMMON_THREAD_POOL_H
#define SPEAD_COMMON_THREAD_POOL_H

#include <type_traits>
#include <future>
#include <vector>
#include <array>
#include <cstdint>
#include <memory>
#include <boost/asio.hpp>
#include "recv_reader.h"

namespace spead
{

/**
 * Combination of a @c boost::asio::io_service with a set of threads to handle
 * the callbacks. The threads are created by the constructor and shut down
 * and joined in the destructor.
 */
class thread_pool
{
private:
    boost::asio::io_service io_service;
    /// Prevents the io_service terminating automatically
    boost::asio::io_service::work work;
    /**
     * Futures that becomes ready when a worker thread completes. It
     * is connected to an async task.
     */
    std::vector<std::future<void> > workers;

public:
    explicit thread_pool(int num_threads = 1);
    ~thread_pool();

    /// Retrieve the embedded io_service
    boost::asio::io_service &get_io_service() { return io_service; }
};

} // namespace spead

#endif // SPEAD_COMMON_THREAD_POOL_H
