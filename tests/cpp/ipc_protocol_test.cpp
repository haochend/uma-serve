#include "gtest/gtest.h"
#include "ipc/protocol.h"
#include <vector>
#include <string>

// Test case for try_read_frame with an oversized frame
TEST(ProtocolTest, TryReadFrameOversized) {
    std::vector<uint8_t> rx;
    // Create a length header indicating a frame larger than kDefaultMaxFrameBytes
    // kDefaultMaxFrameBytes is 1 MiB, so 2 MiB will be too large
    uint32_t oversized_len = 2 * 1024 * 1024; 
    rx.push_back((uint8_t)(oversized_len & 0xFF));
    rx.push_back((uint8_t)((oversized_len >> 8) & 0xFF));
    rx.push_back((uint8_t)((oversized_len >> 16) & 0xFF));
    rx.push_back((uint8_t)((oversized_len >> 24) & 0xFF));
    
    // Add some dummy payload data
    rx.push_back(0x01); 

    std::string out_json;
    std::string err_msg;
    bool result = uma::ipc::protocol::try_read_frame(rx, out_json, uma::ipc::protocol::kDefaultMaxFrameBytes, &err_msg);

    // This test is expected to fail with "frame too large"
    ASSERT_FALSE(result);
    ASSERT_EQ(err_msg, "frame too large");
}

// Test case for try_read_frame with an incomplete frame (not enough payload data)
TEST(ProtocolTest, TryReadFrameIncomplete) {
    std::vector<uint8_t> rx;
    // Length header indicating a payload of 10 bytes
    uint32_t len = 10;
    rx.push_back((uint8_t)(len & 0xFF));
    rx.push_back((uint8_t)((len >> 8) & 0xFF));
    rx.push_back((uint8_t)((len >> 16) & 0xFF));
    rx.push_back((uint8_t)((len >> 24) & 0xFF));
    
    // Only 5 bytes of payload data
    rx.push_back('h'); rx.push_back('e'); rx.push_back('l'); rx.push_back('l'); rx.push_back('o');

    std::string out_json;
    std::string err_msg; // Should not be set for incomplete frames
    bool result = uma::ipc::protocol::try_read_frame(rx, out_json, uma::ipc::protocol::kDefaultMaxFrameBytes, &err_msg);

    // This test is expected to return false, but not set an error message
    ASSERT_FALSE(result);
    ASSERT_TRUE(err_msg.empty()); // No error message for incomplete frame, just "need more"
}

// Test case for try_read_frame with too few bytes to even read the length header
TEST(ProtocolTest, TryReadFrameTooShortForHeader) {
    std::vector<uint8_t> rx = {0x01, 0x02, 0x03}; // Only 3 bytes
    std::string out_json;
    std::string err_msg;
    bool result = uma::ipc::protocol::try_read_frame(rx, out_json, uma::ipc::protocol::kDefaultMaxFrameBytes, &err_msg);

    ASSERT_FALSE(result);
    ASSERT_TRUE(err_msg.empty()); // No error message for not enough header bytes
}

// Test case for try_read_frame with a zero length frame (invalid)
TEST(ProtocolTest, TryReadFrameZeroLength) {
    std::vector<uint8_t> rx;
    uint32_t zero_len = 0;
    rx.push_back((uint8_t)(zero_len & 0xFF));
    rx.push_back((uint8_t)((zero_len >> 8) & 0xFF));
    rx.push_back((uint8_t)((zero_len >> 16) & 0xFF));
    rx.push_back((uint8_t)((zero_len >> 24) & 0xFF));
    
    std::string out_json;
    std::string err_msg;
    bool result = uma::ipc::protocol::try_read_frame(rx, out_json, uma::ipc::protocol::kDefaultMaxFrameBytes, &err_msg);

    ASSERT_FALSE(result);
    ASSERT_EQ(err_msg, "invalid frame length 0");
}

// Test write_frame + try_read_frame roundtrip with multiple frames
TEST(ProtocolTest, WriteReadRoundtrip) {
    std::vector<uint8_t> buf;
    std::string js1 = "{\"a\":1}";
    std::string js2 = "{\"b\":\"text\"}";
    uma::ipc::protocol::write_frame(buf, js1);
    uma::ipc::protocol::write_frame(buf, js2);

    std::string out;
    std::string err;
    ASSERT_TRUE(uma::ipc::protocol::try_read_frame(buf, out, uma::ipc::protocol::kDefaultMaxFrameBytes, &err));
    ASSERT_EQ(out, js1);
    ASSERT_TRUE(err.empty());

    out.clear(); err.clear();
    ASSERT_TRUE(uma::ipc::protocol::try_read_frame(buf, out, uma::ipc::protocol::kDefaultMaxFrameBytes, &err));
    ASSERT_EQ(out, js2);
    ASSERT_TRUE(buf.empty());
}
