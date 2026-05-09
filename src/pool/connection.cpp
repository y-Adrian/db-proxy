#include "pool/connection.h"
#include "core/logger.h"
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#include <fcntl.h>
#include <cstring>
#include <algorithm>
#include <openssl/sha.h>
#include <openssl/evp.h>

namespace dbproxy {

namespace {

constexpr uint32_t CLIENT_LONG_PASSWORD = 0x00000001;
constexpr uint32_t CLIENT_LONG_FLAG = 0x00000004;
constexpr uint32_t CLIENT_CONNECT_WITH_DB = 0x00000008;
constexpr uint32_t CLIENT_PROTOCOL_41 = 0x00000200;
constexpr uint32_t CLIENT_TRANSACTIONS = 0x00002000;
constexpr uint32_t CLIENT_SECURE_CONNECTION = 0x00008000;
constexpr uint32_t CLIENT_MULTI_STATEMENTS = 0x00010000;
constexpr uint32_t CLIENT_MULTI_RESULTS = 0x00020000;
constexpr uint32_t CLIENT_PLUGIN_AUTH = 0x00080000;

void appendInt3(std::string& out, size_t value) {
    out.push_back(static_cast<char>(value & 0xff));
    out.push_back(static_cast<char>((value >> 8) & 0xff));
    out.push_back(static_cast<char>((value >> 16) & 0xff));
}

void appendInt4(std::string& out, uint32_t value) {
    out.push_back(static_cast<char>(value & 0xff));
    out.push_back(static_cast<char>((value >> 8) & 0xff));
    out.push_back(static_cast<char>((value >> 16) & 0xff));
    out.push_back(static_cast<char>((value >> 24) & 0xff));
}

uint16_t readInt2(const std::string& data, size_t offset) {
    return static_cast<uint8_t>(data[offset]) |
           (static_cast<uint8_t>(data[offset + 1]) << 8);
}

bool waitFd(int fd, bool write, std::chrono::milliseconds timeout) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);

    struct timeval tv;
    tv.tv_sec = static_cast<long>(timeout.count() / 1000);
    tv.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);

    int ret = select(fd + 1, write ? nullptr : &fds, write ? &fds : nullptr, nullptr, &tv);
    return ret > 0 && FD_ISSET(fd, &fds);
}

bool readLenEncInt(const std::string& data, size_t& offset, uint64_t& value) {
    if (offset >= data.size()) {
        return false;
    }

    uint8_t first = static_cast<uint8_t>(data[offset++]);
    if (first < 0xfb) {
        value = first;
        return true;
    }
    if (first == 0xfc && offset + 2 <= data.size()) {
        value = static_cast<uint8_t>(data[offset]) |
                (static_cast<uint8_t>(data[offset + 1]) << 8);
        offset += 2;
        return true;
    }
    if (first == 0xfd && offset + 3 <= data.size()) {
        value = static_cast<uint8_t>(data[offset]) |
                (static_cast<uint8_t>(data[offset + 1]) << 8) |
                (static_cast<uint8_t>(data[offset + 2]) << 16);
        offset += 3;
        return true;
    }
    if (first == 0xfe && offset + 8 <= data.size()) {
        value = 0;
        for (int i = 0; i < 8; ++i) {
            value |= static_cast<uint64_t>(static_cast<uint8_t>(data[offset++])) << (i * 8);
        }
        return true;
    }

    return false;
}

bool readLenEncString(const std::string& data, size_t& offset, std::string& value) {
    if (offset < data.size() && static_cast<uint8_t>(data[offset]) == 0xfb) {
        ++offset;
        value = "NULL";
        return true;
    }

    uint64_t len = 0;
    if (!readLenEncInt(data, offset, len) || offset + len > data.size()) {
        return false;
    }

    value.assign(data.data() + offset, static_cast<size_t>(len));
    offset += static_cast<size_t>(len);
    return true;
}

std::string readNullTerminated(const std::string& data, size_t& offset) {
    size_t end = data.find('\0', offset);
    if (end == std::string::npos) {
        end = data.size();
    }
    std::string value = data.substr(offset, end - offset);
    offset = end < data.size() ? end + 1 : end;
    return value;
}

bool isEOFPacket(const std::string& payload) {
    return !payload.empty() &&
           static_cast<uint8_t>(payload[0]) == 0xfe &&
           payload.size() < 9;
}

std::string digest(const EVP_MD* md, const std::string& data) {
    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    EVP_Digest(data.data(), data.size(), out, &len, md, nullptr);
    return std::string(reinterpret_cast<char*>(out), len);
}

}  // namespace

uint64_t Connection::next_id_ = 1;

Connection::Connection(const std::string& host, uint16_t port,
                       const std::string& user, const std::string& password,
                       const std::string& database)
    : id_(next_id_++), remote_host_(host), remote_port_(port),
      username_(user), password_(password), database_(database),
      last_active_(std::chrono::steady_clock::now()) {
}

Connection::~Connection() {
    close();
}

bool Connection::connect() {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) {
        LOG_ERROR("Failed to create socket: " + std::string(strerror(errno)));
        return false;
    }

    int flags = fcntl(fd_, F_GETFL, 0);
    fcntl(fd_, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(remote_port_);

    if (inet_pton(AF_INET, remote_host_.c_str(), &addr.sin_addr) <= 0) {
        struct hostent* he = gethostbyname(remote_host_.c_str());
        if (he == nullptr) {
            last_error_ = "Failed to resolve host: " + remote_host_;
            LOG_ERROR(last_error_);
            close();
            return false;
        }
        memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
    }

    int ret = ::connect(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (ret < 0 && errno != EINPROGRESS) {
        last_error_ = "Failed to connect: " + std::string(strerror(errno));
        LOG_ERROR(last_error_);
        close();
        return false;
    }

    if (!waitFd(fd_, true, std::chrono::seconds(5))) {
        last_error_ = "Connection timeout";
        LOG_ERROR(last_error_);
        close();
        return false;
    }

    int error = 0;
    socklen_t len = sizeof(error);
    getsockopt(fd_, SOL_SOCKET, SO_ERROR, &error, &len);
    if (error != 0) {
        last_error_ = "Connection failed: " + std::string(strerror(error));
        LOG_ERROR(last_error_);
        close();
        return false;
    }

    if (!doHandshake()) {
        LOG_ERROR("MySQL handshake failed: " + last_error_);
        close();
        return false;
    }

    state_.store(State::IDLE);
    LOG_DEBUG("MySQL connection " + std::to_string(id_) + " connected to " +
              remote_host_ + ":" + std::to_string(remote_port_));
    return true;
}

bool Connection::doHandshake() {
    std::string handshake;
    if (!readPacket(handshake)) {
        last_error_ = "Failed to read handshake packet";
        return false;
    }
    if (handshake.empty() || static_cast<uint8_t>(handshake[0]) != 0x0a) {
        last_error_ = "Unsupported or malformed handshake packet";
        return false;
    }

    size_t offset = 1;
    std::string server_version = readNullTerminated(handshake, offset);
    if (offset + 4 + 8 + 1 + 2 + 1 + 2 + 2 + 1 + 10 > handshake.size()) {
        last_error_ = "Malformed handshake packet";
        return false;
    }

    offset += 4;
    std::string scramble(handshake.data() + offset, 8);
    offset += 9;

    uint32_t capabilities = readInt2(handshake, offset);
    offset += 2;
    uint8_t charset = static_cast<uint8_t>(handshake[offset++]);
    offset += 2;
    capabilities |= static_cast<uint32_t>(readInt2(handshake, offset)) << 16;
    offset += 2;

    uint8_t auth_plugin_len = static_cast<uint8_t>(handshake[offset++]);
    offset += 10;

    size_t part2_len = auth_plugin_len > 8 ? std::max<size_t>(13, auth_plugin_len - 8) : 13;
    part2_len = std::min(part2_len, handshake.size() - offset);
    scramble.append(handshake.data() + offset, part2_len);
    while (!scramble.empty() && scramble.back() == '\0') {
        scramble.pop_back();
    }
    offset += part2_len;

    std::string auth_plugin_name = offset < handshake.size()
        ? readNullTerminated(handshake, offset)
        : "mysql_native_password";
    if (auth_plugin_name.empty()) {
        auth_plugin_name = "mysql_native_password";
    }

    std::string auth_response = auth_plugin_name == "caching_sha2_password"
        ? makeCachingSha2Response(scramble, password_)
        : makeAuthResponse(scramble, password_);

    uint32_t client_capabilities =
        CLIENT_LONG_PASSWORD |
        CLIENT_LONG_FLAG |
        CLIENT_PROTOCOL_41 |
        CLIENT_TRANSACTIONS |
        CLIENT_SECURE_CONNECTION |
        CLIENT_MULTI_STATEMENTS |
        CLIENT_MULTI_RESULTS |
        CLIENT_PLUGIN_AUTH;
    if (!database_.empty()) {
        client_capabilities |= CLIENT_CONNECT_WITH_DB;
    }
    client_capabilities = (client_capabilities & capabilities) |
                          CLIENT_PROTOCOL_41 |
                          CLIENT_SECURE_CONNECTION;

    std::string auth_payload;
    appendInt4(auth_payload, client_capabilities);
    appendInt4(auth_payload, 16 * 1024 * 1024);
    auth_payload.push_back(static_cast<char>(charset == 0 ? 33 : charset));
    auth_payload.append(23, '\0');
    auth_payload.append(username_);
    auth_payload.push_back('\0');
    auth_payload.push_back(static_cast<char>(auth_response.size()));
    auth_payload.append(auth_response);
    if (!database_.empty()) {
        auth_payload.append(database_);
        auth_payload.push_back('\0');
    }
    auth_payload.append(auth_plugin_name);
    auth_payload.push_back('\0');

    if (!writePacket(auth_payload, 1)) {
        last_error_ = "Failed to send auth packet";
        return false;
    }

    std::string response;
    uint8_t seq = 0;
    if (!readPacket(response, &seq) || response.empty()) {
        last_error_ = "Failed to read auth response";
        return false;
    }

    uint8_t type = static_cast<uint8_t>(response[0]);
    if (type == 0x00) {
        LOG_DEBUG("MySQL authentication successful against " + server_version);
        return true;
    }
    if (type == 0xff) {
        uint16_t error_code = response.size() >= 3 ? readInt2(response, 1) : 0;
        std::string msg = response.size() > 9 ? response.substr(9) : "Authentication failed";
        last_error_ = "MySQL auth error " + std::to_string(error_code) + ": " + msg;
        return false;
    }
    if (type == 0x01 && auth_plugin_name == "caching_sha2_password") {
        if (response.size() >= 2 && static_cast<uint8_t>(response[1]) == 0x03) {
            std::string ok;
            if (readPacket(ok) && !ok.empty() && static_cast<uint8_t>(ok[0]) == 0x00) {
                LOG_DEBUG("MySQL caching_sha2 fast authentication successful");
                return true;
            }
        }
        last_error_ = "caching_sha2 full authentication requires TLS/RSA";
        return false;
    }

    last_error_ = "Unsupported MySQL auth response packet: " + std::to_string(type);
    return false;
}

std::string Connection::makeAuthResponse(const std::string& scramble,
                                         const std::string& password) {
    if (password.empty()) {
        return "";
    }

    std::string stage1 = digest(EVP_sha1(), password);
    std::string stage2 = digest(EVP_sha1(), stage1);
    std::string stage3 = digest(EVP_sha1(), scramble + stage2);

    std::string token(SHA_DIGEST_LENGTH, '\0');
    for (int i = 0; i < SHA_DIGEST_LENGTH; ++i) {
        token[i] = static_cast<char>(stage1[i] ^ stage3[i]);
    }
    return token;
}

std::string Connection::makeCachingSha2Response(const std::string& scramble,
                                                const std::string& password) {
    if (password.empty()) {
        return "";
    }

    std::string stage1 = digest(EVP_sha256(), password);
    std::string stage2 = digest(EVP_sha256(), stage1);
    std::string stage3 = digest(EVP_sha256(), stage2 + scramble);

    std::string token(SHA256_DIGEST_LENGTH, '\0');
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        token[i] = static_cast<char>(stage1[i] ^ stage3[i]);
    }
    return token;
}

bool Connection::readPacket(std::string& payload, uint8_t* sequence_id,
                            std::chrono::milliseconds timeout) {
    char header[4];
    size_t header_read = 0;
    while (header_read < sizeof(header)) {
        if (!waitFd(fd_, false, timeout)) {
            return false;
        }
        ssize_t n = recv(fd_, header + header_read, sizeof(header) - header_read, 0);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        }
        if (n <= 0) {
            return false;
        }
        header_read += static_cast<size_t>(n);
    }

    size_t packet_len = static_cast<uint8_t>(header[0]) |
                        (static_cast<uint8_t>(header[1]) << 8) |
                        (static_cast<uint8_t>(header[2]) << 16);
    if (sequence_id) {
        *sequence_id = static_cast<uint8_t>(header[3]);
    }

    payload.assign(packet_len, '\0');
    size_t received = 0;
    while (received < packet_len) {
        if (!waitFd(fd_, false, timeout)) {
            return false;
        }
        ssize_t n = recv(fd_, payload.data() + received, packet_len - received, 0);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        }
        if (n <= 0) {
            return false;
        }
        received += static_cast<size_t>(n);
    }

    return true;
}

bool Connection::writePacket(const std::string& payload, uint8_t sequence_id) {
    std::string packet;
    appendInt3(packet, payload.size());
    packet.push_back(static_cast<char>(sequence_id));
    packet.append(payload);
    return sendRaw(packet.data(), packet.size());
}

bool Connection::sendRaw(const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        if (!waitFd(fd_, true, std::chrono::seconds(5))) {
            last_error_ = "Timeout while sending to MySQL backend";
            return false;
        }
        ssize_t n = send(fd_, data + sent, len - sent, 0);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        }
        if (n <= 0) {
            last_error_ = "Failed to send to MySQL backend";
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

ssize_t Connection::recvRaw(char* buffer, size_t len, std::chrono::milliseconds timeout) {
    if (!waitFd(fd_, false, timeout)) {
        return 0;
    }
    ssize_t n = recv(fd_, buffer, len, 0);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return 0;
    }
    if (n < 0) {
        last_error_ = "Failed to receive from MySQL backend";
    }
    return n;
}

void Connection::close() {
    if (fd_ >= 0) {
        std::string quit(1, static_cast<char>(0x01));
        writePacket(quit, 0);
        ::close(fd_);
        fd_ = -1;
    }
    state_.store(State::CLOSED);
}

bool Connection::isConnected() const {
    return fd_ >= 0 && state_.load() != State::CLOSED;
}

bool Connection::ping() {
    if (!isConnected()) {
        return false;
    }

    std::string ping_payload(1, static_cast<char>(0x0e));
    if (!writePacket(ping_payload, 0)) {
        return false;
    }

    std::string response;
    return readPacket(response) && !response.empty() && static_cast<uint8_t>(response[0]) == 0x00;
}

bool Connection::execute(const std::string& sql) {
    if (!isConnected()) {
        last_error_ = "Not connected";
        return false;
    }

    clearResult();

    std::string query_payload(1, static_cast<char>(0x03));
    query_payload.append(sql);
    if (!writePacket(query_payload, 0)) {
        last_error_ = "Failed to send query";
        return false;
    }

    return recvResult();
}

void Connection::clearResult() {
    result_columns_.clear();
    result_rows_.clear();
    affected_rows_ = 0;
    last_error_.clear();
}

bool Connection::recvResult() {
    std::string packet;
    if (!readPacket(packet)) {
        last_error_ = "Failed to receive MySQL response";
        return false;
    }
    if (packet.empty()) {
        return true;
    }

    uint8_t packet_type = static_cast<uint8_t>(packet[0]);
    if (packet_type == 0x00) {
        size_t offset = 1;
        uint64_t affected = 0;
        if (readLenEncInt(packet, offset, affected)) {
            affected_rows_ = static_cast<int>(affected);
        }
        return true;
    }
    if (packet_type == 0xff) {
        uint16_t error_code = packet.size() >= 3 ? readInt2(packet, 1) : 0;
        std::string msg = packet.size() > 9 ? packet.substr(9) : "Unknown error";
        last_error_ = "Error " + std::to_string(error_code) + ": " + msg;
        LOG_ERROR("Query error: {}", last_error_);
        return false;
    }
    if (packet_type == 0xfb) {
        last_error_ = "Local infile not supported";
        LOG_WARN(last_error_);
        return false;
    }

    size_t offset = 0;
    uint64_t column_count = 0;
    if (!readLenEncInt(packet, offset, column_count)) {
        last_error_ = "Failed to parse MySQL column count";
        return false;
    }

    for (uint64_t i = 0; i < column_count; ++i) {
        std::string column_packet;
        if (!readPacket(column_packet)) {
            last_error_ = "Failed to receive MySQL column definition";
            return false;
        }

        size_t col_offset = 0;
        std::string ignored;
        std::string name;
        for (int field = 0; field < 4; ++field) {
            if (!readLenEncString(column_packet, col_offset, ignored)) {
                last_error_ = "Failed to parse MySQL column metadata";
                return false;
            }
        }
        if (!readLenEncString(column_packet, col_offset, name)) {
            last_error_ = "Failed to parse MySQL column name";
            return false;
        }
        result_columns_.push_back({name.empty() ? "column_" + std::to_string(i + 1) : name, "text"});
    }

    std::string eof_packet;
    if (!readPacket(eof_packet)) {
        last_error_ = "Failed to receive MySQL column EOF packet";
        return false;
    }

    while (true) {
        std::string row_packet;
        if (!readPacket(row_packet)) {
            last_error_ = "Failed to receive MySQL row packet";
            return false;
        }
        if (isEOFPacket(row_packet) ||
            (!row_packet.empty() && static_cast<uint8_t>(row_packet[0]) == 0x00 && row_packet.size() < 9)) {
            break;
        }
        if (!row_packet.empty() && static_cast<uint8_t>(row_packet[0]) == 0xff) {
            uint16_t error_code = row_packet.size() >= 3 ? readInt2(row_packet, 1) : 0;
            last_error_ = "Error " + std::to_string(error_code);
            return false;
        }

        std::vector<std::string> row;
        row.reserve(static_cast<size_t>(column_count));
        size_t row_offset = 0;
        for (uint64_t i = 0; i < column_count; ++i) {
            std::string value;
            if (!readLenEncString(row_packet, row_offset, value)) {
                value.clear();
            }
            row.push_back(std::move(value));
        }
        result_rows_.push_back(std::move(row));
    }

    return true;
}

}  // namespace dbproxy
