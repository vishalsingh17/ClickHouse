#include <Server/HTTP/WriteBufferFromHTTPServerResponse.h>
#include <IO/HTTPCommon.h>
#include <IO/Progress.h>
#include <IO/WriteBufferFromString.h>
#include <IO/WriteHelpers.h>
#include <memory>
#include <sstream>
#include <string>

namespace DB
{


void WriteBufferFromHTTPServerResponse::startSendHeaders()
{
    if (!headers_started_sending)
    {
        headers_started_sending = true;

        if (response.getChunkedTransferEncoding())
            setChunked();

        if (add_cors_header)
            response.set("Access-Control-Allow-Origin", "*");

        setResponseDefaultHeaders(response, keep_alive_timeout);

        std::stringstream header;
        response.beginWrite(header);
        auto header_str = header.str();
        socketSendBytes(header_str.data(), header_str.size());
    }
}

void WriteBufferFromHTTPServerResponse::writeHeaderProgressImpl(const char * header_name)
{
    if (is_http_method_head || headers_finished_sending || !headers_started_sending)
        return;

    WriteBufferFromOwnString progress_string_writer;

    accumulated_progress.writeJSON(progress_string_writer);

    socketSendBytes(header_name, strlen(header_name));
    socketSendBytes(progress_string_writer.str().data(), progress_string_writer.str().size());
    socketSendBytes("\r\n", 2);
}

void WriteBufferFromHTTPServerResponse::writeHeaderSummary()
{
    accumulated_progress.incrementElapsedNs(progress_watch.elapsed());
    writeHeaderProgressImpl("X-ClickHouse-Summary: ");
}

void WriteBufferFromHTTPServerResponse::writeHeaderProgress()
{
    writeHeaderProgressImpl("X-ClickHouse-Progress: ");
}

void WriteBufferFromHTTPServerResponse::writeExceptionCode()
{
    if (headers_finished_sending || !exception_code)
        return;
    if (headers_started_sending)
    {
        socketSendBytes("X-ClickHouse-Exception-Code: ", sizeof("X-ClickHouse-Exception-Code: ") - 1);
        auto str_code = std::to_string(exception_code);
        socketSendBytes(str_code.data(), str_code.size());
        socketSendBytes("\r\n", 2);
    }
}

void WriteBufferFromHTTPServerResponse::finishSendHeaders()
{
    if (headers_finished_sending)
        return;

    if (!headers_started_sending)
        startSendHeaders();

    writeHeaderSummary();
    writeExceptionCode();

    headers_finished_sending = true;

    /// Send end of headers delimiter.
    socketSendBytes("\r\n", 2);
}


void WriteBufferFromHTTPServerResponse::nextImpl()
{
    if (!initialized)
    {
        std::lock_guard lock(mutex);
        /// Initialize as early as possible since if the code throws,
        /// next() should not be called anymore.
        initialized = true;

        startSendHeaders();
        finishSendHeaders();
    }

    if (!is_http_method_head)
        HTTPWriteBuffer::nextImpl();
}


WriteBufferFromHTTPServerResponse::WriteBufferFromHTTPServerResponse(
    HTTPServerResponse & response_,
    bool is_http_method_head_,
    size_t keep_alive_timeout_,
    const CurrentMetrics::Metric & write_metric_)
    : HTTPWriteBuffer(response_.getSocket(), write_metric_)
    , response(response_)
    , is_http_method_head(is_http_method_head_)
    , keep_alive_timeout(keep_alive_timeout_)
{
}


void WriteBufferFromHTTPServerResponse::onProgress(const Progress & progress)
{
    std::lock_guard lock(mutex);

    /// Cannot add new headers if body was started to send.
    if (headers_finished_sending)
        return;

    accumulated_progress.incrementPiecewiseAtomically(progress);
    if (send_progress && progress_watch.elapsed() >= send_progress_interval_ms * 1000000)
    {
        accumulated_progress.incrementElapsedNs(progress_watch.elapsed());
        progress_watch.restart();

        /// Send all common headers before our special progress headers.
        startSendHeaders();
        writeHeaderProgress();
    }
}

WriteBufferFromHTTPServerResponse::~WriteBufferFromHTTPServerResponse()
{
    finalize();
}

void WriteBufferFromHTTPServerResponse::finalizeImpl()
{
    if (!is_http_method_head)
        HTTPWriteBuffer::finalizeImpl();

    std::lock_guard lock(mutex);

    if (headers_finished_sending)
        return;

    /// If no body data just send header
    startSendHeaders();
    finishSendHeaders();
}


}
