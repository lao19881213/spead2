/**
 * @file
 */

#ifndef SPEAD_PY_COMMON_H
#define SPEAD_PY_COMMON_H

#include <memory>

/* Older versions of boost don't understand std::shared_ptr properly. This needs
 * to be defined before boost.python is included.
 */
#if BOOST_VERSION < 105300
namespace boost
{

template<typename T>
T *get_pointer(const std::shared_ptr<T> &p)
{
    return p.get();
}

} // namespace boost
#endif

#include <boost/python.hpp>
#include <boost/noncopyable.hpp>
#include <boost/version.hpp>
#include <cassert>
#include <mutex>
#include <stdexcept>
#include "common_ringbuffer.h"

namespace spead
{

class stop_iteration : public std::exception
{
public:
    using std::exception::exception;
};

/**
 * RAII wrapper that releases the Python Global Interpreter Lock on
 * construction and reacquires it on destruction. It is also possible to
 * freely acquire and release it during the lifetime; if it is released on
 * destruction, it is reacquired.
 *
 * One thread must @em not have two instances of this object.
 */
class release_gil
{
private:
    PyThreadState *save = nullptr;

public:
    release_gil()
    {
        release();
    }

    ~release_gil()
    {
        if (save != nullptr)
            PyEval_RestoreThread(save);
    }

    void release()
    {
        assert(save == nullptr);
        save = PyEval_SaveThread();
    }

    void acquire()
    {
        assert(save != nullptr);
        PyEval_RestoreThread(save);
        save = nullptr;
    }
};

/**
 * RAII class to acquire the GIL in a non-Python thread.
 */
class acquire_gil
{
private:
    PyGILState_STATE gstate;
public:
    acquire_gil()
    {
        gstate = PyGILState_Ensure();
    }

    ~acquire_gil()
    {
        PyGILState_Release(gstate);
    }
};

/**
 * Wraps access to a Python buffer-protocol object. On construction, it
 * fetches the buffer, and on destruction it releases it. At present, only
 * @c PyBUF_SIMPLE is supported, but it could easily be extended.
 */
class buffer_view : public boost::noncopyable
{
public:
    Py_buffer view;

    explicit buffer_view(boost::python::object obj)
    {
        if (PyObject_GetBuffer(obj.ptr(), &view, PyBUF_SIMPLE) != 0)
            boost::python::throw_error_already_set();
    }

    ~buffer_view()
    {
        PyBuffer_Release(&view);
    }
};

/**
 * Ringbuffer variant that releases the GIL while waiting for data, and aborts
 * if there was a @c KeyboardInterrupt.
 */
template<typename T>
class ringbuffer_fd_gil : public ringbuffer_fd<T>
{
public:
    using ringbuffer_fd<T>::ringbuffer_fd;

    T pop();
};

template<typename T>
T ringbuffer_fd_gil<T>::pop()
{
    int bytes;
    do
    {
        release_gil gil;
        bytes = this->try_read_byte();
        if (bytes == 0)
            throw ringbuffer_stopped();
        else if (bytes < 0)
        {
            // Allow SIGINT to abort the pop
            gil.acquire();
            if (PyErr_CheckSignals() == -1)
                boost::python::throw_error_already_set();
        }
    } while (bytes < 0);

    std::unique_lock<std::mutex> lock(this->mutex);
    assert(!this->empty_unlocked());
    return this->pop_unlocked();
}

} // namespace spead

#endif // SPEAD_PY_COMMON_H
