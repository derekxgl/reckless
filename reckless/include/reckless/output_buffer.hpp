#ifndef RECKLESS_OUTPUT_BUFFER_HPP
#define RECKLESS_OUTPUT_BUFFER_HPP

#include "detail/branch_hints.hpp"

#include <cstddef>  // size_t
#include <new>      // bad_alloc
#include <cstring>  // strlen, memcpy
#include <system_error> // system_error, error_code

namespace reckless {
    
enum class error_policy {
    ignore,
    notify_on_recovery,
    block,
    fail_immediately
};

// Thrown if output_buffer::reserve() is used to allocate more than can fit in
// the output buffer during formatting of a single input frame. If this happens
// then you need to either enlarge the output buffer or reduce the amount of
// data produced by your formatter.
class excessive_output_by_frame : public std::bad_alloc {
public:
    char const* what() const override;
};

// Thrown if output_buffer::flush fails. This inherits from bad_alloc because
// it makes sense in the context where the formatter calls
// output_buffer::reserve(), and a flush intended to create more space in the
// buffer fails. In that context, it is essentially an allocation error. The
// formatter may catch this if it wishes, but it is expected that most will
// just let it fall through to the worker thread, where it will be dealt with
// appropriately.
class flush_error : public std::bad_alloc {
public:
    flush_error(std::error_code const& error_code) :
        error_code_(error_code)
    {
    }
    char const* what() const override;
    std::error_code const& code() const
    {
        return error_code_;
    }

private:
    std::error_code error_code_;
};

// Thrown if output_buffer::flush fails and the error policy is
// fail_immediately. The formatter should not prevent this exception from
// propagating to the top of the call stack, as that will prevent the error
// from being reported to the caller immediately as requested by the error
// policy. Preventing it from propagating may also cause the log file to be
// corrupted, since half-written log entries remain in the output buffer.
class fatal_flush_error : public std::system_error {
public:
    fatal_flush_error(std::error_code const& error_code) :
        error_code_(error_code)
    {
    }
    char const* what() const override;
    std::error_code const& code() const
    {
        return error_code_;
    }

private:
    std::error_code error_code_;
}

using flush_error_callback_t = std::function<void (std::error_code ec, unsigned lost_record_count)>;

class output_buffer {
public:
    output_buffer();
    // TODO hide functions that are not relevant to the client, e.g. move
    // assignment, empty(), flush etc?
    output_buffer(writer* pwriter, std::size_t max_capacity);
    ~output_buffer();

    char* reserve(std::size_t size)
    {
        std::size_t remaining = pbuffer_end_ - pcommit_end_;
        if(detail::likely(size <= remaining))
            return pcommit_end_;
        else
            return reserve_slow_path(size);
    }

    void commit(std::size_t size)
    {
        pcommit_end_ += size;
    }
    
    void write(void const* buf, std::size_t count);
    
    void write(char const* s)
    {
        write(s, std::strlen(s));
    }

    void write(char c)
    {
        char* p = reserve(1);
        *p = c;
        commit(1);
    }
    
protected:
    void reset();
    // throw bad_alloc if unable to malloc() the buffer.
    void reset(writer* pwriter, std::size_t max_capacity);

    void frame_end()
    {
        pframe_end_ = pcommit_end_;
        ++input_frames_in_buffer_;
    }

    // Undo everything that has been written during the current input frame.
    void revert_frame()
    {
        pcommit_end_ = pframe_end_;
    }
    
    bool empty() const
    {
        return pcommit_end_ == pbuffer_;
    }

    void flush();
    
    NEXT add mutex etc, and introduce this function in basic_log through using statement.
    Then...? Compile?
    void flush_error_callback(flush_error_callback_t callback = flush_error_callback_t())
    {
        std::lock_guard<std::mutex> lk(callback_mutex_);
        flush_error_callback_ = move(callback);
    }
    
    spsc_event shared_input_queue_full_event_; // FIXME rename to something that indicates this is used for all "notifications" to the worker thread

private:
    output_buffer(output_buffer const&) = delete;
    output_buffer& operator=(output_buffer const&) = delete;

    char* reserve_slow_path(std::size_t size);

    writer* pwriter_;
    char* pbuffer_;
    char* pframe_end_;
    char* pcommit_end_;
    char* pbuffer_end_;
    unsigned input_frames_in_buffer_;
    unsigned lost_input_frames_;
};

}

#endif  // RECKLESS_OUTPUT_BUFFER_HPP
