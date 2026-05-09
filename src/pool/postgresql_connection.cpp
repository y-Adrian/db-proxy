#include "pool/postgresql_connection.h"
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
#include <cctype>
#include <random>
#include <sstream>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>

namespace dbproxy {

namespace {

void appendInt32(std::string& out, uint32_t value) {
    uint32_t net = htonl(value);
    out.append(reinterpret_cast<const char*>(&net), sizeof(net));
}

int16_t readInt16(const std::string& data, size_t offset) {
    uint16_t net = 0;
    memcpy(&net, data.data() + offset, sizeof(net));
    return static_cast<int16_t>(ntohs(net));
}

int32_t readInt32(const std::string& data, size_t offset) {
    uint32_t net = 0;
    memcpy(&net, data.data() + offset, sizeof(net));
    return static_cast<int32_t>(ntohl(net));
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

std::string readCString(const std::string& data, size_t& offset) {
    size_t end = data.find('\0', offset);
    if (end == std::string::npos) {
        end = data.size();
    }
    std::string value = data.substr(offset, end - offset);
    offset = end < data.size() ? end + 1 : end;
    return value;
}

std::string digestHex(const EVP_MD* md, const std::string& input) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    EVP_Digest(input.data(), input.size(), digest, &len, md, nullptr);

    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (unsigned int i = 0; i < len; ++i) {
        out.push_back(hex[(digest[i] >> 4) & 0x0f]);
        out.push_back(hex[digest[i] & 0x0f]);
    }
    return out;
}

std::string base64Encode(const std::string& input) {
    std::string out(4 * ((input.size() + 2) / 3), '\0');
    int len = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                              reinterpret_cast<const unsigned char*>(input.data()),
                              static_cast<int>(input.size()));
    out.resize(static_cast<size_t>(len));
    return out;
}

std::string base64Decode(const std::string& input) {
    std::string out((input.size() * 3) / 4 + 3, '\0');
    int len = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                              reinterpret_cast<const unsigned char*>(input.data()),
                              static_cast<int>(input.size()));
    if (len < 0) {
        return "";
    }
    size_t padding = 0;
    if (!input.empty() && input.back() == '=') ++padding;
    if (input.size() > 1 && input[input.size() - 2] == '=') ++padding;
    out.resize(static_cast<size_t>(len) - padding);
    return out;
}

std::string hmacSha256(const std::string& key, const std::string& data) {
    unsigned int len = 0;
    unsigned char digest[EVP_MAX_MD_SIZE];
    HMAC(EVP_sha256(),
         key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(data.data()), data.size(),
         digest, &len);
    return std::string(reinterpret_cast<char*>(digest), len);
}

std::string sha256(const std::string& data) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), digest);
    return std::string(reinterpret_cast<char*>(digest), SHA256_DIGEST_LENGTH);
}

std::string randomNonce() {
    static constexpr char alphabet[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<size_t> dist(0, sizeof(alphabet) - 2);

    std::string nonce;
    nonce.reserve(24);
    for (int i = 0; i < 24; ++i) {
        nonce.push_back(alphabet[dist(gen)]);
    }
    return nonce;
}

std::string getScramAttribute(const std::string& message, char key) {
    size_t pos = 0;
    while (pos < message.size()) {
        size_t next = message.find(',', pos);
        if (next == std::string::npos) {
            next = message.size();
        }
        if (next > pos + 2 && message[pos] == key && message[pos + 1] == '=') {
            return message.substr(pos + 2, next - pos - 2);
        }
        pos = next + 1;
    }
    return "";
}

int parseTrailingRowCount(const std::string& tag) {
    size_t end = tag.size();
    while (end > 0 && tag[end - 1] == '\0') {
        --end;
    }
    size_t start = end;
    while (start > 0 && std::isdigit(static_cast<unsigned char>(tag[start - 1]))) {
        --start;
    }
    if (start == end) {
        return 0;
    }
    return std::stoi(tag.substr(start, end - start));
}

}  // namespace

uint64_t PostgreSQLConnection::next_id_ = 1;

PostgreSQLConnection::PostgreSQLConnection(const std::string& host, uint16_t port,
                                           const std::string& user, const std::string& password,
                                           const std::string& database)
    : id_(next_id_++), remote_host_(host), remote_port_(port),
      username_(user), password_(password), database_(database),
      last_active_(std::chrono::steady_clock::now()) {
}

PostgreSQLConnection::~PostgreSQLConnection() {
    close();
}

bool PostgreSQLConnection::connect() {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) {
        last_error_ = "Failed to create socket: " + std::string(strerror(errno));
        LOG_ERROR(last_error_);
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
    if (!waitWritable(std::chrono::seconds(5))) {
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

    if (!doStartup()) {
        LOG_ERROR("PostgreSQL startup failed: " + last_error_);
        close();
        return false;
    }

    state_.store(State::IDLE);
    LOG_DEBUG("PostgreSQL connection " + std::to_string(id_) + " connected to " +
              remote_host_ + ":" + std::to_string(remote_port_));
    return true;
}

bool PostgreSQLConnection::doStartup() {
    std::string params;
    appendInt32(params, 196608);
    params.append("user");
    params.push_back('\0');
    params.append(username_);
    params.push_back('\0');
    if (!database_.empty()) {
        params.append("database");
        params.push_back('\0');
        params.append(database_);
        params.push_back('\0');
    }
    params.append("client_encoding");
    params.push_back('\0');
    params.append("UTF8");
    params.push_back('\0');
    params.append("application_name");
    params.push_back('\0');
    params.append("db-proxy");
    params.push_back('\0');
    params.push_back('\0');

    std::string startup;
    appendInt32(startup, static_cast<uint32_t>(params.size() + 4));
    startup.append(params);
    if (!writeAll(startup.data(), startup.size())) {
        last_error_ = "Failed to send PostgreSQL startup message";
        return false;
    }

    while (true) {
        char type = 0;
        std::string payload;
        if (!readMessage(type, payload)) {
            last_error_ = "Failed to read PostgreSQL startup response";
            return false;
        }

        if (type == 'R') {
            if (payload.size() < 4) {
                last_error_ = "Malformed PostgreSQL authentication response";
                return false;
            }
            int32_t auth_type = readInt32(payload, 0);
            if (!handleAuthentication(auth_type, payload)) {
                return false;
            }
        } else if (type == 'E') {
            parseQueryMessage(type, payload);
            return false;
        } else if (type == 'Z') {
            return true;
        }
    }
}

bool PostgreSQLConnection::handleAuthentication(int32_t auth_type, const std::string& payload) {
    switch (auth_type) {
        case 0:
            return true;
        case 3:
            return handleCleartextPassword();
        case 5:
            if (payload.size() < 8) {
                last_error_ = "Malformed MD5 authentication challenge";
                return false;
            }
            return handleMD5Password(payload.data() + 4);
        case 10:
            return handleSASL(payload);
        default:
            last_error_ = "Unsupported PostgreSQL authentication type: " + std::to_string(auth_type);
            return false;
    }
}

bool PostgreSQLConnection::handleCleartextPassword() {
    std::string payload = password_;
    payload.push_back('\0');
    return writeMessage('p', payload);
}

bool PostgreSQLConnection::handleMD5Password(const char* salt) {
    std::string inner = digestHex(EVP_md5(), password_ + username_);
    std::string salt_input = inner + std::string(salt, 4);
    std::string response = "md5" + digestHex(EVP_md5(), salt_input);
    response.push_back('\0');
    return writeMessage('p', response);
}

bool PostgreSQLConnection::handleSASL(const std::string& payload) {
    bool supports_scram = false;
    size_t offset = 4;
    while (offset < payload.size() && payload[offset] != '\0') {
        std::string mechanism = readCString(payload, offset);
        if (mechanism == "SCRAM-SHA-256") {
            supports_scram = true;
        }
    }
    if (!supports_scram) {
        last_error_ = "Server does not offer SCRAM-SHA-256";
        return false;
    }

    std::string client_nonce = randomNonce();
    std::string client_first_bare = "n=" + username_ + ",r=" + client_nonce;
    std::string client_first = "n,," + client_first_bare;

    std::string initial;
    initial.append("SCRAM-SHA-256");
    initial.push_back('\0');
    appendInt32(initial, static_cast<uint32_t>(client_first.size()));
    initial.append(client_first);
    if (!writeMessage('p', initial)) {
        last_error_ = "Failed to send SCRAM initial response";
        return false;
    }

    char type = 0;
    std::string continue_payload;
    if (!readMessage(type, continue_payload) || type != 'R' ||
        continue_payload.size() < 4 || readInt32(continue_payload, 0) != 11) {
        last_error_ = "Expected SCRAM continue message";
        return false;
    }

    std::string server_first = continue_payload.substr(4);
    std::string server_nonce = getScramAttribute(server_first, 'r');
    std::string salt_b64 = getScramAttribute(server_first, 's');
    std::string iterations_text = getScramAttribute(server_first, 'i');
    if (server_nonce.rfind(client_nonce, 0) != 0 || salt_b64.empty() || iterations_text.empty()) {
        last_error_ = "Malformed SCRAM challenge";
        return false;
    }

    int iterations = std::stoi(iterations_text);
    std::string salt = base64Decode(salt_b64);
    std::string salted_password(SHA256_DIGEST_LENGTH, '\0');
    if (PKCS5_PBKDF2_HMAC(password_.data(), static_cast<int>(password_.size()),
                          reinterpret_cast<const unsigned char*>(salt.data()),
                          static_cast<int>(salt.size()), iterations, EVP_sha256(),
                          SHA256_DIGEST_LENGTH,
                          reinterpret_cast<unsigned char*>(salted_password.data())) != 1) {
        last_error_ = "Failed to derive SCRAM salted password";
        return false;
    }

    std::string client_final_without_proof = "c=biws,r=" + server_nonce;
    std::string auth_message = client_first_bare + "," + server_first + "," + client_final_without_proof;
    std::string client_key = hmacSha256(salted_password, "Client Key");
    std::string stored_key = sha256(client_key);
    std::string client_signature = hmacSha256(stored_key, auth_message);
    std::string client_proof(client_key.size(), '\0');
    for (size_t i = 0; i < client_key.size(); ++i) {
        client_proof[i] = static_cast<char>(client_key[i] ^ client_signature[i]);
    }

    std::string final_payload = client_final_without_proof + ",p=" + base64Encode(client_proof);
    if (!writeMessage('p', final_payload)) {
        last_error_ = "Failed to send SCRAM final response";
        return false;
    }

    std::string final_response;
    if (!readMessage(type, final_response) || type != 'R' ||
        final_response.size() < 4 || readInt32(final_response, 0) != 12) {
        last_error_ = "Expected SCRAM final message";
        return false;
    }

    return true;
}

void PostgreSQLConnection::close() {
    if (fd_ >= 0) {
        std::string payload;
        writeMessage('X', payload);
        ::close(fd_);
        fd_ = -1;
    }
    state_.store(State::CLOSED);
}

bool PostgreSQLConnection::isConnected() const {
    return fd_ >= 0 && state_.load() != State::CLOSED;
}

bool PostgreSQLConnection::ping() {
    return execute("SELECT 1");
}

bool PostgreSQLConnection::execute(const std::string& sql) {
    if (!isConnected()) {
        last_error_ = "Not connected";
        return false;
    }

    clearResult();
    std::string payload = sql;
    payload.push_back('\0');
    if (!writeMessage('Q', payload)) {
        last_error_ = "Failed to send PostgreSQL query";
        return false;
    }

    return waitReadyForQuery();
}

bool PostgreSQLConnection::waitReadyForQuery() {
    while (true) {
        char type = 0;
        std::string payload;
        if (!readMessage(type, payload)) {
            last_error_ = "Failed to receive PostgreSQL query response";
            return false;
        }
        if (type == 'Z') {
            return last_error_.empty();
        }
        if (!parseQueryMessage(type, payload)) {
            return false;
        }
    }
}

bool PostgreSQLConnection::parseQueryMessage(char type, const std::string& payload) {
    if (type == 'T') {
        if (payload.size() < 2) {
            last_error_ = "Malformed RowDescription";
            return false;
        }
        size_t offset = 0;
        int16_t fields = readInt16(payload, offset);
        offset += 2;
        for (int i = 0; i < fields; ++i) {
            std::string name = readCString(payload, offset);
            if (offset + 18 > payload.size()) {
                last_error_ = "Malformed RowDescription field";
                return false;
            }
            offset += 6;
            int32_t type_oid = readInt32(payload, offset);
            offset += 12;
            result_columns_.push_back({name, "oid:" + std::to_string(type_oid)});
        }
        return true;
    }

    if (type == 'D') {
        if (payload.size() < 2) {
            last_error_ = "Malformed DataRow";
            return false;
        }
        size_t offset = 0;
        int16_t fields = readInt16(payload, offset);
        offset += 2;
        std::vector<std::string> row;
        row.reserve(fields);
        for (int i = 0; i < fields; ++i) {
            if (offset + 4 > payload.size()) {
                last_error_ = "Malformed DataRow field length";
                return false;
            }
            int32_t len = readInt32(payload, offset);
            offset += 4;
            if (len < 0) {
                row.push_back("NULL");
            } else {
                if (offset + static_cast<size_t>(len) > payload.size()) {
                    last_error_ = "Malformed DataRow field";
                    return false;
                }
                row.emplace_back(payload.data() + offset, static_cast<size_t>(len));
                offset += static_cast<size_t>(len);
            }
        }
        result_rows_.push_back(std::move(row));
        return true;
    }

    if (type == 'C') {
        affected_rows_ = parseTrailingRowCount(payload);
        return true;
    }

    if (type == 'E') {
        size_t offset = 0;
        std::string message = "PostgreSQL error";
        while (offset < payload.size() && payload[offset] != '\0') {
            char field = payload[offset++];
            std::string value = readCString(payload, offset);
            if (field == 'M') {
                message = value;
            }
        }
        last_error_ = message;
        LOG_ERROR("PostgreSQL query error: {}", last_error_);
        return false;
    }

    return true;
}

void PostgreSQLConnection::clearResult() {
    result_columns_.clear();
    result_rows_.clear();
    affected_rows_ = 0;
    last_error_.clear();
}

bool PostgreSQLConnection::sendRaw(const char* data, size_t len) {
    return writeAll(data, len);
}

ssize_t PostgreSQLConnection::recvRaw(char* buffer, size_t len, std::chrono::milliseconds timeout) {
    if (!waitReadable(timeout)) {
        return 0;
    }
    ssize_t n = recv(fd_, buffer, len, 0);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return 0;
    }
    if (n < 0) {
        last_error_ = "Failed to receive from PostgreSQL backend";
    }
    return n;
}

bool PostgreSQLConnection::readMessage(char& type, std::string& payload,
                                       std::chrono::milliseconds timeout) {
    if (!readExact(&type, 1, timeout)) {
        return false;
    }

    char len_buf[4];
    if (!readExact(len_buf, sizeof(len_buf), timeout)) {
        return false;
    }
    uint32_t net_len = 0;
    memcpy(&net_len, len_buf, sizeof(net_len));
    uint32_t len = ntohl(net_len);
    if (len < 4) {
        return false;
    }

    payload.assign(len - 4, '\0');
    if (!payload.empty() && !readExact(payload.data(), payload.size(), timeout)) {
        return false;
    }
    return true;
}

bool PostgreSQLConnection::writeMessage(char type, const std::string& payload) {
    std::string message;
    message.push_back(type);
    appendInt32(message, static_cast<uint32_t>(payload.size() + 4));
    message.append(payload);
    return writeAll(message.data(), message.size());
}

bool PostgreSQLConnection::readExact(char* buffer, size_t len, std::chrono::milliseconds timeout) {
    size_t received = 0;
    while (received < len) {
        if (!waitReadable(timeout)) {
            return false;
        }
        ssize_t n = recv(fd_, buffer + received, len - received, 0);
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

bool PostgreSQLConnection::writeAll(const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        if (!waitWritable(std::chrono::seconds(5))) {
            last_error_ = "Timeout while sending to PostgreSQL backend";
            return false;
        }
        ssize_t n = send(fd_, data + sent, len - sent, 0);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        }
        if (n <= 0) {
            last_error_ = "Failed to send to PostgreSQL backend";
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool PostgreSQLConnection::waitReadable(std::chrono::milliseconds timeout) const {
    return waitFd(fd_, false, timeout);
}

bool PostgreSQLConnection::waitWritable(std::chrono::milliseconds timeout) const {
    return waitFd(fd_, true, timeout);
}

}  // namespace dbproxy
