#include "lines/venue_relay.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#ifdef TOYQUANT_HAVE_OPENSSL
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "market/ws_feed.h"

namespace
{
constexpr std::string_view kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
constexpr int kPingAfterSeconds = 25;

int64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// ------------------------------------------------------------------ a small JSON reader

// The venue publishes nested JSON, and the fields this relay needs sit inside arrays of arrays
// (`asks: [[price, size, orders, count], ...]`). A recursive parser is less code than field scraping
// through that nesting, and it refuses a shape it does not understand instead of inventing one.
struct Json
{
    enum class Type
    {
        Null,
        Bool,
        Number,
        String,
        Array,
        Object
    };
    Type type{Type::Null};
    bool boolean{false};
    double number{0.0};
    std::string text;
    std::vector<Json> items;
    std::unordered_map<std::string, Json> fields;

    const Json* find(std::string_view key) const
    {
        if (type != Type::Object) return nullptr;
        const auto found = fields.find(std::string(key));
        return found == fields.end() ? nullptr : &found->second;
    }

    bool is_number() const { return type == Type::Number; }
    bool is_string() const { return type == Type::String; }
    bool is_array() const { return type == Type::Array; }

    // The venues quote numbers as strings ("82915.7"), so both spellings are read as text.
    std::string as_text() const
    {
        if (type == Type::String) return text;
        if (type == Type::Number)
        {
            char buffer[32];
            const auto result = std::to_chars(buffer, buffer + sizeof(buffer), number);
            return std::string(buffer, result.ptr);
        }
        return {};
    }

    double as_double() const
    {
        if (type == Type::Number) return number;
        if (type == Type::String) return std::stod(text);
        return 0.0;
    }
};

class JsonParser
{
   public:
    explicit JsonParser(std::string_view text) : text_(text) {}

    bool parse(Json& out, std::string& error)
    {
        skip_space();
        if (!parse_value(out))
        {
            error = "invalid JSON at offset " + std::to_string(position_);
            return false;
        }
        skip_space();
        if (position_ != text_.size())
        {
            error = "trailing text after JSON at offset " + std::to_string(position_);
            return false;
        }
        return true;
    }

   private:
    void skip_space()
    {
        while (position_ < text_.size())
        {
            const char c = text_[position_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                ++position_;
            else
                break;
        }
    }

    bool consume(std::string_view token)
    {
        if (text_.substr(position_, token.size()) != token) return false;
        position_ += token.size();
        return true;
    }

    bool parse_value(Json& out)
    {
        if (position_ >= text_.size()) return false;
        const char c = text_[position_];
        if (c == '{') return parse_object(out);
        if (c == '[') return parse_array(out);
        if (c == '"')
        {
            out.type = Json::Type::String;
            return parse_string(out.text);
        }
        if (consume("true"))
        {
            out.type = Json::Type::Bool;
            out.boolean = true;
            return true;
        }
        if (consume("false"))
        {
            out.type = Json::Type::Bool;
            out.boolean = false;
            return true;
        }
        if (consume("null"))
        {
            out.type = Json::Type::Null;
            return true;
        }
        return parse_number(out);
    }

    bool parse_number(Json& out)
    {
        const std::size_t start = position_;
        if (position_ < text_.size() && (text_[position_] == '-' || text_[position_] == '+'))
            ++position_;
        while (position_ < text_.size())
        {
            const char c = text_[position_];
            if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '-' || c == '+')
                ++position_;
            else
                break;
        }
        if (position_ == start) return false;
        out.type = Json::Type::Number;
        out.number = std::stod(std::string(text_.substr(start, position_ - start)));
        return true;
    }

    bool parse_string(std::string& out)
    {
        if (position_ >= text_.size() || text_[position_] != '"') return false;
        ++position_;
        out.clear();
        while (position_ < text_.size())
        {
            const char c = text_[position_++];
            if (c == '"') return true;
            if (c != '\\')
            {
                out.push_back(c);
                continue;
            }
            if (position_ >= text_.size()) return false;
            const char escaped = text_[position_++];
            switch (escaped)
            {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u':
                    // The fields this relay reads are ASCII; a \u escape is skipped rather than
                    // half-decoded, so a byte it cannot represent is empty instead of wrong.
                    if (position_ + 4 > text_.size()) return false;
                    position_ += 4;
                    break;
                default: return false;
            }
        }
        return false;
    }

    bool parse_array(Json& out)
    {
        out.type = Json::Type::Array;
        ++position_;  // '['
        skip_space();
        if (position_ < text_.size() && text_[position_] == ']')
        {
            ++position_;
            return true;
        }
        while (true)
        {
            out.items.emplace_back();
            skip_space();
            if (!parse_value(out.items.back())) return false;
            skip_space();
            if (position_ >= text_.size()) return false;
            if (text_[position_] == ',')
            {
                ++position_;
                continue;
            }
            if (text_[position_] == ']')
            {
                ++position_;
                return true;
            }
            return false;
        }
    }

    bool parse_object(Json& out)
    {
        out.type = Json::Type::Object;
        ++position_;  // '{'
        skip_space();
        if (position_ < text_.size() && text_[position_] == '}')
        {
            ++position_;
            return true;
        }
        while (true)
        {
            skip_space();
            std::string key;
            if (!parse_string(key)) return false;
            skip_space();
            if (position_ >= text_.size() || text_[position_] != ':') return false;
            ++position_;
            skip_space();
            Json value;
            if (!parse_value(value)) return false;
            out.fields.emplace(std::move(key), std::move(value));
            skip_space();
            if (position_ >= text_.size()) return false;
            if (text_[position_] == ',')
            {
                ++position_;
                continue;
            }
            if (text_[position_] == '}')
            {
                ++position_;
                return true;
            }
            return false;
        }
    }

    std::string_view text_;
    std::size_t position_{0};
};


// ------------------------------------------------------------------ the venue decoder

struct RelayStats
{
    uint64_t trades{0};
    uint64_t book_messages{0};
    uint64_t depth_rows{0};
    uint64_t gaps{0};
    uint64_t reconnects{0};
    uint64_t parse_errors{0};
    // Backpressure and heartbeat, both of which a six second run never exercises: the high-water mark
    // is how deep the hand-off to the engine got, the waits are how often it was full, and the two
    // heartbeat counters say the keepalive path ran at all.
    uint64_t queue_high_water{0};
    uint64_t queue_waits{0};
    uint64_t pings_sent{0};
    uint64_t pongs_sent{0};

    std::string to_log_string() const
    {
        return "trades=" + std::to_string(trades) + " book_messages=" +
               std::to_string(book_messages) + " depth_rows=" + std::to_string(depth_rows) +
               " gaps=" + std::to_string(gaps) + " reconnects=" + std::to_string(reconnects) +
               " parse_errors=" + std::to_string(parse_errors) +
               " queue_high_water=" + std::to_string(queue_high_water) +
               " queue_waits=" + std::to_string(queue_waits) +
               " pings_sent=" + std::to_string(pings_sent) +
               " pongs_sent=" + std::to_string(pongs_sent);
    }
};

int64_t json_int(const Json* value)
{
    return value == nullptr ? 0 : static_cast<int64_t>(std::llround(value->as_double()));
}

// The OKX public channels, decoded into the replay rows this repository already reads. One decoder
// serves all three roles -- relay, direct record, and capture replay -- so a row means the same thing
// wherever it is produced.
class VenueDecoder
{
   public:
    explicit VenueDecoder(const RelayConfig& config)
        : instrument_(config.instrument),
          contract_size_(config.contract_size),
          quantity_scale_(config.quantity_scale),
          max_levels_(config.levels)
    {
    }

    void decode(std::string_view message, std::vector<std::string>& rows, RelayStats& stats)
    {
        Json root;
        std::string error;
        JsonParser parser(message);
        if (!parser.parse(root, error))
        {
            ++stats.parse_errors;
            return;
        }
        const Json* arg = root.find("arg");
        if (arg == nullptr) return;  // an event or a pong, not data
        const Json* channel = arg->find("channel");
        const Json* inst = arg->find("instId");
        const std::string symbol = inst != nullptr ? inst->as_text() : instrument_;
        if (channel == nullptr) return;
        const std::string name = channel->as_text();
        if (name == "trades")
            decode_trades(root, symbol, rows, stats);
        else if (name == "books" || name == "books5" || name == "bbo-tbt")
            decode_books(root, symbol, rows, stats);
    }

   private:
    uint64_t amount_of(const Json* size) const
    {
        const double value = size == nullptr ? 0.0 : size->as_double();
        return static_cast<uint64_t>(std::llround(value * contract_size_ *
                                                   static_cast<double>(quantity_scale_)));
    }

    void decode_trades(const Json& root, const std::string& symbol, std::vector<std::string>& rows,
                       RelayStats& stats)
    {
        const Json* data = root.find("data");
        if (data == nullptr || !data->is_array()) return;
        for (const Json& trade : data->items)
        {
            const Json* ts = trade.find("ts");
            if (ts == nullptr) continue;
            const Json* id = trade.find("tradeId");
            const Json* side = trade.find("side");
            const Json* price = trade.find("px");
            const Json* size = trade.find("sz");
            std::string row = "okx,";
            row += symbol;
            row += ',';
            row += ts->as_text();
            row += ',';
            row += id != nullptr ? id->as_text() : std::string();
            row += ',';
            row += side != nullptr ? side->as_text() : std::string();
            row += ',';
            row += price != nullptr ? price->as_text() : std::string();
            row += ',';
            row += std::to_string(amount_of(size));
            row += '\n';
            rows.push_back(std::move(row));
            ++stats.trades;
        }
    }

    void decode_books(const Json& root, const std::string& symbol, std::vector<std::string>& rows,
                      RelayStats& stats);


    std::string instrument_;
    double contract_size_;
    uint64_t quantity_scale_;
    std::size_t max_levels_;
    int64_t last_seq_{0};
    bool has_last_seq_{false};
};

void VenueDecoder::decode_books(const Json& root, const std::string& symbol,
                                std::vector<std::string>& rows, RelayStats& stats)
{
    const Json* action = root.find("action");
    const bool snapshot = action == nullptr || action->as_text() == "snapshot";
    const Json* data = root.find("data");
    if (data == nullptr || !data->is_array()) return;
    for (const Json& book : data->items)
    {
        const int64_t seq = json_int(book.find("seqId"));
        const int64_t previous = json_int(book.find("prevSeqId"));
        if (snapshot)
        {
            has_last_seq_ = true;
            last_seq_ = seq;
        }
        else
        {
            // A break between the venue's `prevSeqId` and the `seqId` we last saw is a lost update;
            // counting it is what "recorded for hours without a drop" is measured on.
            if (has_last_seq_ && previous != last_seq_) ++stats.gaps;
            has_last_seq_ = true;
            last_seq_ = seq;
        }
        const Json* ts = book.find("ts");
        const std::string stamp = ts != nullptr ? ts->as_text() : std::string("0");
        const Json* sides[2] = {book.find("bids"), book.find("asks")};
        const char* names[2] = {"bid", "ask"};
        for (int side = 0; side < 2; ++side)
        {
            const Json* levels = sides[side];
            if (levels == nullptr || !levels->is_array()) continue;
            std::size_t count = levels->items.size();
            if (snapshot && max_levels_ > 0 && count > max_levels_) count = max_levels_;
            for (std::size_t index = 0; index < count; ++index)
            {
                const Json& level = levels->items[index];
                if (level.items.size() < 2) continue;
                std::string row = "okx,";
                row += symbol;
                row += ',';
                row += stamp;
                row += ',';
                row += stamp;
                row += snapshot ? ",1," : ",0,";
                row += names[side];
                row += ',';
                row += level.items[0].as_text();
                row += ',';
                row += std::to_string(amount_of(&level.items[1]));
                row += '\n';
                rows.push_back(std::move(row));
                ++stats.depth_rows;
            }
        }
        ++stats.book_messages;
    }
}


// ------------------------------------------------------------------ transport and framing

struct Url
{
    bool secure{false};
    std::string host;
    uint16_t port{0};
    std::string target{"/"};
};

Url parse_url(const std::string& url)
{
    Url parsed;
    const std::size_t scheme = url.find("://");
    if (scheme == std::string::npos)
        throw std::runtime_error("a venue url needs a scheme: " + url);
    parsed.secure = url.substr(0, scheme) == "wss";
    const std::string rest = url.substr(scheme + 3);
    const std::size_t slash = rest.find('/');
    const std::string host_port = slash == std::string::npos ? rest : rest.substr(0, slash);
    parsed.target = slash == std::string::npos ? "/" : rest.substr(slash);
    const std::size_t colon = host_port.find(':');
    parsed.host = colon == std::string::npos ? host_port : host_port.substr(0, colon);
    parsed.port = static_cast<uint16_t>(
        colon == std::string::npos ? (parsed.secure ? 443 : 80) : std::stoi(host_port.substr(colon + 1)));
    if (parsed.host.empty() || parsed.port == 0)
        throw std::runtime_error("a venue url needs a host and a port: " + url);
    return parsed;
}

// One connection, plaintext or TLS. The TLS branch is compiled only when OpenSSL's headers were found,
// which is the one thing this tree cannot assume; without it a `wss://` url is refused by name rather
// than half-connected.
class Stream
{
   public:
    Stream() = default;
    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;
    ~Stream() { close(); }

    void connect(const Url& url)
    {
        if (url.secure)
        {
#ifdef TOYQUANT_HAVE_OPENSSL
            OPENSSL_init_ssl(0, nullptr);
#else
            throw std::runtime_error(
                "this build has no TLS: pass a ws:// url, or build with OpenSSL's headers");
#endif
        }
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* result = nullptr;
        const std::string port = std::to_string(url.port);
        if (getaddrinfo(url.host.c_str(), port.c_str(), &hints, &result) != 0)
            throw std::runtime_error("could not resolve " + url.host);
        for (addrinfo* address = result; address != nullptr; address = address->ai_next)
        {
            fd_ = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
            if (fd_ < 0) continue;
            if (::connect(fd_, address->ai_addr, address->ai_addrlen) == 0) break;
            ::close(fd_);
            fd_ = -1;
        }
        freeaddrinfo(result);
        if (fd_ < 0) throw std::runtime_error("could not connect to " + url.host + ":" + port);

        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 200'000;
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

#ifdef TOYQUANT_HAVE_OPENSSL
        if (url.secure)
        {
            ctx_ = SSL_CTX_new(TLS_client_method());
            if (ctx_ == nullptr) throw std::runtime_error("could not create a TLS context");
            SSL_CTX_set_default_verify_paths(ctx_);
            SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER, nullptr);
            ssl_ = SSL_new(ctx_);
            SSL_set_tlsext_host_name(ssl_, url.host.c_str());
            SSL_set_fd(ssl_, fd_);
            if (SSL_connect(ssl_) != 1)
            {
                const unsigned long code = ERR_get_error();
                throw std::runtime_error(std::string("TLS handshake failed: ") +
                                         ERR_error_string(code, nullptr));
            }
        }
#endif
    }

    // >0 bytes, 0 closed, -1 error, -2 a receive timeout.
    int read(void* buffer, std::size_t count)
    {
#ifdef TOYQUANT_HAVE_OPENSSL
        if (ssl_ != nullptr)
        {
            const int got = SSL_read(ssl_, buffer, static_cast<int>(count));
            if (got > 0) return got;
            const int error = SSL_get_error(ssl_, got);
            if (error == SSL_ERROR_ZERO_RETURN) return 0;
            if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) return -2;
            return -1;
        }
#endif
        const ssize_t got = ::recv(fd_, buffer, count, 0);
        if (got > 0) return static_cast<int>(got);
        if (got == 0) return 0;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return -2;
        return -1;
    }

    bool write_all(std::string_view data)
    {
        std::size_t done = 0;
        while (done < data.size())
        {
#ifdef TOYQUANT_HAVE_OPENSSL
            if (ssl_ != nullptr)
            {
                const int sent = SSL_write(ssl_, data.data() + done,
                                           static_cast<int>(data.size() - done));
                if (sent <= 0) return false;
                done += static_cast<std::size_t>(sent);
                continue;
            }
#endif
            const ssize_t sent = ::send(fd_, data.data() + done, data.size() - done, MSG_NOSIGNAL);
            if (sent <= 0) return false;
            done += static_cast<std::size_t>(sent);
        }
        return true;
    }

    void close()
    {
#ifdef TOYQUANT_HAVE_OPENSSL
        if (ssl_ != nullptr)
        {
            SSL_shutdown(ssl_);
            SSL_free(ssl_);
            ssl_ = nullptr;
        }
        if (ctx_ != nullptr)
        {
            SSL_CTX_free(ctx_);
            ctx_ = nullptr;
        }
#endif
        if (fd_ >= 0)
        {
            ::close(fd_);
            fd_ = -1;
        }
    }

    // Takes over an already-accepted connection, which is how the server side gets a Stream.
    void adopt(int fd) { fd_ = fd; }

    int fd_{-1};
#ifdef TOYQUANT_HAVE_OPENSSL
    SSL* ssl_{nullptr};
    SSL_CTX* ctx_{nullptr};
#endif
};


// ------------------------------------------------------------------ frames and handshakes

std::string base64_encode(const unsigned char* data, std::size_t length)
{
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
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

uint32_t next_mask_word()
{
    static uint32_t state = static_cast<uint32_t>(now_ms()) ^ 0x9E3779B9u;
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

// 1 a frame, 0 closed, -1 error, -2 a receive timeout.
int read_exact(Stream& stream, void* destination, std::size_t count)
{
    auto* out = static_cast<unsigned char*>(destination);
    std::size_t done = 0;
    while (done < count)
    {
        const int got = stream.read(out + done, count - done);
        if (got > 0)
        {
            done += static_cast<std::size_t>(got);
            continue;
        }
        return got;
    }
    return 1;
}

bool send_frame(Stream& stream, int opcode, std::string_view payload, bool mask)
{
    std::string frame;
    frame.push_back(static_cast<char>(0x80 | opcode));
    const std::size_t length = payload.size();
    if (length < 126)
        frame.push_back(static_cast<char>((mask ? 0x80 : 0) | length));
    else if (length < 65536)
    {
        frame.push_back(static_cast<char>((mask ? 0x80 : 0) | 126));
        frame.push_back(static_cast<char>(length >> 8));
        frame.push_back(static_cast<char>(length & 0xFF));
    }
    else
    {
        frame.push_back(static_cast<char>((mask ? 0x80 : 0) | 127));
        for (int shift = 56; shift >= 0; shift -= 8)
            frame.push_back(static_cast<char>((static_cast<uint64_t>(length) >> shift) & 0xFF));
    }
    if (mask)
    {
        const uint32_t word = next_mask_word();
        const unsigned char key[4] = {static_cast<unsigned char>(word), static_cast<unsigned char>(word >> 8),
                                      static_cast<unsigned char>(word >> 16),
                                      static_cast<unsigned char>(word >> 24)};
        for (unsigned char byte : key) frame.push_back(static_cast<char>(byte));
        for (std::size_t index = 0; index < length; ++index)
            frame.push_back(static_cast<char>(payload[index] ^ key[index & 3]));
    }
    else
    {
        frame.append(payload);
    }
    return stream.write_all(frame);
}

int read_frame(Stream& stream, std::string& payload, int& opcode, bool& fin)
{
    unsigned char header[2];
    const int head = read_exact(stream, header, 2);
    if (head != 1) return head;
    fin = (header[0] & 0x80) != 0;
    opcode = header[0] & 0x0F;
    const bool masked = (header[1] & 0x80) != 0;
    uint64_t length = header[1] & 0x7F;
    if (length == 126)
    {
        unsigned char extended[2];
        if (read_exact(stream, extended, 2) != 1) return -1;
        length = (static_cast<uint64_t>(extended[0]) << 8) | extended[1];
    }
    else if (length == 127)
    {
        unsigned char extended[8];
        if (read_exact(stream, extended, 8) != 1) return -1;
        length = 0;
        for (unsigned char byte : extended) length = (length << 8) | byte;
    }
    if (length > (uint64_t{1} << 24)) return -1;
    unsigned char mask[4] = {0, 0, 0, 0};
    if (masked && read_exact(stream, mask, 4) != 1) return -1;
    payload.resize(static_cast<std::size_t>(length));
    if (length > 0 && read_exact(stream, payload.data(), static_cast<std::size_t>(length)) != 1)
        return -1;
    if (masked)
        for (uint64_t index = 0; index < length; ++index)
            payload[static_cast<std::size_t>(index)] ^= mask[index & 3];
    return 1;
}


void client_handshake(Stream& stream, const Url& url)
{
    unsigned char key_bytes[16];
    for (unsigned char& byte : key_bytes) byte = static_cast<unsigned char>(next_mask_word());
    const std::string key = base64_encode(key_bytes, sizeof(key_bytes));
    std::string request = "GET " + url.target + " HTTP/1.1\r\nHost: " + url.host + ":" +
                          std::to_string(url.port) +
                          "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key +
                          "\r\nSec-WebSocket-Version: 13\r\n"
                          "User-Agent: Mozilla/5.0 toyquant-relay-cpp/1.0\r\n\r\n";
    if (!stream.write_all(request)) throw std::runtime_error("could not write the handshake");
    std::string response;
    char byte = 0;
    while (response.find("\r\n\r\n") == std::string::npos)
    {
        if (stream.read(&byte, 1) != 1)
            throw std::runtime_error("the venue closed during the handshake");
        response.push_back(byte);
        if (response.size() > 16 * 1024) throw std::runtime_error("handshake response too large");
    }
    if (response.rfind("HTTP/1.1 101", 0) != 0 && response.rfind("HTTP/1.0 101", 0) != 0)
        throw std::runtime_error("the venue refused the handshake: " +
                                 response.substr(0, response.find("\r\n")));
}

std::string header_value(const std::string& request, const std::string& name)
{
    std::size_t start = request.find("\r\n");
    if (start == std::string::npos) return {};
    start += 2;
    while (start < request.size())
    {
        const std::size_t end = request.find("\r\n", start);
        if (end == std::string::npos) break;
        const std::string line = request.substr(start, end - start);
        const std::size_t colon = line.find(':');
        if (colon != std::string::npos)
        {
            std::string key = line.substr(0, colon);
            for (char& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (key == name)
            {
                std::string value = line.substr(colon + 1);
                while (!value.empty() && value.front() == ' ') value.erase(value.begin());
                return value;
            }
        }
        start = end + 2;
    }
    return {};
}

// Blocks until the engine connects and completes the handshake, then adopts the connection into `stream`.
void accept_engine(const std::string& listen, Stream& stream)
{
    const std::size_t colon = listen.find(':');
    if (colon == std::string::npos || colon == 0)
        throw std::runtime_error("--listen must be host:port, not " + listen);
    const std::string host = listen.substr(0, colon);
    const int port = std::stoi(listen.substr(colon + 1));

    const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) throw std::runtime_error("could not create the listening socket");
    int one = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(port));
    if (host.empty() || host == "0.0.0.0")
        address.sin_addr.s_addr = INADDR_ANY;
    else if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1)
        throw std::runtime_error("--listen needs a literal IPv4 address");
    if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0)
    {
        ::close(listener);
        throw std::runtime_error("could not bind " + listen);
    }
    if (::listen(listener, 1) < 0)
    {
        ::close(listener);
        throw std::runtime_error("could not listen on " + listen);
    }
    std::cout << "relay listening on ws://" << listen << ", waiting for the engine ..." << std::endl;

    sockaddr_in peer{};
    socklen_t length = sizeof(peer);
    const int conn = ::accept(listener, reinterpret_cast<sockaddr*>(&peer), &length);
    ::close(listener);
    if (conn < 0) throw std::runtime_error("the engine connection was not accepted");

    std::string request;
    char byte = 0;
    while (request.find("\r\n\r\n") == std::string::npos)
    {
        const ssize_t got = ::recv(conn, &byte, 1, 0);
        if (got <= 0)
        {
            ::close(conn);
            throw std::runtime_error("the engine closed during the handshake");
        }
        request.push_back(byte);
        if (request.size() > 16 * 1024)
        {
            ::close(conn);
            throw std::runtime_error("the engine handshake is too large");
        }
    }
    const std::string key = header_value(request, "sec-websocket-key");
    if (key.empty())
    {
        ::close(conn);
        throw std::runtime_error("the engine handshake carries no Sec-WebSocket-Key");
    }
    const std::string accept = WebSocketFeed::accept_for_key(key);
    const std::string response = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                                 "Connection: Upgrade\r\nSec-WebSocket-Accept: " +
                                 accept + "\r\n\r\n";
    std::size_t done = 0;
    while (done < response.size())
    {
        const ssize_t sent = ::send(conn, response.data() + done, response.size() - done, MSG_NOSIGNAL);
        if (sent <= 0)
        {
            ::close(conn);
            throw std::runtime_error("could not answer the engine handshake");
        }
        done += static_cast<std::size_t>(sent);
    }
    std::cout << "engine connected" << std::endl;
    stream.adopt(conn);
}


// ------------------------------------------------------------------ rows, recording, and the venue client

enum class Pop
{
    Row,
    Timeout,
    Closed
};

class RowQueue
{
   public:
    // Rows in flight between the venue thread and the thread serving the engine. Bounded for a live
    // relay, because an engine that falls behind must slow the venue down rather than grow this process
    // without limit; a replay reads a finite file, so it keeps the unbounded queue.
    static constexpr std::size_t kCapacity = 200000;

    explicit RowQueue(bool bounded) : bounded_(bounded) {}

    // Waits for room rather than dropping the row. Returns false once the queue is closed, so a stop
    // does not leave the venue thread blocked in a push nobody will consume.
    bool push(std::string row, RelayStats& stats)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (bounded_ && rows_.size() >= kCapacity)
        {
            ++stats.queue_waits;
            room_.wait(lock, [this] { return rows_.size() < kCapacity || closed_; });
            if (closed_) return false;
        }
        rows_.push_back(std::move(row));
        stats.queue_high_water = std::max<uint64_t>(stats.queue_high_water, rows_.size());
        ready_.notify_one();
        return true;
    }

    void close()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        room_.notify_all();
        ready_.notify_all();
    }

    Pop pop(std::string& row, int timeout_ms)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!ready_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                             [this] { return !rows_.empty() || closed_; }))
            return Pop::Timeout;
        if (rows_.empty()) return Pop::Closed;
        row = std::move(rows_.front());
        rows_.pop_front();
        room_.notify_one();
        return Pop::Row;
    }

   private:
    std::mutex mutex_;
    std::condition_variable ready_;
    std::condition_variable room_;
    std::deque<std::string> rows_;
    bool closed_{false};
    bool bounded_{false};
};

class Recorder
{
   public:
    explicit Recorder(const std::string& prefix)
        : trades_(prefix + "_trades.csv", std::ios::out | std::ios::trunc),
          depth_(prefix + "_depth.csv", std::ios::out | std::ios::trunc)
    {
        if (!trades_.is_open() || !depth_.is_open())
            throw std::runtime_error("could not open the recording files under " + prefix);
        trades_ << "exchange,symbol,timestamp,id,side,price,amount\n";
        depth_ << "exchange,symbol,timestamp,local_timestamp,is_snapshot,side,price,amount\n";
    }

    void write(const std::string& row)
    {
        // A trade row has seven fields and a depth row eight, which is how the reader tells them apart.
        std::size_t commas = 0;
        for (const char c : row)
            if (c == ',') ++commas;
        (commas == 6 ? trades_ : depth_) << row;
        // Flushed as it goes, so a session that is stopped rather than closed loses at most this many
        // rows instead of whatever the stream was holding.
        if (++rows_ % 4096 == 0) flush();
    }

    void flush()
    {
        trades_.flush();
        depth_.flush();
    }

   private:
    std::ofstream trades_;
    std::ofstream depth_;
    std::size_t rows_{0};
};

class VenueClient
{
   public:
    VenueClient(const RelayConfig& config, VenueDecoder& decoder, RelayStats& stats, RowQueue& rows,
                Recorder* recorder, bool enqueue)
        : config_(config), decoder_(decoder), stats_(stats), rows_(rows), recorder_(recorder),
          enqueue_(enqueue)
    {
        if (!config_.capture.empty())
        {
            capture_.open(config_.capture, std::ios::out | std::ios::app);
            if (!capture_.is_open())
                throw std::runtime_error("could not open " + config_.capture);
        }
    }

    void run()
    {
        const int64_t deadline = config_.max_seconds > 0.0
                                     ? now_ms() + static_cast<int64_t>(config_.max_seconds * 1000.0)
                                     : 0;
        int backoff_ms = 1000;
        while (!stop_.load())
        {
            if (deadline > 0 && now_ms() >= deadline) break;
            try
            {
                session(deadline);
                backoff_ms = 1000;
            }
            catch (const std::exception& error)
            {
                if (stop_.load()) break;
                ++stats_.reconnects;
                std::cerr << "venue connection lost (" << error.what() << "); reconnecting in "
                          << backoff_ms / 1000 << "s" << std::endl;
            }
            if (stop_.load()) break;
            if (deadline > 0 && now_ms() >= deadline) break;
            // Sleep in slices so stop() does not wait out the whole backoff.
            for (int waited = 0; waited < backoff_ms && !stop_.load(); waited += 100)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            backoff_ms = static_cast<int>(std::min<int64_t>(backoff_ms * 2LL, 8000LL));
        }
        if (enqueue_) rows_.close();
    }

    void stop() { stop_.store(true); }

   private:
    void session(int64_t deadline)
    {
        const Url url = parse_url(config_.venue);
        Stream stream;
        stream.connect(url);
        client_handshake(stream, url);
        const std::string subscription =
            "{\"op\":\"subscribe\",\"args\":[{\"channel\":\"" + config_.books_channel +
            "\",\"instId\":\"" + config_.instrument + "\"},{\"channel\":\"trades\",\"instId\":\"" +
            config_.instrument + "\"}]}";
        if (!send_frame(stream, 0x1, subscription, true))
            throw std::runtime_error("could not subscribe");

        while (!stop_.load())
        {
            if (deadline > 0 && now_ms() >= deadline) return;
            std::string payload;
            int opcode = 0;
            bool fin = true;
            const int status = read_frame(stream, payload, opcode, fin);
            if (status == -2)
            {
                send_frame(stream, 0x9, "ping", true);  // keep an idle public connection alive
                ++stats_.pings_sent;
                continue;
            }
            if (status != 1) return;  // closed or broken
            if (opcode == 0x8) return;
            if (opcode == 0x9)
            {
                // A control ping is owed a control pong with the same payload. A session that answers
                // neither this nor the text form is dropped by the venue once its heartbeat expires.
                send_frame(stream, 0xA, payload, true);
                ++stats_.pongs_sent;
                continue;
            }
            if (opcode == 0xA) continue;
            if (opcode != 0x1 && opcode != 0x0) continue;
            // OKX's heartbeat is a text frame rather than a control frame, and neither it nor its
            // answer is a market message to capture or decode.
            if (payload == "ping")
            {
                send_frame(stream, 0x1, "pong", true);
                ++stats_.pongs_sent;
                continue;
            }
            if (payload == "pong") continue;
            if (capture_.is_open())
            {
                capture_ << payload << "\n";
                capture_.flush();
            }
            std::vector<std::string> decoded;
            decoder_.decode(payload, decoded, stats_);
            for (std::string& row : decoded)
            {
                if (recorder_ != nullptr) recorder_->write(row);
                if (enqueue_ && !rows_.push(std::move(row), stats_)) return;
            }
        }
    }

    RelayConfig config_;
    VenueDecoder& decoder_;
    RelayStats& stats_;
    RowQueue& rows_;
    Recorder* recorder_;
    bool enqueue_;
    std::ofstream capture_;
    std::atomic<bool> stop_{false};
};

// ------------------------------------------------------------------ serving the engine, and the two entry points

void stream_engine(Stream& engine, RowQueue& rows)
{
    send_frame(engine, 0x1, "exchange,symbol,timestamp,id,side,price,amount\n", false);
    send_frame(engine, 0x1,
               "exchange,symbol,timestamp,local_timestamp,is_snapshot,side,price,amount\n", false);
    std::string row;
    while (true)
    {
        const Pop result = rows.pop(row, 200);
        if (result == Pop::Closed) break;
        if (result == Pop::Timeout) continue;
        if (!send_frame(engine, 0x1, row, false)) break;
    }
    send_frame(engine, 0x8, "", false);
}

int run_relay_impl(const RelayConfig& config)
{
    VenueDecoder decoder(config);
    RelayStats stats;

    if (!config.replay.empty())
    {
        std::ifstream capture(config.replay);
        if (!capture.is_open()) throw std::runtime_error("could not open " + config.replay);
        std::unique_ptr<Recorder> recorder;
        if (!config.record.empty()) recorder = std::make_unique<Recorder>(config.record);
        RowQueue rows(false);  // a replay reads a finite file, so it needs no bound
        std::string line;
        while (std::getline(capture, line))
        {
            if (line.empty()) continue;
            std::vector<std::string> decoded;
            decoder.decode(line, decoded, stats);
            for (std::string& row : decoded)
            {
                if (recorder != nullptr) recorder->write(row);
                rows.push(std::move(row), stats);
            }
        }
        if (recorder != nullptr) recorder->flush();
        rows.close();
        if (!config.listen.empty())
        {
            Stream engine;
            accept_engine(config.listen, engine);
            stream_engine(engine, rows);
        }
        std::cout << stats.to_log_string() << std::endl;
        return 0;
    }

#ifndef TOYQUANT_HAVE_OPENSSL
    // A build without OpenSSL cannot reach a real venue; fail by name instead of retrying a wall.
    if (parse_url(config.venue).secure)
        throw std::runtime_error(
            "this build has no TLS: pass a ws:// venue, or build with OpenSSL's headers");
#endif

    std::unique_ptr<Recorder> recorder;
    if (!config.record.empty()) recorder = std::make_unique<Recorder>(config.record);
    RowQueue rows(true);  // a live relay bounds the hand-off, so a stalled engine slows the venue down
    const bool serve = !config.listen.empty();
    Stream engine;
    if (serve) accept_engine(config.listen, engine);

    VenueClient client(config, decoder, stats, rows, recorder.get(), serve);
    std::thread thread([&client] { client.run(); });
    if (serve)
    {
        stream_engine(engine, rows);
        client.stop();
    }
    thread.join();
    if (recorder != nullptr) recorder->flush();
    std::cout << stats.to_log_string() << std::endl;
    return 0;
}

bool check(bool condition, const char* what)
{
    if (!condition) std::cerr << "self-test FAILED: " << what << "\n";
    return condition;
}

int self_test_impl()
{
    RelayConfig config;
    config.levels = 1;  // so the snapshot cap is visible
    VenueDecoder decoder(config);
    RelayStats stats;
    std::vector<std::string> rows;

    decoder.decode(
        R"({"arg":{"channel":"trades","instId":"BTC-USDT-SWAP"},"data":[{"instId":"BTC-USDT-SWAP","tradeId":"7","px":"100.5","sz":"2.5","side":"buy","ts":"1000","seqId":5}]})",
        rows, stats);
    bool ok = check(rows.size() == 1 && rows[0] == "okx,BTC-USDT-SWAP,1000,7,buy,100.5,25000\n",
                    "a trade row");

    rows.clear();
    decoder.decode(
        R"({"arg":{"channel":"books","instId":"BTC-USDT-SWAP"},"action":"snapshot","data":[{"asks":[["100.6","3","0","1"],["100.7","1","0","1"]],"bids":[["100.4","5","0","2"],["100.3","2","0","2"]],"ts":"1000","seqId":10,"prevSeqId":-1}]})",
        rows, stats);
    ok = check(rows.size() == 2 && rows[0] == "okx,BTC-USDT-SWAP,1000,1000,1,bid,100.4,50000\n" &&
                   rows[1] == "okx,BTC-USDT-SWAP,1000,1000,1,ask,100.6,30000\n",
               "a snapshot capped to one level a side") && ok;

    rows.clear();
    decoder.decode(
        R"({"arg":{"channel":"books","instId":"BTC-USDT-SWAP"},"action":"update","data":[{"asks":[["100.6","0","0","0"]],"bids":[],"ts":"1001","seqId":11,"prevSeqId":10}]})",
        rows, stats);
    ok = check(rows.size() == 1 && rows[0] == "okx,BTC-USDT-SWAP,1001,1001,0,ask,100.6,0\n",
               "a removal") && ok;
    ok = check(stats.gaps == 0, "no gap when the sequence continues") && ok;

    rows.clear();
    decoder.decode(
        R"({"arg":{"channel":"books","instId":"BTC-USDT-SWAP"},"action":"update","data":[{"asks":[],"bids":[],"ts":"1002","seqId":99,"prevSeqId":42}]})",
        rows, stats);
    ok = check(stats.gaps == 1, "a gap when the sequence breaks") && ok;

    rows.clear();
    decoder.decode(R"({"event":"subscribe","arg":{"channel":"books"},"connId":"x"})", rows, stats);
    ok = check(rows.empty(), "an event is ignored") && ok;
    decoder.decode("pong", rows, stats);
    ok = check(stats.parse_errors == 1, "a non-JSON line is counted, not crashed") && ok;

    if (ok) std::cout << "self-test OK" << std::endl;
    return ok ? 0 : 1;
}
}  // namespace

int run_relay(const RelayConfig& config)
{
    return run_relay_impl(config);
}

int relay_self_test()
{
    return self_test_impl();
}

