// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <boost/algorithm/string/replace.hpp>
#include <cryptopp/aes.h>
#include <cryptopp/modes.h>
#include <fmt/format.h>
#include "common/archives.h"
#include "common/assert.h"
#include "common/file_util.h"
#include "common/scope_exit.h"
#include "common/string_util.h"
#include "core/core.h"
#include "core/file_sys/archive_ncch.h"
#include "core/file_sys/file_backend.h"
#include "core/hle/ipc_helpers.h"
#include "core/hle/kernel/ipc.h"
#include "core/hle/romfs.h"
#include "core/hle/service/fs/archive.h"
#include "core/hle/service/http/http_c.h"
#include "core/hw/aes/key.h"

SERIALIZE_EXPORT_IMPL(Service::HTTP::HTTP_C)
SERIALIZE_EXPORT_IMPL(Service::HTTP::SessionData)

namespace Service::HTTP {

#include "ctr-common-1-cert.h"
#include "ctr-common-1-key.h"

namespace ErrCodes {
enum {
    InvalidRequestState = 22,
    TooManyContexts = 26,
    InvalidRequestMethod = 32,
    HeaderNotFound = 40,
    BufferTooSmall = 43,

    /// This error is returned in multiple situations: when trying to add Post data that is
    /// incompatible with the one that is used in the session, or when trying to use chunked
    /// requests with Post data already set
    IncompatibleAddPostData = 50,

    InvalidPostDataEncoding = 53,
    IncompatibleSendPostData = 54,
    WrongCertID = 57,
    CertAlreadySet = 61,
    ContextNotFound = 100,
    Timeout = 105,

    /// This error is returned in multiple situations: when trying to initialize an
    /// already-initialized session, or when using the wrong context handle in a context-bound
    /// session
    SessionStateError = 102,

    WrongCertHandle = 201,
    TooManyClientCerts = 203,
    NotImplemented = 1012,
};
}

constexpr Result ErrorStateError = // 0xD8A0A066
    Result(ErrCodes::SessionStateError, ErrorModule::HTTP, ErrorSummary::InvalidState,
           ErrorLevel::Permanent);
constexpr Result ErrorNotImplemented = // 0xD960A3F4
    Result(ErrCodes::NotImplemented, ErrorModule::HTTP, ErrorSummary::Internal,
           ErrorLevel::Permanent);
constexpr Result ErrorTooManyClientCerts = // 0xD8A0A0CB
    Result(ErrCodes::TooManyClientCerts, ErrorModule::HTTP, ErrorSummary::InvalidState,
           ErrorLevel::Permanent);
constexpr Result ErrorHeaderNotFound = // 0xD8A0A028
    Result(ErrCodes::HeaderNotFound, ErrorModule::HTTP, ErrorSummary::InvalidState,
           ErrorLevel::Permanent);
constexpr Result ErrorBufferSmall = // 0xD840A02B
    Result(ErrCodes::BufferTooSmall, ErrorModule::HTTP, ErrorSummary::WouldBlock,
           ErrorLevel::Permanent);
constexpr Result ErrorWrongCertID = // 0xD8E0B839
    Result(ErrCodes::WrongCertID, ErrorModule::SSL, ErrorSummary::InvalidArgument,
           ErrorLevel::Permanent);
constexpr Result ErrorWrongCertHandle = // 0xD8A0A0C9
    Result(ErrCodes::WrongCertHandle, ErrorModule::HTTP, ErrorSummary::InvalidState,
           ErrorLevel::Permanent);
constexpr Result ErrorCertAlreadySet = // 0xD8A0A03D
    Result(ErrCodes::CertAlreadySet, ErrorModule::HTTP, ErrorSummary::InvalidState,
           ErrorLevel::Permanent);
constexpr Result ErrorIncompatibleAddPostData = // 0xD8A0A032
    Result(ErrCodes::IncompatibleAddPostData, ErrorModule::HTTP, ErrorSummary::InvalidState,
           ErrorLevel::Permanent);
constexpr Result ErrorContextNotFound = // 0xD8A0A064
    Result(ErrCodes::ContextNotFound, ErrorModule::HTTP, ErrorSummary::InvalidState,
           ErrorLevel::Permanent);
constexpr Result ErrorTimeout = // 0xD820A069
    Result(ErrCodes::Timeout, ErrorModule::HTTP, ErrorSummary::NothingHappened,
           ErrorLevel::Permanent);
constexpr Result ErrorTooManyContexts = // 0xD8A0A01A
    Result(ErrCodes::TooManyContexts, ErrorModule::HTTP, ErrorSummary::InvalidState,
           ErrorLevel::Permanent);
constexpr Result ErrorInvalidRequestMethod = // 0xD8A0A020
    Result(ErrCodes::InvalidRequestMethod, ErrorModule::HTTP, ErrorSummary::InvalidState,
           ErrorLevel::Permanent);
constexpr Result ErrorInvalidRequestState = // 0xD8A0A016
    Result(ErrCodes::InvalidRequestState, ErrorModule::HTTP, ErrorSummary::InvalidState,
           ErrorLevel::Permanent);
constexpr Result ErrorInvalidPostDataEncoding = // 0xD8A0A035
    Result(ErrCodes::InvalidPostDataEncoding, ErrorModule::HTTP, ErrorSummary::InvalidState,
           ErrorLevel::Permanent);
constexpr Result ErrorIncompatibleSendPostData = // 0xD8A0A036
    Result(ErrCodes::IncompatibleSendPostData, ErrorModule::HTTP, ErrorSummary::InvalidState,
           ErrorLevel::Permanent);

// Splits URL into its components. Example: https://citra-emu.org:443/index.html
// is_https: true; host: citra-emu.org; port: 443; path: /index.html
static URLInfo SplitUrl(const std::string& url) {
    const std::string prefix = "://";
    constexpr int default_http_port = 80;
    constexpr int default_https_port = 443;

    std::string host;
    int port = -1;
    std::string path;

    const auto scheme_end = url.find(prefix);
    const auto prefix_end = scheme_end == std::string::npos ? 0 : scheme_end + prefix.length();
    bool is_https = scheme_end != std::string::npos && url.starts_with("https");
    const auto path_index = url.find("/", prefix_end);

    if (path_index == std::string::npos) {
        // If no path is specified after the host, set it to "/"
        host = url.substr(prefix_end);
        path = "/";
    } else {
        host = url.substr(prefix_end, path_index - prefix_end);
        path = url.substr(path_index);
    }

    const auto port_start = host.find(":");
    if (port_start != std::string::npos) {
        std::string port_str = host.substr(port_start + 1);
        host = host.substr(0, port_start);
        char* p_end = nullptr;
        port = std::strtol(port_str.c_str(), &p_end, 10);
        if (*p_end) {
            port = -1;
        }
    }

    if (port == -1) {
        port = is_https ? default_https_port : default_http_port;
    }
    return URLInfo{
        .is_https = is_https,
        .host = host,
        .port = port,
        .path = path,
    };
}

static constexpr u64 MaxGuestDownloadSize = 0xFFFFFFFFULL;

static u32 ClampDownloadSizeToGuest(u64 size) {
    return static_cast<u32>(std::min(size, MaxGuestDownloadSize));
}

static bool HeaderNameEquals(std::string_view lhs, std::string_view rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }

    return std::equal(lhs.begin(), lhs.end(), rhs.begin(), [](unsigned char a, unsigned char b) {
        return std::tolower(a) == std::tolower(b);
    });
}

static httplib::Headers::const_iterator FindHeaderCaseInsensitive(const httplib::Headers& headers,
                                                                  std::string_view name) {
    const auto exact = headers.find(std::string(name));
    if (exact != headers.end()) {
        return exact;
    }

    return std::find_if(headers.begin(), headers.end(), [name](const auto& header) {
        return HeaderNameEquals(header.first, name);
    });
}

static std::string RedactUrlForLog(std::string_view url) {
    const auto query_start = url.find('?');
    if (query_start == std::string_view::npos) {
        return std::string(url);
    }

    std::string redacted(url.substr(0, query_start));
    redacted += "?<redacted>";
    return redacted;
}

static std::optional<u64> ParseContentLength(const httplib::Headers& headers) {
    const auto it = FindHeaderCaseInsensitive(headers, "Content-Length");
    if (it == headers.end()) {
        return std::nullopt;
    }

    char* parse_end = nullptr;
    const unsigned long long parsed_length = std::strtoull(it->second.c_str(), &parse_end, 10);
    if (parse_end == it->second.c_str() || *parse_end != '\0') {
        return std::nullopt;
    }

    return static_cast<u64>(parsed_length);
}

static constexpr std::size_t MinimumBufferedResponseBodyBytes = 128 * 1024;
static constexpr std::size_t DefaultBufferedResponseBodyBytes = 1024 * 1024;
static constexpr std::size_t MirroredResponseBodyBytes = 1024 * 1024;

static std::size_t GetMaxBufferedResponseBodyBytes(const Context& context) {
    return std::max<std::size_t>(DefaultBufferedResponseBodyBytes,
                                 std::max<std::size_t>(context.socket_buffer_size,
                                                       MinimumBufferedResponseBodyBytes));
}

static bool IsRequestComplete(const Context& context) {
    return context.request_future.valid() &&
           context.request_future.wait_for(std::chrono::milliseconds(0)) ==
               std::future_status::ready;
}

static u64 GetResponseTotalSizeLocked(const Context& context) {
    if (const auto content_length = ParseContentLength(context.response.headers)) {
        return *content_length;
    }

    const u64 progress_total = context.total_download_size_bytes.load();
    if (progress_total != 0) {
        return progress_total;
    }

    if (context.response_complete) {
        return context.current_download_size_bytes.load();
    }

    return 0;
}

static void PrepareContextForRequest(Context& context) {
    std::lock_guard lock(context.response_mutex);
    context.response = httplib::Response{};
    context.response_body_buffer.clear();
    context.current_copied_data = 0;
    context.current_download_size_bytes = 0;
    context.total_download_size_bytes = 0;
    context.response_headers_ready = false;
    context.response_complete = false;
    context.response_cancelled = false;
    context.response_failed = false;
}

static std::size_t WriteHeaders(httplib::Stream& stream,
                                std::span<const Context::RequestHeader> headers) {
    std::size_t write_len = 0;
    for (const auto& header : headers) {
        auto len = stream.write_format("%s: %s\r\n", header.name.c_str(), header.value.c_str());
        if (len < 0) {
            return len;
        }
        write_len += len;
    }
    auto len = stream.write("\r\n");
    if (len < 0) {
        return len;
    }
    write_len += len;
    return write_len;
}

static std::string StringFromHTTPStaticBuffer(const std::vector<u8>& buffer, u32 advertised_size) {
    const std::size_t copy_size =
        std::min<std::size_t>(buffer.size(), static_cast<std::size_t>(advertised_size));
    if (copy_size == 0) {
        return {};
    }

    std::string out(reinterpret_cast<const char*>(buffer.data()), copy_size);
    Common::TruncateString(out);
    return out;
}

static void SerializeChunkedAsciiPostData(httplib::DataSink& sink, const Context::Params& params) {
    std::string query;

    for (auto it = params.begin(); it != params.end(); ++it) {
        if (it != params.begin()) {
            sink.os << "&";
        }

        query =
            fmt::format("{}={}", it->first, httplib::detail::encode_query_param(it->second.value));
        boost::replace_all(query, "*", "%2A");
        sink.os << query;
    }
}

static void SerializeChunkedMultipartPostData(httplib::DataSink& sink,
                                              const Context::Params& params,
                                              const std::string& boundary) {
    for (const auto& param : params) {
        const auto item = param.second.ToMultipartForm();
        std::string body = httplib::detail::serialize_multipart_formdata_item_begin(item, boundary);
        body += item.content + httplib::detail::serialize_multipart_formdata_item_end();
        sink.os << body;
    }

    sink.os << httplib::detail::serialize_multipart_formdata_finish(boundary);
}

std::size_t Context::HandleHeaderWrite(std::vector<Context::RequestHeader>& pending_headers,
                                       httplib::Stream& strm, httplib::Headers& httplib_headers) {
    std::vector<Context::RequestHeader> final_headers;
    std::vector<Context::RequestHeader>::iterator it_pending_headers;
    httplib::Headers::iterator it_httplib_headers;

    auto find_pending_header = [&pending_headers](const std::string& str) {
        return std::find_if(pending_headers.begin(), pending_headers.end(),
                            [&str](Context::RequestHeader& rh) { return rh.name == str; });
    };

    // Watch out for header ordering!!
    // First: Host
    it_pending_headers = find_pending_header("Host");
    if (it_pending_headers != pending_headers.end()) {
        final_headers.push_back(
            Context::RequestHeader(it_pending_headers->name, it_pending_headers->value));
        pending_headers.erase(it_pending_headers);
    } else {
        it_httplib_headers = httplib_headers.find("Host");
        if (it_httplib_headers != httplib_headers.end()) {
            final_headers.push_back(
                Context::RequestHeader(it_httplib_headers->first, it_httplib_headers->second));
        }
    }

    // Second, user defined headers
    // Third, Content-Type (optional, appended by MakeRequest)
    for (const auto& header : pending_headers) {
        final_headers.push_back(header);
    }

    // Fourth: Content-Length
    it_pending_headers = find_pending_header("Content-Length");
    if (it_pending_headers == pending_headers.end()) {
        if ((method == RequestMethod::Post || method == RequestMethod::Put) && !chunked_request) {
            it_httplib_headers = httplib_headers.find("Content-Length");
            if (it_httplib_headers != httplib_headers.end()) {
                final_headers.push_back(
                    Context::RequestHeader(it_httplib_headers->first, it_httplib_headers->second));
            }
        }
    }

    // Fifth: Transfer-Encoding
    if (chunked_request) {
        final_headers.push_back(Context::RequestHeader("Transfer-Encoding", "chunked"));
    }

    return WriteHeaders(strm, final_headers);
};

void Context::ParseAsciiPostData() {
    httplib::Params ascii_form;
    for (auto param : post_data) {
        ascii_form.emplace(param.first, param.second.value);
    }

    post_data_raw = httplib::detail::params_to_query_str(ascii_form);
    boost::replace_all(post_data_raw, "*", "%2A");
}

std::string Context::ParseMultipartFormData() {
    httplib::MultipartFormDataItems multipart_form;
    for (auto param : post_data) {
        multipart_form.push_back(param.second.ToMultipartForm());
    }

    multipart_boundary = httplib::detail::make_multipart_data_boundary();
    post_data_raw =
        httplib::detail::serialize_multipart_formdata(multipart_form, multipart_boundary);
    return httplib::detail::serialize_multipart_formdata_get_content_type(multipart_boundary);
}

void Context::MakeRequest() {
    ASSERT(state == RequestState::NotStarted);

    state = RequestState::SendingRequest;

    static const std::unordered_map<RequestMethod, std::string> request_method_strings{
        {RequestMethod::Get, "GET"},       {RequestMethod::Post, "POST"},
        {RequestMethod::Head, "HEAD"},     {RequestMethod::Put, "PUT"},
        {RequestMethod::Delete, "DELETE"}, {RequestMethod::PostEmpty, "POST"},
        {RequestMethod::PutEmpty, "PUT"},
    };

    URLInfo url_info = SplitUrl(url);

    httplib::Request request;
    std::vector<Context::RequestHeader> pending_headers;
    request.method = request_method_strings.at(method);
    request.path = url_info.path;

    // Apply URL replacements if any
    url_info.host = url_replacer->Apply(url_info.host);

    for (const auto& header : headers) {
        pending_headers.push_back(header);
    }

    httplib::Params ascii_form;
    httplib::MultipartFormDataItems multipart_form;
    if ((method == RequestMethod::Post || method == RequestMethod::Put) && !chunked_request) {
        switch (post_data_encoding) {
        case PostDataEncoding::AsciiForm:
            ParseAsciiPostData();
            pending_headers.push_back(
                Context::RequestHeader("Content-Type", "application/x-www-form-urlencoded"));
            break;
        case PostDataEncoding::MultipartForm:
            pending_headers.push_back(
                Context::RequestHeader("Content-Type", ParseMultipartFormData()));
            break;
        case PostDataEncoding::Auto:
            if (!post_data.empty()) {
                if (force_multipart) {
                    pending_headers.push_back(
                        Context::RequestHeader("Content-Type", ParseMultipartFormData()));
                } else {
                    pending_headers.push_back(Context::RequestHeader(
                        "Content-Type", "application/x-www-form-urlencoded"));
                    ParseAsciiPostData();
                }
            }
            break;
        }
    }

    // httplib doesn't expose setting the content provider for the request when not using the usual
    // send methods like Client::Post or Client::Put, so we have to set the internal fields manually
    if (!chunked_request) {
        request.content_length_ = post_data_raw.size();
        request.content_provider_ = [this](size_t offset, size_t length, httplib::DataSink& sink) {
            return ContentProvider(offset, length, sink);
        };
    } else {
        if (post_data_type == PostDataType::MultipartForm) {
            multipart_boundary = httplib::detail::make_multipart_data_boundary();
            pending_headers.push_back(Context::RequestHeader(
                "Content-Type", httplib::detail::serialize_multipart_formdata_get_content_type(
                                    multipart_boundary)));
        }

        if (post_data_type == PostDataType::Raw && chunked_content_length > 0) {
            pending_headers.push_back(Context::RequestHeader(
                "Content-Length", fmt::format("{}", chunked_content_length)));
        }

        request.content_length_ = 0;
        request.content_provider_ =
            httplib::detail::ContentProviderAdapter([this](size_t offset, httplib::DataSink& sink) {
                return ChunkedContentProvider(offset, sink);
            });
        request.is_chunked_content_provider_ = true;
    }

    ConfigureStreamingRequest(request);

    if (url_info.is_https) {
        MakeRequestSSL(request, url_info, pending_headers);
    } else {
        MakeRequestNonSSL(request, url_info, pending_headers);
    }
}

void Context::ConfigureStreamingRequest(httplib::Request& request) {
    request.response_handler = [this](const httplib::Response& received_response) {
        return HandleResponseHeaders(received_response);
    };

    request.content_receiver = [this](const char* data, std::size_t data_length, u64 offset,
                                      u64 total_length) {
        return HandleResponseContent(data, data_length, offset, total_length);
    };

    request.progress = [this](u64 current, u64 total) -> bool {
        bool keep_going = true;
        {
            std::lock_guard lock(response_mutex);
            current_download_size_bytes = current;
            if (total != 0) {
                total_download_size_bytes = total;
            }
            keep_going = !response_cancelled;
        }
        response_cv.notify_all();
        return keep_going;
    };
}

bool Context::HandleResponseHeaders(const httplib::Response& received_response) {
    bool keep_going = true;
    {
        std::lock_guard lock(response_mutex);
        response = received_response;
        response.body.clear();
        if (const auto content_length = ParseContentLength(response.headers)) {
            total_download_size_bytes = *content_length;
        }
        response_headers_ready = true;
        response_failed = false;
        state = RequestState::ReceivingBody;
        keep_going = !response_cancelled;
    }
    response_cv.notify_all();
    return keep_going;
}

bool Context::HandleResponseContent(const char* data, std::size_t data_length, u64 offset,
                                    u64 total_length) {
    const auto* bytes = reinterpret_cast<const u8*>(data);
    std::size_t copied = 0;

    while (copied < data_length) {
        std::unique_lock lock(response_mutex);
        response_cv.wait(lock, [this] {
            return response_cancelled ||
                   response_body_buffer.size() < GetMaxBufferedResponseBodyBytes(*this);
        });

        if (response_cancelled) {
            return false;
        }

        const std::size_t capacity =
            GetMaxBufferedResponseBodyBytes(*this) - response_body_buffer.size();
        const std::size_t chunk_size = std::min<std::size_t>(capacity, data_length - copied);
        response_body_buffer.insert(response_body_buffer.end(), bytes + copied,
                                    bytes + copied + chunk_size);

        if (response.body.size() < MirroredResponseBodyBytes) {
            const std::size_t mirror_size =
                std::min<std::size_t>(chunk_size, MirroredResponseBodyBytes - response.body.size());
            response.body.append(reinterpret_cast<const char*>(bytes + copied), mirror_size);
        }

        const u64 received = offset + copied + chunk_size;
        current_download_size_bytes = std::max(current_download_size_bytes.load(), received);
        if (total_length != 0) {
            total_download_size_bytes = total_length;
        }
        state = RequestState::ReceivingBody;
        copied += chunk_size;

        lock.unlock();
        response_cv.notify_all();
    }

    return true;
}

void Context::FinishRequest(bool request_success, httplib::Error error) {
    bool cancelled = false;
    int status = 0;
    std::size_t buffered = 0;
    {
        std::lock_guard lock(response_mutex);
        cancelled = response_cancelled;
        if (!request_success && !response_cancelled) {
            response_failed = true;
        }
        if (!response_headers_ready && response.status > 0) {
            response_headers_ready = true;
            if (const auto content_length = ParseContentLength(response.headers)) {
                total_download_size_bytes = *content_length;
            }
        }
        response_complete = true;
        buffered = response_body_buffer.size();
        status = response.status;
        if (response_headers_ready && !response_body_buffer.empty()) {
            state = RequestState::Received;
        } else {
            state = RequestState::Completed;
        }
    }
    response_cv.notify_all();

    if (!request_success && !cancelled) {
        LOG_ERROR(Service_HTTP, "Request failed: {}: {}", error, httplib::to_string(error));
    } else {
        LOG_DEBUG(Service_HTTP, "Request finished, status={} buffered={} cancelled={}", status,
                  buffered, cancelled);
    }
}

void Context::CancelRequest() {
    {
        std::lock_guard lock(response_mutex);
        response_cancelled = true;
    }
    response_cv.notify_all();
}

Result Context::WaitForResponseHeaders(bool timeout, u64 timeout_nanos) {
    std::unique_lock lock(response_mutex);
    const auto ready = [this] {
        return response_headers_ready || response_complete || response_cancelled || response_failed;
    };

    if (timeout) {
        if (!response_cv.wait_for(lock, std::chrono::nanoseconds(timeout_nanos), ready)) {
            return ErrorTimeout;
        }
    } else {
        response_cv.wait(lock, ready);
    }

    return response_headers_ready ? ResultSuccess : ErrorTimeout;
}

Result Context::WaitForBodyData(std::size_t requested_size, bool timeout, u64 timeout_nanos) {
    std::unique_lock lock(response_mutex);
    const auto ready = [this, requested_size] {
        if (response_cancelled || response_failed || requested_size == 0) {
            return true;
        }
        const std::size_t target_size =
            std::min<std::size_t>(requested_size, GetMaxBufferedResponseBodyBytes(*this));
        return response_body_buffer.size() >= target_size || response_complete;
    };

    if (timeout) {
        if (!response_cv.wait_for(lock, std::chrono::nanoseconds(timeout_nanos), ready)) {
            return ErrorTimeout;
        }
    } else {
        response_cv.wait(lock, ready);
    }

    if ((response_cancelled || response_failed) && response_body_buffer.empty()) {
        return ErrorTimeout;
    }

    return ResultSuccess;
}

std::vector<u8> Context::CopyBodyData(std::size_t max_size, Result& result) {
    std::vector<u8> out;
    {
        std::lock_guard lock(response_mutex);
        const std::size_t write_size = std::min<std::size_t>(max_size, response_body_buffer.size());
        out.reserve(write_size);
        for (std::size_t i = 0; i < write_size; ++i) {
            out.push_back(response_body_buffer.front());
            response_body_buffer.pop_front();
        }

        current_copied_data += write_size;
        const bool has_more_body = !response_body_buffer.empty() || !response_complete;
        if ((response_cancelled || response_failed) && response_body_buffer.empty()) {
            result = ErrorTimeout;
        } else {
            result = has_more_body ? ErrorBufferSmall : ResultSuccess;
        }

        if (result == ResultSuccess) {
            state = RequestState::Completed;
        } else if (state != RequestState::Completed) {
            state = RequestState::ReceivingBody;
        }
    }
    response_cv.notify_all();
    return out;
}

void Context::MakeRequestNonSSL(httplib::Request& request, const URLInfo& url_info,
                                std::vector<Context::RequestHeader>& pending_headers) {
    httplib::Error error{-1};
    std::unique_ptr<httplib::Client> client =
        std::make_unique<httplib::Client>(url_info.host, url_info.port);
    if (proxy) {
        client->set_proxy(proxy->url, proxy->port);
        if (!proxy->username.empty() || !proxy->password.empty()) {
            client->set_proxy_basic_auth(proxy->username, proxy->password);
        }
    }
    client->set_header_writer(
        [this, &pending_headers](httplib::Stream& strm, httplib::Headers& httplib_headers) {
            return HandleHeaderWrite(pending_headers, strm, httplib_headers);
        });

    const bool request_success = client->send(request, response, error);
    FinishRequest(request_success, error);
}

void Context::MakeRequestSSL(httplib::Request& request, const URLInfo& url_info,
                             std::vector<Context::RequestHeader>& pending_headers) {
    httplib::Error error{-1};
    X509* cert = nullptr;
    EVP_PKEY* key = nullptr;
    const unsigned char* cert_data = nullptr;
    const unsigned char* key_data = nullptr;
    long cert_size = 0;
    long key_size = 0;
    SCOPE_EXIT({
        if (cert) {
            X509_free(cert);
        }
        if (key) {
            EVP_PKEY_free(key);
        }
    });

    if (uses_default_client_cert) {
        cert_data = clcert_data->certificate.data();
        key_data = clcert_data->private_key.data();
        cert_size = static_cast<long>(clcert_data->certificate.size());
        key_size = static_cast<long>(clcert_data->private_key.size());
    } else if (auto client_cert = ssl_config.client_cert_ctx.lock()) {
        cert_data = client_cert->certificate.data();
        key_data = client_cert->private_key.data();
        cert_size = static_cast<long>(client_cert->certificate.size());
        key_size = static_cast<long>(client_cert->private_key.size());
    }

    std::unique_ptr<httplib::SSLClient> client;
    if (cert_data && key_data) {
        cert = d2i_X509(nullptr, &cert_data, cert_size);
        key = d2i_PrivateKey(EVP_PKEY_RSA, nullptr, &key_data, key_size);
        client = std::make_unique<httplib::SSLClient>(url_info.host, url_info.port, cert, key);
    } else {
        client = std::make_unique<httplib::SSLClient>(url_info.host, url_info.port);
    }
    if (proxy) {
        client->set_proxy(proxy->url, proxy->port);
        if (!proxy->username.empty() || !proxy->password.empty()) {
            client->set_proxy_basic_auth(proxy->username, proxy->password);
        }
    }
    // TODO(B3N30): Check for SSLOptions-Bits and set the verify method accordingly
    // https://www.3dbrew.org/wiki/SSL_Services#SSLOpt
    // Hack: Since for now RootCerts are not implemented we set the VerifyMode to None.
    client->enable_server_certificate_verification(false);

    client->set_header_writer(
        [this, &pending_headers](httplib::Stream& strm, httplib::Headers& httplib_headers) {
            return HandleHeaderWrite(pending_headers, strm, httplib_headers);
        });

    const bool request_success = client->send(request, response, error);
    FinishRequest(request_success, error);
}

bool Context::ContentProvider(size_t offset, size_t length, httplib::DataSink& sink) {
    if (!post_data_raw.empty()) {
        sink.write(post_data_raw.data() + offset, length);
    }

    // This state is set after sending the request, even if it hasn't received a response yet
    state = RequestState::ReceivingResponse;
    return true;
}

bool Context::ChunkedContentProvider(size_t offset, httplib::DataSink& sink) {
    finish_post_data.Wait();

    switch (post_data_type) {
    case PostDataType::AsciiForm:
        SerializeChunkedAsciiPostData(sink, post_data);
        break;
    case PostDataType::MultipartForm:
        SerializeChunkedMultipartPostData(sink, post_data, multipart_boundary);
        break;
    // Write the data values
    case PostDataType::Raw:
        for (const auto& data : post_data) {
            sink.os << data.second.value;
        }
        break;
    }

    sink.done();
    // This state is set after sending the request, even if it hasn't received a response yet
    state = RequestState::ReceivingResponse;
    return true;
}

void HTTP_C::Initialize(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const u32 shmem_size = rp.Pop<u32>();
    u32 pid = rp.PopPID();
    shared_memory = rp.PopObject<Kernel::SharedMemory>();
    if (shared_memory) {
        shared_memory->SetName("HTTP_C:shared_memory");
    }

    LOG_DEBUG(Service_HTTP, "called, shared memory size: {} pid: {}", shmem_size, pid);

    auto* session_data = GetSessionData(ctx.Session());
    ASSERT(session_data);

    if (session_data->initialized) {
        LOG_ERROR(Service_HTTP, "Tried to initialize an already initialized session");
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
        rb.Push(ErrorStateError);
        return;
    }

    session_data->initialized = true;
    session_data->session_id = ++session_counter;

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    // This returns 0xd8a0a046 if no network connection is available.
    // Just assume we are always connected.
    rb.Push(ResultSuccess);
}

void HTTP_C::InitializeConnectionSession(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const Context::Handle context_handle = rp.Pop<u32>();
    u32 pid = rp.PopPID();

    LOG_DEBUG(Service_HTTP, "called, context_id={} pid={}", context_handle, pid);

    auto* session_data = GetSessionData(ctx.Session());
    ASSERT(session_data);

    if (session_data->initialized) {
        LOG_ERROR(Service_HTTP, "Tried to initialize an already initialized session");
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
        rb.Push(ErrorStateError);
        return;
    }

    // TODO(Subv): Check that the input PID matches the PID that created the context.
    auto itr = contexts.find(context_handle);
    if (itr == contexts.end()) {
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
        rb.Push(ErrorContextNotFound);
        return;
    }

    session_data->initialized = true;
    session_data->session_id = ++session_counter;
    // Bind the context to the current session.
    session_data->current_http_context = context_handle;

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(ResultSuccess);
}

void HTTP_C::BeginRequest(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const Context::Handle context_handle = rp.Pop<u32>();

    LOG_DEBUG(Service_HTTP, "called, context_id={}", context_handle);

    if (!PerformStateChecks(ctx, rp, context_handle)) {
        return;
    }

    Context& http_context = GetContext(context_handle);

    // This should never happen in real hardware, but can happen on citra.
    if (http_context.uses_default_client_cert && !http_context.clcert_data->init) {
        LOG_ERROR(Service_HTTP, "Failed to begin HTTP request: client cert not found.");
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
        rb.Push(ErrorStateError);
        return;
    }

    // On a 3DS BeginRequest and BeginRequestAsync will push the Request to a worker queue.
    // You can only enqueue 8 requests at the same time.
    // trying to enqueue any more will either fail (BeginRequestAsync), or block (BeginRequest)
    // Note that you only can have 8 Contexts at a time. So this difference shouldn't matter
    // Then there are 3? worker threads that pop the requests from the queue and send them
    // For now make every request async in it's own thread.

    // This always returns success, but the request is only performed when it hasn't started

    if (http_context.state == RequestState::NotStarted) {
        if (http_context.method == RequestMethod::Post && !http_context.post_data_added) {
            http_context.post_pending_request = true;
        } else {
            PrepareContextForRequest(http_context);
            http_context.request_future =
                std::async(std::launch::async, &Context::MakeRequest, std::ref(http_context));
        }
    }

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(ResultSuccess);
}

void HTTP_C::BeginRequestAsync(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const Context::Handle context_handle = rp.Pop<u32>();

    LOG_DEBUG(Service_HTTP, "called, context_id={}", context_handle);

    if (!PerformStateChecks(ctx, rp, context_handle)) {
        return;
    }

    Context& http_context = GetContext(context_handle);

    // This should never happen in real hardware, but can happen on citra.
    if (http_context.uses_default_client_cert && !http_context.clcert_data->init) {
        LOG_ERROR(Service_HTTP, "Failed to begin HTTP request: client cert not found.");
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
        rb.Push(ErrorStateError);
        return;
    }

    // On a 3DS BeginRequest and BeginRequestAsync will push the Request to a worker queue.
    // You can only enqueue 8 requests at the same time.
    // trying to enqueue any more will either fail (BeginRequestAsync), or block (BeginRequest)
    // Note that you only can have 8 Contexts at a time. So this difference shouldn't matter
    // Then there are 3? worker threads that pop the requests from the queue and send them
    // For now make every request async in it's own thread.

    // This always returns success, but the request is only performed when it hasn't started
    if (http_context.state == RequestState::NotStarted) {
        if (http_context.method == RequestMethod::Post && !http_context.post_data_added) {
            http_context.post_pending_request = true;
        } else {
            PrepareContextForRequest(http_context);
            http_context.request_future =
                std::async(std::launch::async, &Context::MakeRequest, std::ref(http_context));
        }
    }

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(ResultSuccess);
}

void HTTP_C::ReceiveData(Kernel::HLERequestContext& ctx) {
    ReceiveDataImpl(ctx, false);
}

void HTTP_C::ReceiveDataTimeout(Kernel::HLERequestContext& ctx) {
    ReceiveDataImpl(ctx, true);
}

void HTTP_C::ReceiveDataImpl(Kernel::HLERequestContext& ctx, bool timeout) {
    IPC::RequestParser rp(ctx);

    struct AsyncData {
        // Input
        u64 timeout_nanos = 0;
        bool timeout;
        Context::Handle context_handle;
        u32 buffer_size;
        Kernel::MappedBuffer* buffer;
        // Output
        Result async_res = ResultSuccess;
    };
    std::shared_ptr<AsyncData> async_data = std::make_shared<AsyncData>();
    async_data->timeout = timeout;
    async_data->context_handle = rp.Pop<u32>();
    async_data->buffer_size = rp.Pop<u32>();

    if (timeout) {
        async_data->timeout_nanos = rp.Pop<u64>();
        LOG_DEBUG(Service_HTTP, "called, handle={}, buffer_size={}, timeout={}",
                  async_data->context_handle, async_data->buffer_size, async_data->timeout_nanos);
    } else {
        LOG_DEBUG(Service_HTTP, "called, handle={}, buffer_size={}", async_data->context_handle,
                  async_data->buffer_size);
    }
    async_data->buffer = &rp.PopMappedBuffer();

    if (!PerformStateChecks(ctx, rp, async_data->context_handle)) {
        return;
    }

    auto context_itr = contexts.find(async_data->context_handle);
    if (context_itr == contexts.end()) {
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
        rb.Push(ErrorContextNotFound);
        LOG_ERROR(Service_HTTP, "ReceiveData on missing context, handle={}",
                  async_data->context_handle);
        return;
    }

    if (!context_itr->second.request_future.valid()) {
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
        rb.Push(ErrorInvalidRequestState);
        LOG_ERROR(Service_HTTP, "ReceiveData before request future exists, handle={}",
                  async_data->context_handle);
        return;
    }

    ctx.RunAsync(
        [this, async_data](Kernel::HLERequestContext& ctx) {
            auto context_itr = contexts.find(async_data->context_handle);
            if (context_itr == contexts.end()) {
                async_data->async_res = ErrorContextNotFound;
                return 0;
            }

            Context& http_context = context_itr->second;
            if (!http_context.request_future.valid()) {
                async_data->async_res = ErrorInvalidRequestState;
                return 0;
            }

            async_data->async_res = http_context.WaitForBodyData(
                async_data->buffer_size, async_data->timeout, async_data->timeout_nanos);
            // Simulate small delay from HTTP receive.
            return 1'000'000;
        },
        [this, async_data](Kernel::HLERequestContext& ctx) {
            IPC::RequestBuilder rb(ctx, static_cast<u16>(ctx.CommandHeader().command_id.Value()), 1,
                                   0);
            if (async_data->async_res != ResultSuccess) {
                rb.Push(async_data->async_res);
                return;
            }

            auto context_itr = contexts.find(async_data->context_handle);
            if (context_itr == contexts.end()) {
                rb.Push(ErrorContextNotFound);
                return;
            }

            Context& http_context = context_itr->second;
            Result result = ResultSuccess;
            std::vector<u8> out = http_context.CopyBodyData(async_data->buffer_size, result);
            if (!out.empty()) {
                async_data->buffer->Write(out.data(), 0, out.size());
            }

            u64 copied = 0;
            u64 received = 0;
            u64 total = 0;
            std::size_t buffered = 0;
            bool complete = false;
            {
                std::lock_guard lock(http_context.response_mutex);
                copied = http_context.current_copied_data;
                received = http_context.current_download_size_bytes.load();
                total = GetResponseTotalSizeLocked(http_context);
                buffered = http_context.response_body_buffer.size();
                complete = http_context.response_complete;
            }

            LOG_INFO(Service_HTTP,
                     "ReceiveData{}: handle={} buffer_size={} wrote={} copied={} received={} "
                     "total={} buffered={} complete={} result={:08X}",
                     async_data->timeout ? "Timeout" : "", async_data->context_handle,
                     async_data->buffer_size, out.size(), copied, received, total, buffered,
                     complete, result.raw);

            rb.Push(result);
        });
}

void HTTP_C::SetProxy(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const Context::Handle context_handle = rp.Pop<u32>();
    const u32 proxy_size = rp.Pop<u32>();
    const u16 port = static_cast<u16>(rp.Pop<u32>() & 0xFFFF);
    const u32 username_size = rp.Pop<u32>();
    const u32 password_size = rp.Pop<u32>();
    const std::vector<u8> proxy_buffer = rp.PopStaticBuffer();
    const std::vector<u8> username_buffer = rp.PopStaticBuffer();
    const std::vector<u8> password_buffer = rp.PopStaticBuffer();

    LOG_INFO(Service_HTTP, "called, handle={} proxy_size={} port={} username_size={} password_size={}",
             context_handle, proxy_size, port, username_size, password_size);

    if (!PerformStateChecks(ctx, rp, context_handle)) {
        return;
    }

    Context& http_context = GetContext(context_handle);
    if (http_context.state != RequestState::NotStarted) {
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
        rb.Push(ErrorInvalidRequestState);
        return;
    }

    http_context.proxy = Context::Proxy{
        .url = StringFromHTTPStaticBuffer(proxy_buffer, proxy_size),
        .username = StringFromHTTPStaticBuffer(username_buffer, username_size),
        .password = StringFromHTTPStaticBuffer(password_buffer, password_size),
        .port = port,
    };

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(ResultSuccess);
}

void HTTP_C::SetProxyDefault(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const Context::Handle context_handle = rp.Pop<u32>();

    LOG_DEBUG(Service_HTTP, "called, handle={}", context_handle);

    if (!PerformStateChecks(ctx, rp, context_handle)) {
        return;
    }

    Context& http_context = GetContext(context_handle);
    if (http_context.state != RequestState::NotStarted) {
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
        rb.Push(ErrorInvalidRequestState);
        return;
    }

    http_context.proxy.reset();

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(ResultSuccess);
}

void HTTP_C::CreateContext(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const u32 url_size = rp.Pop<u32>();
    RequestMethod method = rp.PopEnum<RequestMethod>();
    Kernel::MappedBuffer& buffer = rp.PopMappedBuffer();

    // Copy the buffer into a string without the \0 at the end of the buffer
    std::string url(url_size - 1, '\0');
    buffer.Read(url.data(), 0, url_size - 1);

    LOG_DEBUG(Service_HTTP, "called, url_size={}, url={}, method={}", url_size, url, method);

    auto* session_data = EnsureSessionInitialized(ctx, rp);
    if (!session_data) {
        return;
    }

    // This command can only be called without a bound session.
    if (session_data->current_http_context) {
        LOG_ERROR(Service_HTTP, "Command called with a bound context");

        IPC::RequestBuilder rb = rp.MakeBuilder(1, 2);
        rb.Push(ErrorNotImplemented);
        rb.PushMappedBuffer(buffer);
        return;
    }

    static constexpr std::size_t MaxConcurrentHTTPContexts = 8;
    if (session_data->num_http_contexts >= MaxConcurrentHTTPContexts) {
        // There can only be 8 HTTP contexts open at the same time for any particular session.
        LOG_ERROR(Service_HTTP, "Tried to open too many HTTP contexts");

        IPC::RequestBuilder rb = rp.MakeBuilder(1, 2);
        rb.Push(ErrorTooManyContexts);
        rb.PushMappedBuffer(buffer);
        return;
    }

    if (method == RequestMethod::None || static_cast<u32>(method) >= TotalRequestMethods) {
        LOG_ERROR(Service_HTTP, "invalid request method={}", method);

        IPC::RequestBuilder rb = rp.MakeBuilder(1, 2);
        rb.Push(ErrorInvalidRequestMethod);
        rb.PushMappedBuffer(buffer);
        return;
    }

    contexts.try_emplace(++context_counter);
    contexts[context_counter].url = std::move(url);
    contexts[context_counter].method = method;
    contexts[context_counter].state = RequestState::NotStarted;
    // TODO(Subv): Find a correct default value for this field.
    contexts[context_counter].socket_buffer_size = 0;
    contexts[context_counter].handle = context_counter;
    contexts[context_counter].session_id = session_data->session_id;
    contexts[context_counter].url_replacer = &url_replacer;

    LOG_INFO(Service_HTTP, "CreateContext: handle={} method={} url={}", context_counter, method,
             RedactUrlForLog(contexts[context_counter].url));

    session_data->num_http_contexts++;

    IPC::RequestBuilder rb = rp.MakeBuilder(2, 2);
    rb.Push(ResultSuccess);
    rb.Push<u32>(context_counter);
    rb.PushMappedBuffer(buffer);
}

void HTTP_C::CloseContext(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);

    u32 context_handle = rp.Pop<u32>();

    LOG_DEBUG(Service_HTTP, "called, handle={}", context_handle);

    auto* session_data = EnsureSessionInitialized(ctx, rp);
    if (!session_data) {
        return;
    }

    ASSERT_MSG(!session_data->current_http_context,
               "Unimplemented CloseContext on context-bound session");

    auto itr = contexts.find(context_handle);
    if (itr == contexts.end()) {
        // The real HTTP module just silently fails in this case.
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
        rb.Push(ResultSuccess);
        LOG_ERROR(Service_HTTP, "called, context {} not found", context_handle);
        return;
    }

    // TODO(Subv): What happens if you try to close a context that's currently being used?
    // TODO(Subv): Make sure that only the session that created the context can close it.

    Context& http_context = itr->second;
    if (http_context.request_future.valid()) {
        const bool request_complete = IsRequestComplete(http_context);
        if (!request_complete) {
            http_context.CancelRequest();
        }
        LOG_INFO(Service_HTTP, "CloseContext: handle={} state={} request_complete={}",
                 context_handle, static_cast<u32>(http_context.state.load()), request_complete);
        http_context.request_future.wait();
    }

    contexts.erase(itr);
    if (session_data->num_http_contexts > 0) {
        session_data->num_http_contexts--;
    }

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(ResultSuccess);
}

void HTTP_C::CancelConnection(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const u32 context_handle = rp.Pop<u32>();

    const auto* session_data = EnsureSessionInitialized(ctx, rp);
    if (!session_data) {
        return;
    }

    auto itr = contexts.find(context_handle);
    if (itr == contexts.end()) {
        LOG_WARNING(Service_HTTP, "CancelConnection on missing context, handle={}", context_handle);
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
        rb.Push(ResultSuccess);
        return;
    }

    Context& http_context = itr->second;
    const bool future_valid = http_context.request_future.valid();
    const bool request_complete = IsRequestComplete(http_context);
    http_context.CancelRequest();
    if (!future_valid || request_complete) {
        http_context.state = RequestState::Completed;
    }

    LOG_INFO(Service_HTTP,
             "CancelConnection: handle={} state={} future_valid={} request_complete={}",
             context_handle, static_cast<u32>(http_context.state.load()), future_valid,
             request_complete);

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(ResultSuccess);
}

void HTTP_C::GetRequestState(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const u32 context_handle = rp.Pop<u32>();

    const auto* session_data = EnsureSessionInitialized(ctx, rp);
    if (!session_data) {
        return;
    }

    Context& http_context = GetContext(context_handle);
    RequestState state = http_context.state;

    // When POST data is pending to be set, HTTPC stays in the SendingRequest
    // state until NotifyFinishSendPostData is called. Most likely HTTPC
    // already started the HTTP request at this point, has send the headers
    // and is waiting for the client to set the post body to send.
    // We cannot do that with httplib so instead fake the state to SendingRequest
    // if post data is pending. TODO(PabloMK7): Fix if we get a more
    // flexible HTTP library.
    if (state == RequestState::NotStarted && http_context.post_pending_request) {
        state = RequestState::SendingRequest;
    }

    LOG_DEBUG(Service_HTTP, "called, context_handle={} state={}", context_handle, state);

    IPC::RequestBuilder rb = rp.MakeBuilder(2, 0);
    rb.Push(ResultSuccess);
    rb.PushEnum<RequestState>(state);
}

void HTTP_C::AddRequestHeader(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const u32 context_handle = rp.Pop<u32>();
    [[maybe_unused]] const u32 name_size = rp.Pop<u32>();
    const u32 value_size = rp.Pop<u32>();
    const std::vector<u8> name_buffer = rp.PopStaticBuffer();
    Kernel::MappedBuffer& value_buffer = rp.PopMappedBuffer();

    // Copy the name_buffer into a string without the \0 at the end
    const std::string name(name_buffer.begin(), name_buffer.end() - 1);

    // Copy the value_buffer into a string without the \0 at the end
    std::string value(value_size - 1, '\0');
    value_buffer.Read(&value[0], 0, value_size - 1);

    LOG_DEBUG(Service_HTTP, "called, name={}, value={}, context_handle={}", name, value,
              context_handle);

    if (!PerformStateChecks(ctx, rp, context_handle)) {
        return;
    }

    Context& http_context = GetContext(context_handle);

    if (http_context.state != RequestState::NotStarted) {
        LOG_ERROR(Service_HTTP,
                  "Tried to add a request header on a context that has already been started.");
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 2);
        rb.Push(ErrorInvalidRequestState);
        rb.PushMappedBuffer(value_buffer);
        return;
    }

    http_context.headers.emplace_back(name, value);

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 2);
    rb.Push(ResultSuccess);
    rb.PushMappedBuffer(value_buffer);
}

void HTTP_C::AddPostDataAscii(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const u32 context_handle = rp.Pop<u32>();
    [[maybe_unused]] const u32 name_size = rp.Pop<u32>();
    const u32 value_size = rp.Pop<u32>();
    const std::vector<u8> name_buffer = rp.PopStaticBuffer();
    Kernel::MappedBuffer& value_buffer = rp.PopMappedBuffer();

    // Copy the name_buffer into a string without the \0 at the end
    const std::string name(name_buffer.begin(), name_buffer.end() - 1);

    // Copy the value_buffer into a string without the \0 at the end
    std::string value(value_size - 1, '\0');
    value_buffer.Read(value.data(), 0, value_size - 1);

    LOG_DEBUG(Service_HTTP, "called, name={}, value={}, context_handle={}", name, value,
              context_handle);

    if (!PerformStateChecks(ctx, rp, context_handle)) {
        return;
    }

    Context& http_context = GetContext(context_handle);

    if (http_context.state != RequestState::NotStarted) {
        LOG_ERROR(Service_HTTP,
                  "Tried to add Post data on a context that has already been started");
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 2);
        rb.Push(ErrorInvalidRequestState);
        rb.PushMappedBuffer(value_buffer);
        return;
    }

    if (!http_context.post_data_raw.empty()) {
        LOG_ERROR(Service_HTTP, "Cannot add ASCII Post data to context with raw Post data");
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 2);
        rb.Push(ErrorIncompatibleAddPostData);
        rb.PushMappedBuffer(value_buffer);
        return;
    }

    if (http_context.chunked_request) {
        LOG_ERROR(Service_HTTP, "Cannot add ASCII Post data to context in chunked request mode");
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 2);
        rb.Push(ErrorIncompatibleAddPostData);
        rb.PushMappedBuffer(value_buffer);
        return;
    }

    Context::Param param_value(name, value);
    http_context.post_data.emplace(name, param_value);
    http_context.post_data_added = true;

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 2);
    rb.Push(ResultSuccess);
    rb.PushMappedBuffer(value_buffer);
}

void HTTP_C::AddPostDataBinary(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const u32 context_handle = rp.Pop<u32>();
    [[maybe_unused]] const u32 name_size = rp.Pop<u32>();
    const u32 value_size = rp.Pop<u32>();
    const std::vector<u8> name_buffer = rp.PopStaticBuffer();
    Kernel::MappedBuffer& value_buffer = rp.PopMappedBuffer();

    // Copy the name_buffer into a string without the \0 at the end
    const std::string name(name_buffer.begin(), name_buffer.end() - 1);

    // Copy the value_buffer into a vector
    std::vector<u8> value(value_size);
    value_buffer.Read(value.data(), 0, value_size);

    LOG_DEBUG(Service_HTTP, "called, name={}, value_size={}, context_handle={}", name, value_size,
              context_handle);

    if (!PerformStateChecks(ctx, rp, context_handle)) {
        return;
    }

    Context& http_context = GetContext(context_handle);

    if (http_context.state != RequestState::NotStarted) {
        LOG_ERROR(Service_HTTP,
                  "Tried to add Post data on a context that has already been started");
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 2);
        rb.Push(ErrorInvalidRequestState);
        rb.PushMappedBuffer(value_buffer);
        return;
    }

    if (!http_context.post_data_raw.empty()) {
        LOG_ERROR(Service_HTTP, "Cannot add Binary Post data to context with raw Post data");
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 2);
        rb.Push(ErrorIncompatibleAddPostData);
        rb.PushMappedBuffer(value_buffer);
        return;
    }

    if (http_context.chunked_request) {
        LOG_ERROR(Service_HTTP, "Cannot add Binary Post data to context in chunked request mode");
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 2);
        rb.Push(ErrorIncompatibleAddPostData);
        rb.PushMappedBuffer(value_buffer);
        return;
    }

    Context::Param param_value(name, value);
    http_context.post_data.emplace(name, param_value);
    http_context.force_multipart = true;
    http_context.post_data_added = true;

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 2);
    rb.Push(ResultSuccess);
    rb.PushMappedBuffer(value_buffer);
}

void HTTP_C::AddPostDataRaw(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const u32 context_handle = rp.Pop<u32>();
    const u32 post_data_len = rp.Pop<u32>();
    auto buffer = rp.PopMappedBuffer();

    LOG_DEBUG(Service_HTTP, "called, context_handle={}, post_data_len={}", context_handle,
              post_data_len);

    if (!PerformStateChecks(ctx, rp, context_handle)) {
        return;
    }

    Context& http_context = GetContext(context_handle);

    if (http_context.state != RequestState::NotStarted) {
        LOG_ERROR(Service_HTTP,
                  "Tried to add Post data on a context that has already been started");
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 2);
        rb.Push(ErrorInvalidRequestState);
        rb.PushMappedBuffer(buffer);
        return;
    }

    if (!http_context.post_data.empty()) {
        LOG_ERROR(Service_HTTP,
                  "Cannot add raw Post data to context with ASCII or Binary Post data");
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 2);
        rb.Push(ErrorIncompatibleAddPostData);
        rb.PushMappedBuffer(buffer);
        return;
    }

    if (http_context.chunked_request) {
        LOG_ERROR(Service_HTTP, "Cannot add raw Post data to context in chunked request mode");
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 2);
        rb.Push(ErrorIncompatibleAddPostData);
        rb.PushMappedBuffer(buffer);
        return;
    }

    http_context.post_data_raw.resize(buffer.GetSize());
    buffer.Read(http_context.post_data_raw.data(), 0, buffer.GetSize());
    http_context.post_data_added = true;

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 2);
    rb.Push(ResultSuccess);
    rb.PushMappedBuffer(buffer);
}

void HTTP_C::SetPostDataType(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const u32 context_handle = rp.Pop<u32>();
    const PostDataType type = rp.PopEnum<PostDataType>();

    LOG_DEBUG(Service_HTTP, "called, context_handle={}, type={}", context_handle, type);

    if (!PerformStateChecks(ctx, rp, context_handle)) {
        return;
    }

    Context& http_context = GetContext(context_handle);

    if (http_context.state != RequestState::NotStarted) {
        LOG_ERROR(Service_HTTP,
                  "Tried to set chunked mode on a context that has already been started");
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
        rb.Push(ErrorInvalidRequestState);
        return;
    }

    if (!http_context.post_data.empty() || !http_context.post_data_raw.empty()) {
        LOG_ERROR(Service_HTTP, "Tried to set chunked mode on a context that has Post data");
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
        rb.Push(ErrorIncompatibleSendPostData);
        return;
    }

    switch (type) {
    case PostDataType::AsciiForm:
    case PostDataType::MultipartForm:
    case PostDataType::Raw:
        http_context.post_data_type = type;
        break;
    // Use ASCII form by default
    default:
        http_context.post_data_type = PostDataType::AsciiForm;
        break;
    }

    http_context.chunked_request = true;

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(ResultSuccess);
}

void HTTP_C::SendPostDataAscii(Kernel::HLERequestContext& ctx) {
    SendPostDataAsciiImpl(ctx, false);
}

void HTTP_C::SendPostDataAsciiTimeout(Kernel::HLERequestContext& ctx) {
    SendPostDataAsciiImpl(ctx, true);
}

void HTTP_C::SendPostDataAsciiImpl(Kernel::HLERequestContext& ctx, bool timeout) {
    IPC::RequestParser rp(ctx);
    const u32 context_handle = rp.Pop<u32>();
    [[maybe_unused]] const u32 name_size = rp.Pop<u32>();
    const u32 value_size = rp.Pop<u32>();
    // TODO(DaniElectra): The original module waits until a connection with the server is made
    const u64 timeout_nanos = timeout ? rp.Pop<u64>() : 0;
    const std::vector<u8> name_buffer = rp.PopStaticBuffer();
    Kernel::MappedBuffer& value_buffer = rp.PopMappedBuffer();

    // Copy the name_buffer into a string without the \0 at the end
    const std::string name(name_buffer.begin(), name_buffer.end() - 1);

    // Copy the value_buffer into a string without the \0 at the end
    std::string value(value_size - 1, '\0');
    value_buffer.Read(value.data(), 0, value_size - 1);

    if (timeout) {
        LOG_DEBUG(Service_HTTP, "called, name={}, value={}, context_handle={}, timeout={}", name,
                  value, context_handle, timeout_nanos);
    } else {
        LOG_DEBUG(Service_HTTP, "called, name={}, value={}, context_handle={}", name, value,
                  context_handle);
    }

    if (!PerformStateChecks(ctx, rp, context_handle)) {
        return;
    }

    Context& http_context = GetContext(context_handle);

    if (http_context.state != RequestState::NotStarted) {
        LOG_ERROR(Service_HTTP, "Tried to send Post data on a context that has been started");
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 2);
        rb.Push(ErrorInvalidRequestState);
        rb.PushMappedBuffer(value_buffer);
        return;
    }

    if (!http_context.chunked_request) {
        LOG_ERROR(Service_HTTP,
                  "Cannot send ASCII Post data to context not in chunked request mode");
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 2);
        rb.Push(ErrorInvalidRequestState);
        rb.PushMappedBuffer(value_buffer);
        return;
    }

    Context::Param param_value(name, value);
    http_context.post_data.emplace(name, param_value);
    http_context.post_data_added = true;

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 2);
    rb.Push(ResultSuccess);
    rb.PushMappedBuffer(value_buffer);
}

void HTTP_C::SendPostDataBinary(Kernel::HLERequestContext& ctx) {
    SendPostDataBinaryImpl(ctx, false);
}

void HTTP_C::SendPostDataBinaryTimeout(Kernel::HLERequestContext& ctx) {
    SendPostDataBinaryImpl(ctx, true);
}

void HTTP_C::SendPostDataBinaryImpl(Kernel::HLERequestContext& ctx, bool timeout) {
    IPC::RequestParser rp(ctx);
    const u32 context_handle = rp.Pop<u32>();
    [[maybe_unused]] const u32 name_size = rp.Pop<u32>();
    const u32 value_size = rp.Pop<u32>();
    // TODO(DaniElectra): The original module waits until a connection with the server is made
    const u64 timeout_nanos = timeout ? rp.Pop<u64>() : 0;
    const std::vector<u8> name_buffer = rp.PopStaticBuffer();
    Kernel::MappedBuffer& value_buffer = rp.PopMappedBuffer();

    // Copy the name_buffer into a string without the \0 at the end
    const std::string name(name_buffer.begin(), name_buffer.end() - 1);

    // Copy the value_buffer into a vector
    std::vector<u8> value(value_size);
    value_buffer.Read(value.data(), 0, value_size);

    if (timeout) {
        LOG_DEBUG(Service_HTTP, "called, name={}, value_size={}, context_handle={}, timeout={}",
                  name, value_size, context_handle, timeout_nanos);
    } else {
        LOG_DEBUG(Service_HTTP, "called, name={}, value_size={}, context_handle={}", name,
                  value_size, context_handle);
    }

    if (!PerformStateChecks(ctx, rp, context_handle)) {
        return;
    }

    Context& http_context = GetContext(context_handle);

    if (http_context.state == RequestState::NotStarted) {
        LOG_ERROR(Service_HTTP, "Tried to add Post data on a context that has not been started");
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 2);
        rb.Push(ErrorInvalidRequestState);
        rb.PushMappedBuffer(value_buffer);
        return;
    }

    if (!http_context.chunked_request) {
        LOG_ERROR(Service_HTTP,
                  "Cannot send Binary Post data to context not in chunked request mode");
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 2);
        rb.Push(ErrorInvalidRequestState);
        rb.PushMappedBuffer(value_buffer);
        return;
    }

    Context::Param param_value(name, value);
    http_context.post_data.emplace(name, param_value);
    http_context.post_data_added = true;

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 2);
    rb.Push(ResultSuccess);
    rb.PushMappedBuffer(value_buffer);
}

void HTTP_C::SendPostDataRaw(Kernel::HLERequestContext& ctx) {
    SendPostDataRawImpl(ctx, false);
}

void HTTP_C::SendPostDataRawTimeout(Kernel::HLERequestContext& ctx) {
    SendPostDataRawImpl(ctx, true);
}

void HTTP_C::SendPostDataRawImpl(Kernel::HLERequestContext& ctx, bool timeout) {
    IPC::RequestParser rp(ctx);
    const u32 context_handle = rp.Pop<u32>();
    const u32 post_data_len = rp.Pop<u32>();
    // TODO(DaniElectra): The original module waits until a connection with the server is made
    const u64 timeout_nanos = timeout ? rp.Pop<u64>() : 0;
    auto buffer = rp.PopMappedBuffer();

    if (timeout) {
        LOG_DEBUG(Service_HTTP, "called, context_handle={}, post_data_len={}, timeout={}",
                  context_handle, post_data_len, timeout_nanos);
    } else {
        LOG_DEBUG(Service_HTTP, "called, context_handle={}, post_data_len={}", context_handle,
                  post_data_len);
    }

    if (!PerformStateChecks(ctx, rp, context_handle)) {
        return;
    }

    Context& http_context = GetContext(context_handle);

    if (http_context.state != RequestState::NotStarted) {
        LOG_ERROR(Service_HTTP,
                  "Tried to add Post data on a context that has already been started");
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 2);
        rb.Push(ErrorInvalidRequestState);
        rb.PushMappedBuffer(buffer);
        return;
    }

    if (!http_context.chunked_request) {
        LOG_ERROR(Service_HTTP, "Cannot send raw Post data to context not in chunked request mode");
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 2);
        rb.Push(ErrorInvalidRequestState);
        rb.PushMappedBuffer(buffer);
        return;
    }

    if (http_context.post_data_type != PostDataType::Raw) {
        LOG_ERROR(Service_HTTP, "Cannot send raw Post data to context not in raw mode");
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 2);
        rb.Push(ErrorIncompatibleSendPostData);
        rb.PushMappedBuffer(buffer);
        return;
    }

    std::vector<u8> value(buffer.GetSize());
    buffer.Read(value.data(), 0, value.size());

    // Workaround for sending the raw data in combination of other data in chunked requests
    Context::Param raw_param(value);
    std::string value_string(value.begin(), value.end());
    http_context.post_data.emplace(value_string, raw_param);
    http_context.post_data_added = true;

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 2);
    rb.Push(ResultSuccess);
    rb.PushMappedBuffer(buffer);
}

void HTTP_C::SetPostDataEncoding(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const u32 context_handle = rp.Pop<u32>();
    const PostDataEncoding encoding = rp.PopEnum<PostDataEncoding>();

    LOG_DEBUG(Service_HTTP, "called, context_handle={}, encoding={}", context_handle, encoding);

    if (!PerformStateChecks(ctx, rp, context_handle)) {
        return;
    }

    Context& http_context = GetContext(context_handle);

    if (http_context.state != RequestState::NotStarted) {
        LOG_ERROR(Service_HTTP,
                  "Tried to set Post encoding on a context that has already been started");
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
        rb.Push(ErrorInvalidRequestState);
        return;
    }

    switch (encoding) {
    case PostDataEncoding::Auto:
        http_context.post_data_encoding = encoding;
        break;
    case PostDataEncoding::AsciiForm:
    case PostDataEncoding::MultipartForm:
        if (!http_context.post_data_raw.empty()) {
            LOG_ERROR(Service_HTTP, "Cannot set Post data encoding to context with raw Post data");
            IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
            rb.Push(ErrorIncompatibleAddPostData);
            return;
        }

        http_context.post_data_encoding = encoding;
        break;
    default:
        LOG_ERROR(Service_HTTP, "Invalid Post data encoding: {}", encoding);
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
        rb.Push(ErrorInvalidPostDataEncoding);
        return;
    }

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(ResultSuccess);
}

void HTTP_C::NotifyFinishSendPostData(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const u32 context_handle = rp.Pop<u32>();

    LOG_DEBUG(Service_HTTP, "called, context_handle={}", context_handle);

    if (!PerformStateChecks(ctx, rp, context_handle)) {
        return;
    }

    Context& http_context = GetContext(context_handle);

    if (http_context.state != RequestState::NotStarted) {
        LOG_ERROR(Service_HTTP, "Tried to notfy finish Post on a context that has been started");
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
        rb.Push(ErrorInvalidRequestState);
        return;
    }

    if (!http_context.post_pending_request) {
        LOG_ERROR(Service_HTTP, "Tried to notfy finish Post on a context that has not begun");
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
        rb.Push(ErrorInvalidRequestState);
        return;
    }

    if (!http_context.post_data_added) {
        LOG_ERROR(Service_HTTP, "Tried to notfy finish Post on a context that has no post data");
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
        rb.Push(ErrorInvalidRequestState);
        return;
    }

    http_context.finish_post_data.Set();

    PrepareContextForRequest(http_context);
    http_context.request_future =
        std::async(std::launch::async, &Context::MakeRequest, std::ref(http_context));

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(ResultSuccess);
}

void HTTP_C::GetResponseData(Kernel::HLERequestContext& ctx) {
    GetResponseDataImpl(ctx, false);
}

void HTTP_C::GetResponseDataTimeout(Kernel::HLERequestContext& ctx) {
    GetResponseDataImpl(ctx, true);
}

void HTTP_C::GetResponseDataImpl(Kernel::HLERequestContext& ctx, bool timeout) {
    IPC::RequestParser rp(ctx);

    struct AsyncData {
        // Input
        u32 context_handle;
        bool timeout;
        u64 timeout_nanos;
        u32 data_max_len;
        Kernel::MappedBuffer* data_buffer;
        // Output
        Result async_res = ResultSuccess;
    };
    std::shared_ptr<AsyncData> async_data = std::make_shared<AsyncData>();

    async_data->timeout = timeout;
    async_data->context_handle = rp.Pop<u32>();
    async_data->data_max_len = rp.Pop<u32>();
    if (timeout) {
        async_data->timeout_nanos = rp.Pop<u64>();
    }
    async_data->data_buffer = &rp.PopMappedBuffer();

    if (!PerformStateChecks(ctx, rp, async_data->context_handle)) {
        return;
    }

    ctx.RunAsync(
        [this, async_data](Kernel::HLERequestContext& ctx) {
            Context& http_context = GetContext(async_data->context_handle);
            if (!http_context.request_future.valid()) {
                async_data->async_res = ErrorInvalidRequestState;
                return 0;
            }

            async_data->async_res = http_context.WaitForResponseHeaders(
                async_data->timeout, async_data->timeout_nanos);

            return 0;
        },
        [this, async_data](Kernel::HLERequestContext& ctx) {
            IPC::RequestBuilder rb(ctx, 2, 0);
            if (async_data->async_res != ResultSuccess) {
                rb.Push(async_data->async_res);
                return;
            }

            Context& http_context = GetContext(async_data->context_handle);
            auto& headers = http_context.response.headers;
            std::vector<u8> out;

            if (async_data->timeout) {
                LOG_DEBUG(Service_HTTP, "timeout={}", async_data->timeout_nanos);
            } else {
                LOG_DEBUG(Service_HTTP, "");
            }

            // httplib does not keep the raw HTTP header data, so we need to reconstruct it.
            // Sadly, the order of headers is lost, but for now it's good enough.
            std::string hdr =
                fmt::format("{} {} {}\r\n", http_context.response.version,
                            http_context.response.status, http_context.response.reason);
            out.insert(out.end(), hdr.begin(), hdr.end());

            for (auto& h : headers) {
                hdr = fmt::format("{}: {}\r\n", h.first, h.second);
                out.insert(out.end(), hdr.begin(), hdr.end());
            }

            hdr = "\r\n";
            out.insert(out.end(), hdr.begin(), hdr.end());

            size_t write_size = std::min(out.size(), async_data->data_buffer->GetSize());
            async_data->data_buffer->Write(out.data(), 0, write_size);

            rb.Push(ResultSuccess);
            rb.Push(static_cast<u32>(write_size));
        });
}

void HTTP_C::GetResponseHeader(Kernel::HLERequestContext& ctx) {
    GetResponseHeaderImpl(ctx, false);
}

void HTTP_C::GetResponseHeaderTimeout(Kernel::HLERequestContext& ctx) {
    GetResponseHeaderImpl(ctx, true);
}

void HTTP_C::GetResponseHeaderImpl(Kernel::HLERequestContext& ctx, bool timeout) {
    IPC::RequestParser rp(ctx);

    struct AsyncData {
        // Input
        u32 context_handle;
        u32 name_len;
        u32 value_max_len;
        bool timeout;
        u64 timeout_nanos;
        std::span<const u8> header_name;
        Kernel::MappedBuffer* value_buffer;
        // Output
        Result async_res = ResultSuccess;
    };
    std::shared_ptr<AsyncData> async_data = std::make_shared<AsyncData>();

    async_data->timeout = timeout;
    async_data->context_handle = rp.Pop<u32>();
    async_data->name_len = rp.Pop<u32>();
    async_data->value_max_len = rp.Pop<u32>();
    if (timeout) {
        async_data->timeout_nanos = rp.Pop<u64>();
    }
    async_data->header_name = rp.PopStaticBuffer();
    async_data->value_buffer = &rp.PopMappedBuffer();

    if (!PerformStateChecks(ctx, rp, async_data->context_handle)) {
        return;
    }

    ctx.RunAsync(
        [this, async_data](Kernel::HLERequestContext& ctx) {
            Context& http_context = GetContext(async_data->context_handle);
            if (!http_context.request_future.valid()) {
                async_data->async_res = ErrorInvalidRequestState;
                return 0;
            }

            async_data->async_res = http_context.WaitForResponseHeaders(
                async_data->timeout, async_data->timeout_nanos);

            return 0;
        },
        [this, async_data](Kernel::HLERequestContext& ctx) {
            IPC::RequestBuilder rb(ctx, static_cast<u16>(ctx.CommandHeader().command_id.Value()), 2,
                                   2);
            if (async_data->async_res != ResultSuccess) {
                rb.Push(async_data->async_res);
                return;
            }

            std::string header_name_str(
                reinterpret_cast<const char*>(async_data->header_name.data()),
                async_data->name_len);
            Common::TruncateString(header_name_str);

            Context& http_context = GetContext(async_data->context_handle);

            auto& headers = http_context.response.headers;
            u32 copied_size = 0;

            if (async_data->timeout) {
                LOG_DEBUG(Service_HTTP, "header={}, max_len={}, timeout={}", header_name_str,
                          async_data->value_buffer->GetSize(), async_data->timeout_nanos);
            } else {
                LOG_DEBUG(Service_HTTP, "header={}, max_len={}", header_name_str,
                          async_data->value_buffer->GetSize());
            }

            const auto header = FindHeaderCaseInsensitive(headers, header_name_str);
            if (header != headers.end()) {
                std::string header_value = header->second;
                copied_size = static_cast<u32>(header_value.size());
                if (header_value.size() >= async_data->value_buffer->GetSize()) {
                    header_value.resize(async_data->value_buffer->GetSize() - 1);
                }
                header_value.push_back('\0');
                async_data->value_buffer->Write(header_value.data(), 0, header_value.size());
                LOG_INFO(Service_HTTP,
                         "GetResponseHeader: handle={} requested={} matched={} value_size={} status={}",
                         async_data->context_handle, header_name_str, header->first, copied_size,
                         http_context.response.status);
            } else {
                LOG_INFO(Service_HTTP,
                         "GetResponseHeader: handle={} requested={} not found status={} headers={}",
                         async_data->context_handle, header_name_str, http_context.response.status,
                         headers.size());
                rb.Push(ErrorHeaderNotFound);
                rb.Push(0);
                rb.PushMappedBuffer(*async_data->value_buffer);
                return;
            }
            rb.Push(ResultSuccess);
            rb.Push(copied_size);
            rb.PushMappedBuffer(*async_data->value_buffer);
        });
}

void HTTP_C::GetResponseStatusCode(Kernel::HLERequestContext& ctx) {
    GetResponseStatusCodeImpl(ctx, false);
}

void HTTP_C::GetResponseStatusCodeTimeout(Kernel::HLERequestContext& ctx) {
    GetResponseStatusCodeImpl(ctx, true);
}

void HTTP_C::GetResponseStatusCodeImpl(Kernel::HLERequestContext& ctx, bool timeout) {
    IPC::RequestParser rp(ctx);

    struct AsyncData {
        // Input
        Context::Handle context_handle;
        bool timeout;
        u64 timeout_nanos = 0;
        // Output
        Result async_res = ResultSuccess;
    };
    std::shared_ptr<AsyncData> async_data = std::make_shared<AsyncData>();

    async_data->context_handle = rp.Pop<u32>();
    async_data->timeout = timeout;

    if (timeout) {
        async_data->timeout_nanos = rp.Pop<u64>();
        LOG_INFO(Service_HTTP, "called, timeout={}", async_data->timeout_nanos);
    } else {
        LOG_INFO(Service_HTTP, "called");
    }

    if (!PerformStateChecks(ctx, rp, async_data->context_handle)) {
        return;
    }

    ctx.RunAsync(
        [this, async_data](Kernel::HLERequestContext& ctx) {
            Context& http_context = GetContext(async_data->context_handle);
            if (!http_context.request_future.valid()) {
                async_data->async_res = ErrorInvalidRequestState;
                return 0;
            }

            async_data->async_res = http_context.WaitForResponseHeaders(
                async_data->timeout, async_data->timeout_nanos);
            return 0;
        },
        [this, async_data](Kernel::HLERequestContext& ctx) {
            if (async_data->async_res != ResultSuccess) {
                IPC::RequestBuilder rb(
                    ctx, static_cast<u16>(ctx.CommandHeader().command_id.Value()), 1, 0);
                rb.Push(async_data->async_res);
                return;
            }

            Context& http_context = GetContext(async_data->context_handle);

            u32 response_code = 0;
            std::string reason;
            std::size_t mirrored_body = 0;
            std::size_t buffered_body = 0;
            bool complete = false;
            {
                std::lock_guard lock(http_context.response_mutex);
                response_code = http_context.response.status > 0
                                    ? static_cast<u32>(http_context.response.status)
                                    : 0;
                reason = http_context.response.reason;
                mirrored_body = http_context.response.body.size();
                buffered_body = http_context.response_body_buffer.size();
                complete = http_context.response_complete;
            }
            LOG_INFO(Service_HTTP,
                     "GetResponseStatusCode{}: handle={} status={} reason={} mirrored_body={} "
                     "buffered_body={} complete={} url={}",
                     async_data->timeout ? "Timeout" : "", async_data->context_handle,
                     response_code, reason, mirrored_body, buffered_body, complete,
                     RedactUrlForLog(http_context.url));

            IPC::RequestBuilder rb(ctx, static_cast<u16>(ctx.CommandHeader().command_id.Value()), 2,
                                   0);
            rb.Push(ResultSuccess);
            rb.Push(response_code);
        });
}

void HTTP_C::AddTrustedRootCA(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const Context::Handle context_handle = rp.Pop<u32>();
    [[maybe_unused]] const u32 root_ca_len = rp.Pop<u32>();
    auto root_ca_data = rp.PopMappedBuffer();

    LOG_WARNING(Service_HTTP, "(STUBBED) called, handle={}", context_handle);

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 2);
    rb.Push(ResultSuccess);
    rb.PushMappedBuffer(root_ca_data);
}

void HTTP_C::AddDefaultCert(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const Context::Handle context_handle = rp.Pop<u32>();
    const u32 certificate_id = rp.Pop<u32>();

    LOG_WARNING(Service_HTTP, "(STUBBED) called, handle={}, certificate_id={}", context_handle,
                certificate_id);

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(ResultSuccess);
}

void HTTP_C::SelectRootCertChain(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const Context::Handle context_handle = rp.Pop<u32>();
    const RootCertChain::Handle root_cert_chain_handle = rp.Pop<u32>();

    LOG_DEBUG(Service_HTTP, "called, context_handle={} root_cert_chain_handle={}", context_handle,
              root_cert_chain_handle);

    if (!PerformStateChecks(ctx, rp, context_handle)) {
        return;
    }

    Context& http_context = GetContext(context_handle);
    if (http_context.state != RequestState::NotStarted) {
        LOG_ERROR(Service_HTTP,
                  "Tried to select a root cert chain on a context that has already started");
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
        rb.Push(ErrorInvalidRequestState);
        return;
    }

    const auto root_chain = root_cert_chains.find(root_cert_chain_handle);
    if (root_chain == root_cert_chains.end()) {
        LOG_ERROR(Service_HTTP, "called with wrong root_cert_chain_handle {}",
                  root_cert_chain_handle);
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
        rb.Push(ErrorWrongCertHandle);
        return;
    }

    http_context.ssl_config.root_ca_chain = root_chain->second;

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(ResultSuccess);
}

void HTTP_C::CreateRootCertChain(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);

    auto* session_data = GetSessionData(ctx.Session());
    ASSERT(session_data);

    Result result = ResultSuccess;
    RootCertChain::Handle root_cert_chain_handle = 0;

    if (!session_data->initialized) {
        LOG_ERROR(Service_HTTP, "Command called without Initialize");
        result = ErrorStateError;
    } else {
        auto root_cert_chain = std::make_shared<RootCertChain>();
        root_cert_chain_handle = ++root_cert_chain_counter;
        root_cert_chain->handle = root_cert_chain_handle;
        root_cert_chain->session_id = session_data->session_id;
        root_cert_chains[root_cert_chain_handle] = std::move(root_cert_chain);
    }

    LOG_DEBUG(Service_HTTP, "called, root_cert_chain_handle={} result={:#010X}",
              root_cert_chain_handle, result.raw);

    IPC::RequestBuilder rb = rp.MakeBuilder(2, 0);
    rb.Push(result);
    rb.Push(root_cert_chain_handle);
}

void HTTP_C::DestroyRootCertChain(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const RootCertChain::Handle root_cert_chain_handle = rp.Pop<u32>();

    auto* session_data = GetSessionData(ctx.Session());
    ASSERT(session_data);

    Result result = ResultSuccess;
    const auto root_chain = root_cert_chains.find(root_cert_chain_handle);
    if (!session_data->initialized) {
        LOG_ERROR(Service_HTTP, "Command called without Initialize");
        result = ErrorStateError;
    } else if (root_chain == root_cert_chains.end() ||
               root_chain->second->session_id != session_data->session_id) {
        LOG_ERROR(Service_HTTP, "called with wrong root_cert_chain_handle {}",
                  root_cert_chain_handle);
        result = ErrorWrongCertHandle;
    } else {
        for (auto& context : contexts) {
            if (auto selected_chain = context.second.ssl_config.root_ca_chain.lock();
                selected_chain && selected_chain->handle == root_cert_chain_handle) {
                context.second.ssl_config.root_ca_chain = {};
            }
        }
        root_cert_chains.erase(root_chain);
    }

    LOG_DEBUG(Service_HTTP, "called, root_cert_chain_handle={} result={:#010X}",
              root_cert_chain_handle, result.raw);

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(result);
}

void HTTP_C::RootCertChainAddCert(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const RootCertChain::Handle root_cert_chain_handle = rp.Pop<u32>();
    const u32 cert_size = rp.Pop<u32>();
    auto& cert_buffer = rp.PopMappedBuffer();

    auto* session_data = GetSessionData(ctx.Session());
    ASSERT(session_data);

    Result result = ResultSuccess;
    RootCertChain::RootCACert::Handle cert_handle = 0;

    const auto root_chain = root_cert_chains.find(root_cert_chain_handle);
    if (!session_data->initialized) {
        LOG_ERROR(Service_HTTP, "Command called without Initialize");
        result = ErrorStateError;
    } else if (root_chain == root_cert_chains.end() ||
               root_chain->second->session_id != session_data->session_id) {
        LOG_ERROR(Service_HTTP, "called with wrong root_cert_chain_handle {}",
                  root_cert_chain_handle);
        result = ErrorWrongCertHandle;
    } else {
        RootCertChain::RootCACert root_ca_cert;
        cert_handle = ++root_certs_counter;
        root_ca_cert.handle = cert_handle;
        root_ca_cert.session_id = session_data->session_id;
        root_ca_cert.certificate.resize(
            std::min<std::size_t>(cert_size, cert_buffer.GetSize()));
        if (!root_ca_cert.certificate.empty()) {
            cert_buffer.Read(root_ca_cert.certificate.data(), 0, root_ca_cert.certificate.size());
        }
        if (root_ca_cert.certificate.size() != cert_size) {
            LOG_WARNING(Service_HTTP,
                        "Root cert input shorter than advertised: handle={} cert_size={} buffer_size={}",
                        root_cert_chain_handle, cert_size, cert_buffer.GetSize());
        }
        root_chain->second->certificates.push_back(std::move(root_ca_cert));
    }

    LOG_DEBUG(Service_HTTP,
              "called, root_cert_chain_handle={} cert_size={} cert_handle={} result={:#010X}",
              root_cert_chain_handle, cert_size, cert_handle, result.raw);

    IPC::RequestBuilder rb = rp.MakeBuilder(2, 2);
    rb.Push(result);
    rb.Push(cert_handle);
    rb.PushMappedBuffer(cert_buffer);
}

void HTTP_C::RootCertChainAddDefaultCert(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const RootCertChain::Handle root_cert_chain_handle = rp.Pop<u32>();
    const u32 certificate_id = rp.Pop<u32>();

    auto* session_data = GetSessionData(ctx.Session());
    ASSERT(session_data);

    Result result = ResultSuccess;
    RootCertChain::RootCACert::Handle cert_handle = 0;

    const auto root_chain = root_cert_chains.find(root_cert_chain_handle);
    if (!session_data->initialized) {
        LOG_ERROR(Service_HTTP, "Command called without Initialize");
        result = ErrorStateError;
    } else if (root_chain == root_cert_chains.end() ||
               root_chain->second->session_id != session_data->session_id) {
        LOG_ERROR(Service_HTTP, "called with wrong root_cert_chain_handle {}",
                  root_cert_chain_handle);
        result = ErrorWrongCertHandle;
    } else {
        RootCertChain::RootCACert root_ca_cert;
        cert_handle = ++root_certs_counter;
        root_ca_cert.handle = cert_handle;
        root_ca_cert.session_id = session_data->session_id;
        root_ca_cert.certificate_id = certificate_id;
        root_ca_cert.is_default = true;
        root_chain->second->certificates.push_back(std::move(root_ca_cert));
    }

    LOG_DEBUG(Service_HTTP,
              "called, root_cert_chain_handle={} certificate_id={} cert_handle={} result={:#010X}",
              root_cert_chain_handle, certificate_id, cert_handle, result.raw);

    IPC::RequestBuilder rb = rp.MakeBuilder(2, 0);
    rb.Push(result);
    rb.Push(cert_handle);
}

void HTTP_C::RootCertChainRemoveCert(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const RootCertChain::Handle root_cert_chain_handle = rp.Pop<u32>();
    const RootCertChain::RootCACert::Handle cert_handle = rp.Pop<u32>();

    auto* session_data = GetSessionData(ctx.Session());
    ASSERT(session_data);

    Result result = ResultSuccess;
    const auto root_chain = root_cert_chains.find(root_cert_chain_handle);
    if (!session_data->initialized) {
        LOG_ERROR(Service_HTTP, "Command called without Initialize");
        result = ErrorStateError;
    } else if (root_chain == root_cert_chains.end() ||
               root_chain->second->session_id != session_data->session_id) {
        LOG_ERROR(Service_HTTP, "called with wrong root_cert_chain_handle {}",
                  root_cert_chain_handle);
        result = ErrorWrongCertHandle;
    } else {
        auto& certificates = root_chain->second->certificates;
        const auto certificate = std::find_if(
            certificates.begin(), certificates.end(), [cert_handle](const auto& entry) {
                return entry.handle == cert_handle;
            });
        if (certificate == certificates.end()) {
            LOG_ERROR(Service_HTTP, "called with wrong root cert handle {}", cert_handle);
            result = ErrorWrongCertHandle;
        } else {
            certificates.erase(certificate);
        }
    }

    LOG_DEBUG(Service_HTTP,
              "called, root_cert_chain_handle={} cert_handle={} result={:#010X}",
              root_cert_chain_handle, cert_handle, result.raw);

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(result);
}

void HTTP_C::SetDefaultClientCert(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const Context::Handle context_handle = rp.Pop<u32>();
    const ClientCertID client_cert_id = static_cast<ClientCertID>(rp.Pop<u32>());

    LOG_DEBUG(Service_HTTP, "client_cert_id={}", client_cert_id);

    if (!PerformStateChecks(ctx, rp, context_handle)) {
        return;
    }

    Context& http_context = GetContext(context_handle);

    if (client_cert_id != ClientCertID::Default) {
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
        rb.Push(ErrorWrongCertID);
        return;
    }

    http_context.uses_default_client_cert = true;
    http_context.clcert_data = &GetClCertA();

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(ResultSuccess);
}

void HTTP_C::SetClientCertContext(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const u32 context_handle = rp.Pop<u32>();
    const u32 client_cert_handle = rp.Pop<u32>();

    LOG_DEBUG(Service_HTTP, "called with context_handle={} client_cert_handle={}", context_handle,
              client_cert_handle);

    if (!PerformStateChecks(ctx, rp, context_handle)) {
        return;
    }

    Context& http_context = GetContext(context_handle);

    auto cert_context_itr = client_certs.find(client_cert_handle);
    if (cert_context_itr == client_certs.end()) {
        LOG_ERROR(Service_HTTP, "called with wrong client_cert_handle {}", client_cert_handle);
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
        rb.Push(ErrorWrongCertHandle);
        return;
    }

    if (http_context.ssl_config.client_cert_ctx.lock()) {
        LOG_ERROR(Service_HTTP,
                  "Tried to set a client cert to a context that already has a client cert");
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
        rb.Push(ErrorCertAlreadySet);
        return;
    }

    if (http_context.state != RequestState::NotStarted) {
        LOG_ERROR(Service_HTTP,
                  "Tried to set a client cert on a context that has already been started.");
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
        rb.Push(ErrorInvalidRequestState);
        return;
    }

    http_context.ssl_config.client_cert_ctx = cert_context_itr->second;
    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(ResultSuccess);
}

void HTTP_C::GetSSLError(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const u32 context_handle = rp.Pop<u32>();

    LOG_WARNING(Service_HTTP, "(STUBBED) called, context_handle={}", context_handle);

    [[maybe_unused]] Context& http_context = GetContext(context_handle);

    IPC::RequestBuilder rb = rp.MakeBuilder(2, 0);
    rb.Push(ResultSuccess);
    // Since we create the actual http/ssl context only when the request is submitted we can't check
    // for SSL Errors here. Just submit no error.
    rb.Push<u32>(0);
}

void HTTP_C::SetSSLOpt(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const u32 context_handle = rp.Pop<u32>();
    const u32 opts = rp.Pop<u32>();

    LOG_WARNING(Service_HTTP, "(STUBBED) called, context_handle={}, opts={}", context_handle, opts);

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(ResultSuccess);
}

void HTTP_C::OpenClientCertContext(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    u32 cert_size = rp.Pop<u32>();
    u32 key_size = rp.Pop<u32>();
    Kernel::MappedBuffer& cert_buffer = rp.PopMappedBuffer();
    Kernel::MappedBuffer& key_buffer = rp.PopMappedBuffer();

    LOG_DEBUG(Service_HTTP, "called, cert_size {}, key_size {}", cert_size, key_size);

    auto* session_data = GetSessionData(ctx.Session());
    ASSERT(session_data);

    Result result(ResultSuccess);

    if (!session_data->initialized) {
        LOG_ERROR(Service_HTTP, "Command called without Initialize");
        result = ErrorStateError;
    } else if (session_data->current_http_context) {
        LOG_ERROR(Service_HTTP, "Command called with a bound context");
        result = ErrorNotImplemented;
    } else if (session_data->num_client_certs >= 2) {
        LOG_ERROR(Service_HTTP, "tried to load more then 2 client certs");
        result = ErrorTooManyClientCerts;
    } else {
        client_certs[++client_certs_counter] = std::make_shared<ClientCertContext>();
        client_certs[client_certs_counter]->handle = client_certs_counter;
        client_certs[client_certs_counter]->certificate.resize(cert_size);
        cert_buffer.Read(&client_certs[client_certs_counter]->certificate[0], 0, cert_size);
        client_certs[client_certs_counter]->private_key.resize(key_size);
        cert_buffer.Read(&client_certs[client_certs_counter]->private_key[0], 0, key_size);
        client_certs[client_certs_counter]->session_id = session_data->session_id;

        ++session_data->num_client_certs;
    }

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 4);
    rb.Push(result);
    rb.PushMappedBuffer(cert_buffer);
    rb.PushMappedBuffer(key_buffer);
}

void HTTP_C::OpenDefaultClientCertContext(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    u8 cert_id = rp.Pop<u8>();

    LOG_DEBUG(Service_HTTP, "called, cert_id={} cert_handle={}", cert_id, client_certs_counter);

    auto* session_data = EnsureSessionInitialized(ctx, rp);
    if (!session_data) {
        return;
    }

    if (session_data->current_http_context) {
        LOG_ERROR(Service_HTTP, "Command called with a bound context");
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
        rb.Push(ErrorNotImplemented);
        return;
    }

    if (session_data->num_client_certs >= 2) {
        LOG_ERROR(Service_HTTP, "tried to load more then 2 client certs");
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
        rb.Push(ErrorTooManyClientCerts);
        return;
    }

    constexpr u8 default_cert_id = static_cast<u8>(ClientCertID::Default);
    if (cert_id != default_cert_id) {
        LOG_ERROR(Service_HTTP, "called with invalid cert_id {}", cert_id);
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
        rb.Push(ErrorWrongCertID);
        return;
    }

    if (!ClCertA.init) {
        LOG_ERROR(Service_HTTP, "called but ClCertA is missing");
    }

    const auto& it = std::find_if(client_certs.begin(), client_certs.end(),
                                  [default_cert_id, &session_data](const auto& i) {
                                      return default_cert_id == i.second->cert_id &&
                                             session_data->session_id == i.second->session_id;
                                  });

    if (it != client_certs.end()) {
        IPC::RequestBuilder rb = rp.MakeBuilder(2, 0);
        rb.Push(ResultSuccess);
        rb.Push<u32>(it->first);

        LOG_DEBUG(Service_HTTP, "called, with an already loaded cert_id={}", cert_id);
        return;
    }

    client_certs[++client_certs_counter] = std::make_shared<ClientCertContext>();
    client_certs[client_certs_counter]->handle = client_certs_counter;
    client_certs[client_certs_counter]->certificate = ClCertA.certificate;
    client_certs[client_certs_counter]->private_key = ClCertA.private_key;
    client_certs[client_certs_counter]->session_id = session_data->session_id;
    ++session_data->num_client_certs;

    IPC::RequestBuilder rb = rp.MakeBuilder(2, 0);
    rb.Push(ResultSuccess);
    rb.Push<u32>(client_certs_counter);
}

void HTTP_C::CloseClientCertContext(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    ClientCertContext::Handle cert_handle = rp.Pop<u32>();

    LOG_DEBUG(Service_HTTP, "called, cert_handle={}", cert_handle);

    auto* session_data = GetSessionData(ctx.Session());
    ASSERT(session_data);

    if (client_certs.find(cert_handle) == client_certs.end()) {
        LOG_ERROR(Service_HTTP, "Command called with a unkown client cert handle {}", cert_handle);
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
        // This just return success without doing anything
        rb.Push(ResultSuccess);
        return;
    }

    if (client_certs[cert_handle]->session_id != session_data->session_id) {
        LOG_ERROR(Service_HTTP, "called from another main session");
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
        // This just return success without doing anything
        rb.Push(ResultSuccess);
        return;
    }

    client_certs.erase(cert_handle);
    session_data->num_client_certs--;

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(ResultSuccess);
}

void HTTP_C::SetKeepAlive(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const u32 context_handle = rp.Pop<u32>();
    const u32 option = rp.Pop<u32>();

    LOG_WARNING(Service_HTTP, "(STUBBED) called, handle={}, option={}", context_handle, option);

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(ResultSuccess);
}

void HTTP_C::SetPostDataTypeSize(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const u32 context_handle = rp.Pop<u32>();
    const PostDataType type = rp.PopEnum<PostDataType>();
    const u32 size = rp.Pop<u32>();

    LOG_DEBUG(Service_HTTP, "called, context_handle={}, type={}, size={}", context_handle, type,
              size);

    if (!PerformStateChecks(ctx, rp, context_handle)) {
        return;
    }

    Context& http_context = GetContext(context_handle);

    if (http_context.state != RequestState::NotStarted) {
        LOG_ERROR(Service_HTTP,
                  "Tried to set chunked mode on a context that has already been started");
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
        rb.Push(ErrorInvalidRequestState);
        return;
    }

    if (!http_context.post_data.empty() || !http_context.post_data_raw.empty()) {
        LOG_ERROR(Service_HTTP, "Tried to set chunked mode on a context that has Post data");
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
        rb.Push(ErrorIncompatibleSendPostData);
        return;
    }

    switch (type) {
    case PostDataType::AsciiForm:
    case PostDataType::MultipartForm:
    case PostDataType::Raw:
        http_context.post_data_type = type;
        break;
    default:
        http_context.post_data_type = PostDataType::AsciiForm;
        break;
    }

    http_context.chunked_request = true;
    http_context.chunked_content_length = size;

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(ResultSuccess);
}

void HTTP_C::Finalize(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);

    shared_memory = nullptr;

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(ResultSuccess);

    LOG_WARNING(Service_HTTP, "(STUBBED) called");
}

void HTTP_C::RegisterURLReplacement(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const u32 pattern_size = rp.Pop<u32>();
    const u32 replacement_size = rp.Pop<u32>();

    const std::vector<u8>& pattern_buf = rp.PopStaticBuffer();
    const std::vector<u8>& replacement_buf = rp.PopStaticBuffer();

    std::string pattern(reinterpret_cast<const char*>(pattern_buf.data()),
                        std::min(static_cast<size_t>(pattern_size), pattern_buf.size()));
    std::string replacement(
        reinterpret_cast<const char*>(replacement_buf.data()),
        std::min(static_cast<size_t>(replacement_size), replacement_buf.size()));

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    if (url_replacer.HasRule(pattern)) {
        rb.Push(Result{ErrorDescription::AlreadyExists, ErrorModule::HTTP,
                       ErrorSummary::InvalidArgument, ErrorLevel::Status});
        return;
    }

    Result res = url_replacer.AddRule(pattern, replacement)
                     ? ResultSuccess
                     : Result{ErrorDescription::InvalidCombination, ErrorModule::HTTP,
                              ErrorSummary::InvalidArgument, ErrorLevel::Status};
    if (res.IsSuccess()) {
        res = url_replacer.Save() ? res
                                  : Result{ErrorDescription::OutOfMemory, ErrorModule::HTTP,
                                           ErrorSummary::Internal, ErrorLevel::Permanent};
    }

    rb.Push(res);
}

void HTTP_C::UnregisterURLReplacement(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const u32 pattern_size = rp.Pop<u32>();

    const std::vector<u8>& pattern_buf = rp.PopStaticBuffer();

    std::string pattern(reinterpret_cast<const char*>(pattern_buf.data()),
                        std::min(static_cast<size_t>(pattern_size), pattern_buf.size()));

    bool deleted = url_replacer.DeleteRule(pattern);
    Result res = deleted ? ResultSuccess
                         : Result{ErrorDescription::NotFound, ErrorModule::HTTP,
                                  ErrorSummary::NotFound, ErrorLevel::Info};
    if (deleted) {
        url_replacer.Save();
    }

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(res);
}

void HTTP_C::GetDownloadSizeState(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const Context::Handle context_handle = rp.Pop<u32>();

    const auto* session_data = EnsureSessionInitialized(ctx, rp);
    if (!session_data) {
        return;
    }

    auto context_itr = contexts.find(context_handle);
    if (context_itr == contexts.end()) {
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
        rb.Push(ErrorContextNotFound);
        LOG_ERROR(Service_HTTP, "GetDownloadSizeState on missing context, handle={}",
                  context_handle);
        return;
    }

    Context& http_context = context_itr->second;
    const bool future_valid = http_context.request_future.valid();
    const bool request_complete = IsRequestComplete(http_context);

    u64 current_size = 0;
    u64 total_size = 0;
    u64 received_size = 0;
    std::size_t mirrored_body = 0;
    std::size_t buffered_body = 0;
    bool response_complete = false;
    {
        std::lock_guard lock(http_context.response_mutex);
        current_size = http_context.current_copied_data;
        total_size = GetResponseTotalSizeLocked(http_context);
        received_size = http_context.current_download_size_bytes.load();
        mirrored_body = http_context.response.body.size();
        buffered_body = http_context.response_body_buffer.size();
        response_complete = http_context.response_complete;
    }

    if (total_size > 0) {
        current_size = std::min(current_size, total_size);
    }

    LOG_INFO(Service_HTTP,
             "GetDownloadSizeState: handle={} state={} future_valid={} request_complete={} "
             "response_complete={} current={} total={} received={} buffered={} mirrored_body={}",
             context_handle, static_cast<u32>(http_context.state.load()), future_valid,
             request_complete, response_complete, current_size, total_size, received_size,
             buffered_body, mirrored_body);

    IPC::RequestBuilder rb = rp.MakeBuilder(3, 0);
    rb.Push(ResultSuccess);
    rb.Push(ClampDownloadSizeToGuest(current_size));
    rb.Push(ClampDownloadSizeToGuest(total_size));
}

SessionData* HTTP_C::EnsureSessionInitialized(Kernel::HLERequestContext& ctx,
                                              IPC::RequestParser rp) {
    auto* session_data = GetSessionData(ctx.Session());
    ASSERT(session_data);

    if (!session_data->initialized) {
        LOG_ERROR(Service_HTTP, "Tried to make a request on an uninitialized session");
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
        rb.Push(ErrorStateError);
        return nullptr;
    }

    return session_data;
}

bool HTTP_C::PerformStateChecks(Kernel::HLERequestContext& ctx, IPC::RequestParser rp,
                                Context::Handle context_handle) {
    const auto* session_data = EnsureSessionInitialized(ctx, rp);
    if (!session_data) {
        return false;
    }

    // This command can only be called with a bound context
    if (!session_data->current_http_context) {
        LOG_ERROR(Service_HTTP, "Tried to make a request without a bound context");

        IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
        rb.Push(ErrorNotImplemented);
        return false;
    }

    if (session_data->current_http_context != context_handle) {
        LOG_ERROR(
            Service_HTTP,
            "Tried to make a request on a mismatched session input context={} session context={}",
            context_handle, *session_data->current_http_context);
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
        rb.Push(ErrorStateError);
        return false;
    }

    return true;
}

void HTTP_C::DecryptClCertA() {
    if (!HW::AES::IsNormalKeyAvailable(HW::AES::KeySlotID::SSLKey)) {
        LOG_ERROR(Service_HTTP, "NormalKey in KeySlot 0x0D missing");
        return;
    }

    HW::AES::AESKey key = HW::AES::GetNormalKey(HW::AES::KeySlotID::SSLKey);
    static constexpr u32 iv_length = 16;
    std::vector<u8> cert_file_data;
    std::vector<u8> key_file_data;

    FileSys::NCCHArchive archive(0x0004001b00010002, Service::FS::MediaType::NAND);

    std::array<char, 8> exefs_filepath;
    FileSys::Path file_path = FileSys::MakeNCCHFilePath(
        FileSys::NCCHFileOpenType::NCCHData, 0, FileSys::NCCHFilePathType::RomFS, exefs_filepath);
    FileSys::Mode open_mode = {};
    open_mode.read_flag.Assign(1);
    auto file_result = archive.OpenFile(file_path, open_mode, 0);
    if (file_result.Failed()) {
        LOG_ERROR(Service_HTTP, "ClCertA file missing, using default");

        cert_file_data.resize(ctr_common_1_cert_bin_size);
        memcpy(cert_file_data.data(), ctr_common_1_cert_bin, cert_file_data.size());

        key_file_data.resize(ctr_common_1_key_bin_size);
        memcpy(key_file_data.data(), ctr_common_1_key_bin, key_file_data.size());
    } else {
        auto romfs = std::move(file_result).Unwrap();
        std::vector<u8> romfs_buffer(romfs->GetSize());
        romfs->Read(0, romfs_buffer.size(), romfs_buffer.data());
        romfs->Close();

        const RomFS::RomFSFile cert_file =
            RomFS::GetFile(romfs_buffer.data(), {u"ctr-common-1-cert.bin"});
        if (cert_file.Length() == 0) {
            LOG_ERROR(Service_HTTP, "ctr-common-1-cert.bin missing");
            return;
        }
        if (cert_file.Length() <= iv_length) {
            LOG_ERROR(Service_HTTP, "ctr-common-1-cert.bin size is too small. Size: {}",
                      cert_file.Length());
            return;
        }

        cert_file_data.resize(cert_file.Length());
        memcpy(cert_file_data.data(), cert_file.Data(), cert_file.Length());

        const RomFS::RomFSFile key_file =
            RomFS::GetFile(romfs_buffer.data(), {u"ctr-common-1-key.bin"});
        if (key_file.Length() == 0) {
            LOG_ERROR(Service_HTTP, "ctr-common-1-key.bin missing");
            return;
        }
        if (key_file.Length() <= iv_length) {
            LOG_ERROR(Service_HTTP, "ctr-common-1-key.bin size is too small. Size: {}",
                      key_file.Length());
            return;
        }

        key_file_data.resize(key_file.Length());
        memcpy(key_file_data.data(), key_file.Data(), key_file.Length());
    }

    std::vector<u8> cert_data(cert_file_data.size() - iv_length);

    using CryptoPP::AES;
    CryptoPP::CBC_Mode<AES>::Decryption aes_cert;
    std::array<u8, iv_length> cert_iv;
    std::memcpy(cert_iv.data(), cert_file_data.data(), iv_length);
    aes_cert.SetKeyWithIV(key.data(), AES::BLOCKSIZE, cert_iv.data());
    aes_cert.ProcessData(cert_data.data(), cert_file_data.data() + iv_length,
                         cert_file_data.size() - iv_length);

    std::vector<u8> key_data(key_file_data.size() - iv_length);

    CryptoPP::CBC_Mode<AES>::Decryption aes_key;
    std::array<u8, iv_length> key_iv;
    std::memcpy(key_iv.data(), key_file_data.data(), iv_length);
    aes_key.SetKeyWithIV(key.data(), AES::BLOCKSIZE, key_iv.data());
    aes_key.ProcessData(key_data.data(), key_file_data.data() + iv_length,
                        key_file_data.size() - iv_length);

    ClCertA.certificate = std::move(cert_data);
    ClCertA.private_key = std::move(key_data);
    ClCertA.init = true;
}

URLReplacer::URLReplacer() {
    const std::string path{fmt::format("{}/http_hle_replace_rules.txt",
                                       FileUtil::GetUserPath(FileUtil::UserPath::SysDataDir))};

    FileUtil::IOFile f(path, "rb");
    if (!f.IsOpen()) {
        return;
    }

    std::string pattern;
    std::string replacement;
    while (f.ReadLine(pattern) && f.ReadLine(replacement)) {
        try {
            rules.push_back(Rule{
                .regex = boost::regex(pattern),
                .pattern = pattern,
                .replacement = replacement,
            });
        } catch (const boost::regex_error& e) {
            LOG_ERROR(Service_HTTP, "Failed to load HTTP HLE replacement pattern \"{}\": {}",
                      pattern, e.what());
        }
    }
}

bool URLReplacer::HasRule(const std::string& pattern) {
    for (const auto& rule : rules) {
        if (rule.pattern == pattern) {
            return true;
        }
    }
    return false;
}

bool URLReplacer::AddRule(const std::string& pattern, const std::string& replacement) {
    try {
        rules.push_back(Rule{
            .regex = boost::regex(pattern),
            .pattern = pattern,
            .replacement = replacement,
        });
    } catch (const boost::regex_error& e) {
        return false;
    }
    return true;
}

bool URLReplacer::DeleteRule(const std::string& pattern) {
    const auto old_size = rules.size();

    std::erase_if(rules, [&](const Rule& rule) { return rule.pattern == pattern; });

    return rules.size() != old_size;
}

std::string URLReplacer::Apply(const std::string& url) const {
    std::string result = url;

    for (const auto& rule : rules) {
        if (boost::regex_search(result, rule.regex)) {
            result = boost::regex_replace(result, rule.regex, rule.replacement,
                                          boost::match_default | boost::format_all);
            LOG_WARNING(Service_HTTP, "rule \"{}\" has replaced URL \"{}\" to \"{}\"", rule.pattern,
                        url, result);
            break;
        }
    }

    return result;
}

bool URLReplacer::Save() {
    const std::string path{fmt::format("{}/http_hle_replace_rules.txt",
                                       FileUtil::GetUserPath(FileUtil::UserPath::SysDataDir))};

    FileUtil::IOFile f(path, "wb");

    for (const auto& rule : rules) {
        if ((f.WriteLine(rule.pattern) != rule.pattern.size() + 1) ||
            (f.WriteLine(rule.replacement) != rule.replacement.size() + 1)) {
            LOG_ERROR(Service_HTTP, "failed to write URL replacement rules");
            f.Close();
            FileUtil::Delete(path);
            return false;
        }
    }

    return true;
}

HTTP_C::HTTP_C() : ServiceFramework("http:C", 32) {
    static const FunctionInfo functions[] = {
        // clang-format off
        {0x0001, &HTTP_C::Initialize, "Initialize"},
        {0x0002, &HTTP_C::CreateContext, "CreateContext"},
        {0x0003, &HTTP_C::CloseContext, "CloseContext"},
        {0x0004, &HTTP_C::CancelConnection, "CancelConnection"},
        {0x0005, &HTTP_C::GetRequestState, "GetRequestState"},
        {0x0006, &HTTP_C::GetDownloadSizeState, "GetDownloadSizeState"},
        {0x0007, nullptr, "GetRequestError"},
        {0x0008, &HTTP_C::InitializeConnectionSession, "InitializeConnectionSession"},
        {0x0009, &HTTP_C::BeginRequest, "BeginRequest"},
        {0x000A, &HTTP_C::BeginRequestAsync, "BeginRequestAsync"},
        {0x000B, &HTTP_C::ReceiveData, "ReceiveData"},
        {0x000C, &HTTP_C::ReceiveDataTimeout, "ReceiveDataTimeout"},
        {0x000D, &HTTP_C::SetProxy, "SetProxy"},
        {0x000E, &HTTP_C::SetProxyDefault, "SetProxyDefault"},
        {0x000F, nullptr, "SetBasicAuthorization"},
        {0x0010, nullptr, "SetSocketBufferSize"},
        {0x0011, &HTTP_C::AddRequestHeader, "AddRequestHeader"},
        {0x0012, &HTTP_C::AddPostDataAscii, "AddPostDataAscii"},
        {0x0013, &HTTP_C::AddPostDataBinary, "AddPostDataBinary"},
        {0x0014, &HTTP_C::AddPostDataRaw, "AddPostDataRaw"},
        {0x0015, &HTTP_C::SetPostDataType, "SetPostDataType"},
        {0x0016, &HTTP_C::SendPostDataAscii, "SendPostDataAscii"},
        {0x0017, &HTTP_C::SendPostDataAsciiTimeout, "SendPostDataAsciiTimeout"},
        {0x0018, &HTTP_C::SendPostDataBinary, "SendPostDataBinary"},
        {0x0019, &HTTP_C::SendPostDataBinaryTimeout, "SendPostDataBinaryTimeout"},
        {0x001A, &HTTP_C::SendPostDataRaw, "SendPostDataRaw"},
        {0x001B, &HTTP_C::SendPostDataRawTimeout, "SendPostDataRawTimeout"},
        {0x001C, &HTTP_C::SetPostDataEncoding, "SetPostDataEncoding"},
        {0x001D, &HTTP_C::NotifyFinishSendPostData, "NotifyFinishSendPostData"},
        {0x001E, &HTTP_C::GetResponseHeader, "GetResponseHeader"},
        {0x001F, &HTTP_C::GetResponseHeaderTimeout, "GetResponseHeaderTimeout"},
        {0x0020, &HTTP_C::GetResponseData, "GetResponseData"},
        {0x0021, &HTTP_C::GetResponseDataTimeout, "GetResponseDataTimeout"},
        {0x0022, &HTTP_C::GetResponseStatusCode, "GetResponseStatusCode"},
        {0x0023, &HTTP_C::GetResponseStatusCodeTimeout, "GetResponseStatusCodeTimeout"},
        {0x0024, &HTTP_C::AddTrustedRootCA, "AddTrustedRootCA"},
        {0x0025, &HTTP_C::AddDefaultCert, "AddDefaultCert"},
        {0x0026, &HTTP_C::SelectRootCertChain, "SelectRootCertChain"},
        {0x0027, nullptr, "SetClientCert"},
        {0x0028, &HTTP_C::SetDefaultClientCert, "SetDefaultClientCert"},
        {0x0029, &HTTP_C::SetClientCertContext, "SetClientCertContext"},
        {0x002A, &HTTP_C::GetSSLError, "GetSSLError"},
        {0x002B, &HTTP_C::SetSSLOpt, "SetSSLOpt"},
        {0x002C, nullptr, "SetSSLClearOpt"},
        {0x002D, &HTTP_C::CreateRootCertChain, "CreateRootCertChain"},
        {0x002E, &HTTP_C::DestroyRootCertChain, "DestroyRootCertChain"},
        {0x002F, &HTTP_C::RootCertChainAddCert, "RootCertChainAddCert"},
        {0x0030, &HTTP_C::RootCertChainAddDefaultCert, "RootCertChainAddDefaultCert"},
        {0x0031, &HTTP_C::RootCertChainRemoveCert, "RootCertChainRemoveCert"},
        {0x0032, &HTTP_C::OpenClientCertContext, "OpenClientCertContext"},
        {0x0033, &HTTP_C::OpenDefaultClientCertContext, "OpenDefaultClientCertContext"},
        {0x0034, &HTTP_C::CloseClientCertContext, "CloseClientCertContext"},
        {0x0035, nullptr, "SetDefaultProxy"},
        {0x0036, nullptr, "ClearDNSCache"},
        {0x0037, &HTTP_C::SetKeepAlive, "SetKeepAlive"},
        {0x0038, &HTTP_C::SetPostDataTypeSize, "SetPostDataTypeSize"},
        {0x0039, &HTTP_C::Finalize, "Finalize"},
        // Custom
        {0x0C00, &HTTP_C::RegisterURLReplacement, "RegisterURLReplacement"},
        {0x0C01, &HTTP_C::UnregisterURLReplacement, "UnregisterURLReplacement"},
        // clang-format on
    };
    RegisterHandlers(functions);

    DecryptClCertA();
}

std::shared_ptr<HTTP_C> GetService(Core::System& system) {
    return system.ServiceManager().GetService<HTTP_C>("http:C");
}

void InstallInterfaces(Core::System& system) {
    auto& service_manager = system.ServiceManager();
    std::make_shared<HTTP_C>()->InstallAsService(service_manager);
}
} // namespace Service::HTTP
