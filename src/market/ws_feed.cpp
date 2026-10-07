#include "market/ws_feed.h"

#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <exception>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "market/field_parser.h"

namespace
{
// A frame larger than this is refused rather than allocated: a live feed carries one short CSV row
// per message, and a length that claims megabytes is a malformed frame or a desynchronised stream.
constexpr uint64_t kMaxFrameBytes = std::size_t{1} << 20;  // 1 MiB
// The magic the protocol appends to the client key before hashing, from RFC 6455 section 1.3.
constexpr std::string_view kWebSocketGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

// SHA-1, RFC 3174, the whole of what the opening handshake needs. It is written out rather than
// linked because the only thing that consumes it is one hash of a sixty byte string, and pulling a
// crypto dependency into the build for that would be a larger decision than the hash is worth.
class Sha1
{
   public:
    Sha1()
    {
        state_[0] = 0x67452301;
        state_[1] = 0xEFCDAB89;
        state_[2] = 0x98BADCFE;
        state_[3] = 0x10325476;
        state_[4] = 0xC3D2E1F0;
    }

    void update(const unsigned char* data, std::size_t length)
    {
        bit_length_ += static_cast<uint64_t>(length) * 8;
        for (std::size_t index = 0; index < length; ++index)
        {
            buffer_[buffer_length_++] = data[index];
            if (buffer_length_ == 64)
            {
                transform(buffer_);
                buffer_length_ = 0;
            }
        }
    }

    void final(unsigned char out[20])
    {
        const uint64_t total_bits = bit_length_;
        unsigned char padding = 0x80;
        update(&padding, 1);
        padding = 0x00;
        while (buffer_length_ != 56) update(&padding, 1);
        // The length in bits, big endian, is appended by hand: update() would count it as data.
        unsigned char length_bytes[8];
        for (int index = 0; index < 8; ++index)
            length_bytes[index] = static_cast<unsigned char>(total_bits >> (56 - index * 8));
        for (unsigned char byte : length_bytes) buffer_[buffer_length_++] = byte;
        transform(buffer_);
        buffer_length_ = 0;
        for (int word = 0; word < 5; ++word)
        {
            out[word * 4 + 0] = static_cast<unsigned char>(state_[word] >> 24);
            out[word * 4 + 1] = static_cast<unsigned char>(state_[word] >> 16);
            out[word * 4 + 2] = static_cast<unsigned char>(state_[word] >> 8);
            out[word * 4 + 3] = static_cast<unsigned char>(state_[word]);
        }
    }

   private:
    static uint32_t rotate_left(uint32_t value, int bits)
    {
        return (value << bits) | (value >> (32 - bits));
    }

    void transform(const unsigned char block[64])
    {
        uint32_t words[80];
        for (int index = 0; index < 16; ++index)
            words[index] = (static_cast<uint32_t>(block[index * 4]) << 24) |
                           (static_cast<uint32_t>(block[index * 4 + 1]) << 16) |
                           (static_cast<uint32_t>(block[index * 4 + 2]) << 8) |
                           static_cast<uint32_t>(block[index * 4 + 3]);
        for (int index = 16; index < 80; ++index)
            words[index] = rotate_left(words[index - 3] ^ words[index - 8] ^ words[index - 14] ^
                                           words[index - 16],
                                       1);

        uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3], e = state_[4];
        for (int index = 0; index < 80; ++index)
        {
            uint32_t f = 0;
            uint32_t k = 0;
            if (index < 20)
            {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999;
            }
            else if (index < 40)
            {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            }
            else if (index < 60)
            {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            }
            else
            {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            const uint32_t temp = rotate_left(a, 5) + f + e + k + words[index];
            e = d;
            d = c;
            c = rotate_left(b, 30);
            b = a;
            a = temp;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
    }

    uint32_t state_[5];
    uint64_t bit_length_{0};
    unsigned char buffer_[64]{};
    std::size_t buffer_length_{0};
};

std::string base64_encode(const unsigned char* data, std::size_t length)
{
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((length + 2) / 3) * 4);
    std::size_t index = 0;
    while (index + 3 <= length)
    {
        const uint32_t triple = (static_cast<uint32_t>(data[index]) << 16) |
                                (static_cast<uint32_t>(data[index + 1]) << 8) |
                                static_cast<uint32_t>(data[index + 2]);
        out.push_back(kAlphabet[(triple >> 18) & 0x3F]);
        out.push_back(kAlphabet[(triple >> 12) & 0x3F]);
        out.push_back(kAlphabet[(triple >> 6) & 0x3F]);
        out.push_back(kAlphabet[triple & 0x3F]);
        index += 3;
    }
    if (index < length)
    {
        uint32_t triple = static_cast<uint32_t>(data[index]) << 16;
        if (index + 1 < length) triple |= static_cast<uint32_t>(data[index + 1]) << 8;
        out.push_back(kAlphabet[(triple >> 18) & 0x3F]);
        out.push_back(kAlphabet[(triple >> 12) & 0x3F]);
        out.push_back(index + 1 < length ? kAlphabet[(triple >> 6) & 0x3F] : '=');
        out.push_back('=');
    }
    return out;
}

std::string base64_of_sha1(std::string_view input)
{
    Sha1 sha;
    sha.update(reinterpret_cast<const unsigned char*>(input.data()), input.size());
    unsigned char digest[20];
    sha.final(digest);
    return base64_encode(digest, sizeof(digest));
}

// The value of a header in an HTTP response, matched on a lowercased name and trimmed. Empty when the
// header is absent, which the caller treats as "not offered" rather than as an error.
std::string header_value(std::string_view response, std::string_view name)
{
    std::size_t line_start = response.find("\r\n");
    if (line_start == std::string_view::npos) return {};
    line_start += 2;
    while (line_start < response.size())
    {
        const std::size_t line_end = response.find("\r\n", line_start);
        if (line_end == std::string_view::npos) break;
        std::string_view line = response.substr(line_start, line_end - line_start);
        const std::size_t colon = line.find(':');
        if (colon != std::string_view::npos)
        {
            std::string_view key = line.substr(0, colon);
            if (key.size() == name.size())
            {
                bool equal = true;
                for (std::size_t i = 0; i < key.size(); ++i)
                    if (std::tolower(static_cast<unsigned char>(key[i])) != name[i])
                    {
                        equal = false;
                        break;
                    }
                if (equal)
                {
                    std::string_view value = line.substr(colon + 1);
                    while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
                        value.remove_prefix(1);
                    while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
                        value.remove_suffix(1);
                    return std::string(value);
                }
            }
        }
        line_start = line_end + 2;
    }
    return {};
}

std::string first_line(std::string_view response)
{
    const std::size_t end = response.find("\r\n");
    return std::string(response.substr(0, end == std::string_view::npos ? response.size() : end));
}

Side parse_side_token(std::string_view token)
{
    if (token == "buy" || token == "bid") return Side::Buy;
    if (token == "sell" || token == "ask") return Side::Sell;
    return Side::Unknown;
}

// Writes the whole buffer, returning false on a closed or failed socket.
bool write_all(int fd, std::string_view data)
{
    std::size_t done = 0;
    while (done < data.size())
    {
        const ssize_t sent = ::send(fd, data.data() + done, data.size() - done, MSG_NOSIGNAL);
        if (sent > 0)
        {
            done += static_cast<std::size_t>(sent);
            continue;
        }
        if (sent < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}
}  // namespace


std::string WebSocketFeedSummary::to_log_string() const
{
    return "connected=" + std::to_string(connected ? 1 : 0) +
           " clean_close=" + std::to_string(clean_close ? 1 : 0) +
           " frames=" + std::to_string(frames) + " messages=" + std::to_string(messages) +
           " trades=" + std::to_string(trades) + " depth_updates=" + std::to_string(depth_updates) +
           " control_frames=" + std::to_string(control_frames) +
           " parse_errors=" + std::to_string(parse_errors);
}

WebSocketFeed::WebSocketFeed(Endpoint endpoint, std::size_t ring_capacity)
    : endpoint_(std::move(endpoint)), trades_(ring_capacity), depth_(ring_capacity)
{
    if (endpoint_.host.empty()) throw std::invalid_argument("a websocket endpoint needs a host");
    if (endpoint_.port == 0) throw std::invalid_argument("a websocket endpoint needs a port");
    if (endpoint_.target.empty()) endpoint_.target = "/";
}

WebSocketFeed::~WebSocketFeed()
{
    stop();
}

bool WebSocketFeed::connect_socket()
{
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    const std::string port = std::to_string(endpoint_.port);
    if (getaddrinfo(endpoint_.host.c_str(), port.c_str(), &hints, &result) != 0) return false;

    int fd = -1;
    for (addrinfo* address = result; address != nullptr; address = address->ai_next)
    {
        fd = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd < 0) continue;
        if (::connect(fd, address->ai_addr, address->ai_addrlen) == 0) break;
        ::close(fd);
        fd = -1;
    }
    freeaddrinfo(result);
    if (fd < 0) return false;
    sock_ = fd;

    // A receive timeout is what lets a waiting read notice stop(): the loop recv returns EAGAIN
    // periodically and checks the run flag rather than blocking a join forever.
    timeval timeout{};
    timeout.tv_sec = 0;
    timeout.tv_usec = 200'000;
    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    return true;
}

bool WebSocketFeed::read_exact(void* destination, std::size_t count)
{
    auto* out = static_cast<unsigned char*>(destination);
    std::size_t done = 0;
    while (done < count)
    {
        const ssize_t got = ::recv(sock_, out + done, count - done, 0);
        if (got > 0)
        {
            done += static_cast<std::size_t>(got);
            continue;
        }
        if (got == 0) return false;  // the peer closed the connection
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            if (!running_.load(std::memory_order_acquire)) return false;
            continue;
        }
        return false;
    }
    return true;
}

void WebSocketFeed::perform_handshake()
{
    unsigned char key_bytes[16];
    std::random_device random;
    for (unsigned char& byte : key_bytes) byte = static_cast<unsigned char>(random());
    const std::string key = base64_encode(key_bytes, sizeof(key_bytes));

    std::string request;
    request += "GET " + endpoint_.target + " HTTP/1.1\r\n";
    request += "Host: " + endpoint_.host + ":" + std::to_string(endpoint_.port) + "\r\n";
    request += "Upgrade: websocket\r\n";
    request += "Connection: Upgrade\r\n";
    request += "Sec-WebSocket-Key: " + key + "\r\n";
    request += "Sec-WebSocket-Version: 13\r\n\r\n";
    if (!write_all(sock_, request))
        throw std::runtime_error("websocket handshake could not be written");

    std::string response;
    char byte = 0;
    while (response.find("\r\n\r\n") == std::string::npos)
    {
        if (!read_exact(&byte, 1))
            throw std::runtime_error("the connection closed during the websocket handshake");
        response.push_back(byte);
        if (response.size() > 16 * 1024)
            throw std::runtime_error("the websocket handshake response is too large");
    }
    if (response.rfind("HTTP/1.1 101", 0) != 0 && response.rfind("HTTP/1.0 101", 0) != 0)
        throw std::runtime_error("the websocket handshake was refused: " + first_line(response));

    // The accept key is the peer proving it read the key this client sent. A missing header is
    // tolerated because some simple servers omit it; a present but wrong header is a protocol error.
    const std::string accept = header_value(response, "sec-websocket-accept");
    const std::string expected = base64_of_sha1(key + std::string(kWebSocketGuid));
    if (!accept.empty() && accept != expected)
        throw std::runtime_error("the websocket handshake returned a wrong accept key");
    summary_.connected = true;
}


void WebSocketFeed::start()
{
    if (running_.load()) return;
    running_.store(true, std::memory_order_release);
    try
    {
        if (!connect_socket())
            throw std::runtime_error("could not connect to " + endpoint_.host + ":" +
                                     std::to_string(endpoint_.port));
        perform_handshake();
    }
    catch (...)
    {
        running_.store(false, std::memory_order_release);
        stop();
        throw;
    }
    thread_ = std::thread([this] { receive_loop(); });
}

void WebSocketFeed::stop()
{
    running_.store(false, std::memory_order_release);
    if (sock_ >= 0)
    {
        // shutdown() unblocks a recv() that is already inside the kernel, so the join does not wait
        // out the receive timeout.
        ::shutdown(sock_, SHUT_RDWR);
    }
    if (thread_.joinable()) thread_.join();
    if (sock_ >= 0)
    {
        ::close(sock_);
        sock_ = -1;
    }
    producer_done_.store(true, std::memory_order_release);
}

bool WebSocketFeed::read_frame(std::string& payload, int& opcode, bool& fin)
{
    unsigned char header[2];
    if (!read_exact(header, 2)) return false;
    fin = (header[0] & 0x80) != 0;
    opcode = header[0] & 0x0F;
    const bool masked = (header[1] & 0x80) != 0;
    uint64_t length = header[1] & 0x7F;
    if (length == 126)
    {
        unsigned char extended[2];
        if (!read_exact(extended, 2)) return false;
        length = (static_cast<uint64_t>(extended[0]) << 8) | extended[1];
    }
    else if (length == 127)
    {
        unsigned char extended[8];
        if (!read_exact(extended, 8)) return false;
        length = 0;
        for (unsigned char byte : extended) length = (length << 8) | byte;
    }
    if (length > kMaxFrameBytes)
        throw std::runtime_error("websocket frame length " + std::to_string(length) +
                                 " exceeds the cap");

    unsigned char mask[4] = {0, 0, 0, 0};
    if (masked && !read_exact(mask, 4)) return false;

    payload.resize(static_cast<std::size_t>(length));
    if (length > 0 && !read_exact(payload.data(), static_cast<std::size_t>(length))) return false;
    if (masked)
        for (uint64_t index = 0; index < length; ++index)
            payload[static_cast<std::size_t>(index)] ^= mask[index & 3];
    return true;
}

namespace
{
// One client-to-server frame, masked as RFC 6455 requires of a client. Only control frames are sent
// back, so this is the pong path and nothing hot.
void send_masked_frame(int fd, int opcode, std::string_view payload)
{
    unsigned char mask[4];
    std::random_device random;
    for (unsigned char& byte : mask) byte = static_cast<unsigned char>(random());

    std::string frame;
    frame.push_back(static_cast<char>(0x80 | opcode));
    const std::size_t length = payload.size();
    if (length < 126)
        frame.push_back(static_cast<char>(0x80 | length));
    else if (length < 65536)
    {
        frame.push_back(static_cast<char>(0x80 | 126));
        frame.push_back(static_cast<char>(length >> 8));
        frame.push_back(static_cast<char>(length & 0xFF));
    }
    else
    {
        frame.push_back(static_cast<char>(0x80 | 127));
        for (int shift = 56; shift >= 0; shift -= 8)
            frame.push_back(static_cast<char>((static_cast<uint64_t>(length) >> shift) & 0xFF));
    }
    for (unsigned char byte : mask) frame.push_back(static_cast<char>(byte));
    for (std::size_t index = 0; index < length; ++index)
        frame.push_back(static_cast<char>(payload[index] ^ mask[index & 3]));
    write_all(fd, frame);
}
}  // namespace

void WebSocketFeed::receive_loop()
{
    std::string payload;
    std::string assembled;
    try
    {
        while (running_.load(std::memory_order_relaxed))
        {
            int opcode = 0;
            bool fin = true;
            if (!read_frame(payload, opcode, fin)) break;
            ++summary_.frames;

            if (opcode == 0x1 || opcode == 0x0)
            {
                // A data frame, possibly one fragment of a message: continuation frames append to
                // what is assembled and only a frame with FIN set is delivered to the parser.
                assembled += payload;
                if (fin)
                {
                    ++summary_.messages;
                    handle_message(assembled);
                    assembled.clear();
                }
            }
            else if (opcode == 0x8)
            {
                summary_.clean_close = true;
                break;
            }
            else if (opcode == 0x9)
            {
                ++summary_.control_frames;
                send_masked_frame(sock_, 0xA, payload);
            }
            else if (opcode == 0xA)
            {
                ++summary_.control_frames;
            }
            else
            {
                throw std::runtime_error("unsupported websocket opcode " + std::to_string(opcode));
            }
        }
    }
    catch (const std::exception&)
    {
        // A transport error ends the session the same way a close does: the readers drain what
        // arrived and then report end of stream. The counters above still describe what was seen.
    }
    // Published last and with release, so a reader that sees it also sees every push before it.
    producer_done_.store(true, std::memory_order_release);
}


void WebSocketFeed::handle_message(std::string_view message)
{
    std::vector<std::string_view> fields;
    std::size_t start = 0;
    while (start <= message.size())
    {
        std::size_t end = message.find('\n', start);
        std::string_view line =
            end == std::string_view::npos ? message.substr(start) : message.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        const bool last = end == std::string_view::npos;

        if (!line.empty())
        {
            try
            {
                csv_fields::split_csv_views(line, fields, 7);
                // A header row names its first column "exchange"; both replay shapes start with it,
                // so a streamed file that opens with a header is skipped rather than parsed as data.
                if (!fields.empty() && fields[0] == "exchange")
                {
                    // skip a header line
                }
                else if (fields.size() == 7)
                {
                    MarketTrade trade;
                    trade.exchange = std::string(fields[0]);
                    trade.symbol = std::string(fields[1]);
                    trade.ts = csv_fields::parse_uint64(fields[2]);
                    trade.sequence = csv_fields::parse_uint64(fields[3]);
                    trade.aggressor_side = parse_side_token(fields[4]);
                    trade.price = csv_fields::parse_double(fields[5]);
                    trade.quantity = csv_fields::parse_uint64(fields[6]);
                    if (trade.aggressor_side == Side::Unknown)
                        ++summary_.parse_errors;
                    else
                    {
                        while (!trades_.try_push(std::move(trade)))
                        {
                            if (!running_.load(std::memory_order_acquire)) return;
                            concurrency::spin_relax();
                        }
                        ++summary_.trades;
                    }
                }
                else if (fields.size() == 8)
                {
                    DepthMessage row;
                    row.exchange = std::string(fields[0]);
                    row.symbol = std::string(fields[1]);
                    row.update.exchange_ts = csv_fields::parse_uint64(fields[2]);
                    row.update.local_ts = csv_fields::parse_uint64(fields[3]);
                    row.update.is_snapshot = csv_fields::parse_boolean(fields[4]);
                    row.update.side = parse_side_token(fields[5]);
                    row.update.price = csv_fields::parse_double(fields[6]);
                    row.update.amount = csv_fields::parse_uint64(fields[7]);
                    if (row.update.side == Side::Unknown)
                        ++summary_.parse_errors;
                    else
                    {
                        while (!depth_.try_push(std::move(row)))
                        {
                            if (!running_.load(std::memory_order_acquire)) return;
                            concurrency::spin_relax();
                        }
                        ++summary_.depth_updates;
                    }
                }
                else
                {
                    ++summary_.parse_errors;
                }
            }
            catch (const std::exception&)
            {
                // A row the parsers refuse is counted and skipped: one malformed row must not tear
                // down a session, and parse_errors is what says it happened.
                ++summary_.parse_errors;
            }
        }

        if (last) break;
        start = end + 1;
    }
}

// The trade reader: it hands back one MarketTrade per delivered trade row, in arrival order.
class WebSocketFeed::TradeReader final : public IMarketEventReader
{
   public:
    explicit TradeReader(WebSocketFeed& feed) : feed_(feed) {}

    [[nodiscard]] bool next(MarketEvent& event) override
    {
        MarketTrade trade;
        while (true)
        {
            if (feed_.trades_.try_pop(trade))
            {
                event = std::move(trade);
                return true;
            }
            if (feed_.producer_done_.load(std::memory_order_acquire))
            {
                // The producer published "done" after its last push, so this acquire load pairs with
                // that release store and one more attempt is guaranteed to see any final trade.
                if (feed_.trades_.try_pop(trade))
                {
                    event = std::move(trade);
                    return true;
                }
                return false;
            }
            if (!feed_.running_.load(std::memory_order_acquire)) return false;
            concurrency::spin_relax();
        }
    }

   private:
    WebSocketFeed& feed_;
};


// The depth reader: it regroups consecutive depth rows that share a local timestamp into one
// IncrementalBookBatch, which is the rule the incremental CSV reader groups rows on, so the book
// applies a live batch exactly as it applies a replayed one. One row past the group is carried as
// `pending_` so it starts the next batch rather than being read twice.
class WebSocketFeed::DepthReader final : public IMarketEventReader
{
   public:
    explicit DepthReader(WebSocketFeed& feed) : feed_(feed) {}

    [[nodiscard]] bool next(MarketEvent& event) override
    {
        DepthMessage first;
        if (!pop(first)) return false;

        IncrementalBookBatch batch;
        batch.ts = first.update.exchange_ts;
        batch.exchange_ts = first.update.exchange_ts;
        batch.local_ts = first.update.local_ts;
        batch.symbol = first.symbol;
        batch.exchange = first.exchange;
        batch.snapshot_metadata_valid = true;
        batch.contains_snapshot = first.update.is_snapshot;
        batch.updates.push_back(first.update);

        while (true)
        {
            DepthMessage next_row;
            if (feed_.depth_.try_pop(next_row))
            {
                if (next_row.update.local_ts == batch.local_ts && next_row.symbol == batch.symbol)
                {
                    batch.contains_snapshot = batch.contains_snapshot || next_row.update.is_snapshot;
                    batch.updates.push_back(next_row.update);
                    continue;
                }
                pending_ = std::move(next_row);
                has_pending_ = true;
                break;
            }
            if (feed_.producer_done_.load(std::memory_order_acquire))
            {
                if (feed_.depth_.try_pop(next_row))
                {
                    if (next_row.update.local_ts == batch.local_ts && next_row.symbol == batch.symbol)
                    {
                        batch.contains_snapshot =
                            batch.contains_snapshot || next_row.update.is_snapshot;
                        batch.updates.push_back(next_row.update);
                        continue;
                    }
                    pending_ = std::move(next_row);
                    has_pending_ = true;
                }
                break;
            }
            if (!feed_.running_.load(std::memory_order_acquire)) break;
            concurrency::spin_relax();
        }

        event = std::move(batch);
        return true;
    }

   private:
    bool pop(DepthMessage& out)
    {
        if (has_pending_)
        {
            out = std::move(pending_);
            has_pending_ = false;
            return true;
        }
        while (true)
        {
            if (feed_.depth_.try_pop(out)) return true;
            if (feed_.producer_done_.load(std::memory_order_acquire))
            {
                if (feed_.depth_.try_pop(out)) return true;
                return false;
            }
            if (!feed_.running_.load(std::memory_order_acquire)) return false;
            concurrency::spin_relax();
        }
    }

    WebSocketFeed& feed_;
    DepthMessage pending_;
    bool has_pending_{false};
};

std::string WebSocketFeed::accept_for_key(std::string_view key)
{
    return base64_of_sha1(std::string(key) + std::string(kWebSocketGuid));
}

std::unique_ptr<IMarketEventReader> WebSocketFeed::trades_reader()
{
    return std::make_unique<TradeReader>(*this);
}

std::unique_ptr<IMarketEventReader> WebSocketFeed::depth_reader()
{
    return std::make_unique<DepthReader>(*this);
}
