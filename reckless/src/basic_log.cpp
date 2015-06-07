#include <reckless/basic_log.hpp>

#include <vector>
#include <ciso646>

#include <unistd.h>     // sleep

namespace {
void destroy_thread_input_buffer(void* p)
{
    using reckless::detail::thread_input_buffer;
    thread_input_buffer* pbuffer = static_cast<thread_input_buffer*>(p);
    thread_input_buffer::destroy(pbuffer);
}
}

reckless::basic_log::basic_log() :
    thread_input_buffer_size_(0),
    panic_flush_(false)
{
    if(0 != pthread_key_create(&thread_input_buffer_key_, &destroy_thread_input_buffer))
        throw std::bad_alloc();
}

reckless::basic_log::basic_log(writer* pwriter, 
        std::size_t output_buffer_max_capacity,
        std::size_t shared_input_queue_size,
        std::size_t thread_input_buffer_size) :
    thread_input_buffer_size_(0),
    panic_flush_(false)
{
    if(0 != pthread_key_create(&thread_input_buffer_key_, &destroy_thread_input_buffer))
        throw std::bad_alloc();
    open(pwriter, output_buffer_max_capacity, shared_input_queue_size, thread_input_buffer_size);
}

reckless::basic_log::~basic_log()
{
    if(panic_flush_)
        return;
    if(is_open())
        close();
    auto result = pthread_key_delete(thread_input_buffer_key_);
    assert(result == 0);
}

void reckless::basic_log::open(writer* pwriter, 
        std::size_t output_buffer_max_capacity,
        std::size_t shared_input_queue_size,
        std::size_t thread_input_buffer_size)
{
    // The typical disk block size these days is 4 KiB (see
    // https://en.wikipedia.org/wiki/Advanced_Format). We'll make it twice
    // that just in case it grows larger, and to hide some of the effects of
    // misalignment.
    unsigned const ASSUMED_DISK_SECTOR_SIZE = 8192;
    if(output_buffer_max_capacity == 0 or shared_input_queue_size == 0
            or thread_input_buffer_size == 0)
    {
        if(output_buffer_max_capacity == 0)
            output_buffer_max_capacity = ASSUMED_DISK_SECTOR_SIZE;
        // TODO is it right to just do g_page_size/sizeof(commit_extent) if we want
        // the buffer to use up one page? There's likely more overhead in the
        // buffer.
        std::size_t page_size = detail::get_page_size();
        if(shared_input_queue_size == 0)
            shared_input_queue_size = page_size / sizeof(detail::commit_extent);
        if(thread_input_buffer_size == 0)
            thread_input_buffer_size = ASSUMED_DISK_SECTOR_SIZE;
    }
    assert(!is_open());
    reset_shared_input_queue(shared_input_queue_size);
    thread_input_buffer_size_ = thread_input_buffer_size;
    output_buffer_ = output_buffer(pwriter, output_buffer_max_capacity);
    output_thread_ = std::thread(std::mem_fn(&basic_log::output_worker), this);
}

void reckless::basic_log::close()
{
    using namespace detail;
    assert(is_open());
    // FIXME always signal a buffer full event, so we don't have to wait 1
    // second before the thread exits.
    queue_commit_extent({nullptr, nullptr});
    output_thread_.join();
    assert(shared_input_queue_->empty());
    
    output_buffer_ = output_buffer();
    thread_input_buffer_size_ = 0;
    shared_input_queue_ = std::experimental::nullopt;
    assert(!is_open());

}

void reckless::basic_log::panic_flush()
{
    panic_flush_ = true;
    shared_input_queue_full_event_.signal();
    panic_flush_done_event_.wait();
}

void reckless::basic_log::output_worker()
{
    using namespace detail;
    // Result of the last write attempt. Zero for no problems, or one of the
    // writer::errc values.
    unsigned error_state = 0;   

    // TODO if possible we should call signal_input_consumed() whenever the
    // output buffer is flushed, so threads aren't kept waiting indefinitely if
    // the queue never clears up.
    std::vector<thread_input_buffer*> touched_input_buffers;
    touched_input_buffers.reserve(std::max(8u, 2*std::thread::hardware_concurrency()));
    while(true) {
        commit_extent ce;
        pop_log_entries(ce);
            
        if(not ce.pinput_buffer) {
            // A null input-buffer pointer is the termination signal. Finish up
            // and exit the worker thread.
            if(unlikely(panic_flush_))
                on_panic_flush_done();
            output_buffer_.flush();
            return;
        }

        char* pinput_start = ce.pinput_buffer->input_start();
        while(pinput_start != ce.pcommit_end) {
            auto pdispatch = *reinterpret_cast<formatter_dispatch_function_t**>(pinput_start);
            if(WRAPAROUND_MARKER == pdispatch) {
                pinput_start = ce.pinput_buffer->wraparound();
                pdispatch = *reinterpret_cast<formatter_dispatch_function_t**>(pinput_start);
            }

            unsigned block_time_ms = 0;
            do {
                try {
                    // Call formatter. This is what produces actual data in the
                    // output buffer.
                    (*pdispatch)(apply_formatter, &output_buffer_, pinput_start);
                } catch(flush_error const& e) {
                    output_buffer_.revert_frame();
                    error_policy ep;
                    if(e.code() == writer::temporary_error) {
                        error_state = writer::temporary_error;
                        ep = temporary_error_policy_;
                    } else {
                        error_state = writer::permanent_error;
                        ep = permanent_error_policy_;
                    }

                    switch(ep) {
                    case ignore:
                    case notify_on_recovery:
                        break;
                    case block:
                        
                    }
                } catch(...) {
                    std::lock_guard<std::mutex> lk(callback_mutex_);
                    if(format_error_callback_) {
                        auto ti = *reinterpret_cast<std::type_info const*>((*pdispatch)(get_typeid, &output_buffer_, pinput_start));
                        try {
                            format_error_callback_(&output_buffer_,
                                std::current_exception(), ti);
                        } catch(...) {
                        }
                    }
            } while(retry);

            auto frame_size = static_cast<std::size_t>((*pdispatch)(destroy, &output_buffer_, pinput_start));
            pinput_start = ce.pinput_buffer->discard_input_frame(frame_size);
            if(likely(!panic_flush_)) {
                // If we're in panic-flush mode then we don't try to touch the
                // heap-allocated vector.
                if(not ce.pinput_buffer->input_consumed_flag) {
                    touched_input_buffers.push_back(ce.pinput_buffer);
                    ce.pinput_buffer->input_consumed_flag = true;
                }
            }
        }
    }
}

void reckless::basic_log::pop_log_entries(detail::commit_extent& ce)
{
    if(shared_input_queue_->pop(ce))
        return;

    if(unlikely(panic_flush_)) {
        // We are in panic-flush mode and the queue is empty. That
        // means we are done.
        on_panic_flush_done();  // never returns
        assert(false);
    }

    // The queue is empty; signal any threads that are waiting and then flush
    // the output buffer.
    shared_input_consumed_event_.signal();
    for(thread_input_buffer* pinput_buffer : touched_input_buffers)
        pinput_buffer->signal_input_consumed();
    for(thread_input_buffer* pbuffer : touched_input_buffers)
        pbuffer->input_consumed_flag = false;
    touched_input_buffers.clear();
    if(not output_buffer_.empty())
        output_buffer_.flush();

    // Wait until something comes in to the queue. Since logger threads do not
    // normally signal any event (unless the queue fills up), we have to poll
    // the queue. We use an exponential back off to not use too many CPU cycles
    // and allow other threads to run, but we don't wait for more than one
    // second.
    unsigned wait_time_ms = 0;
    while(not shared_input_queue_->pop(ce)) {
        shared_input_queue_full_event_.wait(wait_time_ms);
        wait_time_ms += std::max(1u, wait_time_ms/4);
        wait_time_ms = std::min(wait_time_ms, 1000u);
    }
}

void reckless::basic_log::queue_log_entries(detail::commit_extent const& ce)
{
    using namespace detail;
    if(unlikely(panic_flush_))
    {
        // Another visitor! Stay a while; stay forever!
        // When we are in panic mode because of a crash, we want to flush all
        // the log entries that were produced before the crash, but no more
        // than that. We could put a watermark in the queue and whatnot, but
        // really, when there's a crash in progress we will just confound the
        // problem if we keep trying to push stuff on the queue and muck about
        // with the heap.  Better to just suspend anything that tries, and wait
        // for the kill.
        while(true)
            sleep(3600);
    }
    if(unlikely(not shared_input_queue_->push(ce))) {
        do {
            shared_input_queue_full_event_.signal();
            shared_input_consumed_event_.wait();
        } while(not shared_input_queue_->push(ce));
    }
}

void reckless::basic_log::reset_shared_input_queue(std::size_t node_count)
{
    // boost's lockfree queue has no move constructor and provides no reserve()
    // function when you use fixed_sized policy. So we'll just explicitly
    // destroy the current queue and create a new one with the desired size. We
    // are guaranteed that the queue is nullopt since this is only called when
    // opening the log.
    assert(!shared_input_queue_);
    shared_input_queue_.emplace(node_count);
}

reckless::detail::thread_input_buffer* reckless::basic_log::init_input_buffer()
{
    auto p = detail::thread_input_buffer::create(thread_input_buffer_size_);
    try {
        int result = pthread_setspecific(thread_input_buffer_key_, p);
        if(detail::likely(result == 0))
            return p;
        else if(result == ENOMEM)
            throw std::bad_alloc();
        else
            throw std::system_error(result, std::system_category());
    } catch(...) {
        detail::thread_input_buffer::destroy(p);
        throw;
    }
}

void reckless::basic_log::on_panic_flush_done()
{
    output_buffer_.flush();
    panic_flush_done_event_.signal();
    // Sleep and wait for death.
    while(true)
    {
        sleep(3600);
    }
}
