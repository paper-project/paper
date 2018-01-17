#include <gtest/gtest.h>
#include <paper/node.hpp>

TEST (message, keepalive_serialization)
{
    paper::keepalive request1;
    std::vector <uint8_t> bytes;
    {
        paper::vectorstream stream (bytes);
        request1.serialize (stream);
    }
    paper::keepalive request2;
    paper::bufferstream buffer (bytes.data (), bytes.size ());
    ASSERT_FALSE (request2.deserialize (buffer));
    ASSERT_EQ (request1, request2);
}

TEST (message, keepalive_deserialize)
{
    paper::keepalive message1;
    message1.peers [0] = paper::endpoint (boost::asio::ip::address_v6::loopback (), 10000);
    std::vector <uint8_t> bytes;
    {
        paper::vectorstream stream (bytes);
        message1.serialize (stream);
    }
    uint8_t version_max;
    uint8_t version_using;
    uint8_t version_min;
	paper::message_type type;
	std::bitset <16> extensions;
    paper::bufferstream header_stream (bytes.data (), bytes.size ());
    ASSERT_FALSE (paper::message::read_header (header_stream, version_max, version_using, version_min, type, extensions));
    ASSERT_EQ (paper::message_type::keepalive, type);
    paper::keepalive message2;
    paper::bufferstream stream (bytes.data (), bytes.size ());
    ASSERT_FALSE (message2.deserialize (stream));
    ASSERT_EQ (message1.peers, message2.peers);
}

TEST (message, publish_serialization)
{
    paper::publish publish (std::unique_ptr <paper::block> (new paper::send_block (0, 1, 2, 3, 4, 5)));
    ASSERT_EQ (paper::block_type::send, publish.block_type ());
    ASSERT_FALSE (publish.ipv4_only ());
    publish.ipv4_only_set (true);
    ASSERT_TRUE (publish.ipv4_only ());
    std::vector <uint8_t> bytes;
    {
        paper::vectorstream stream (bytes);
        publish.write_header (stream);
    }
    ASSERT_EQ (8, bytes.size ());
    ASSERT_EQ (0x52, bytes [0]);
    ASSERT_EQ (0x41, bytes [1]);
    ASSERT_EQ (0x01, bytes [2]);
    ASSERT_EQ (0x01, bytes [3]);
    ASSERT_EQ (0x01, bytes [4]);
    ASSERT_EQ (static_cast <uint8_t> (paper::message_type::publish), bytes [5]);
    ASSERT_EQ (0x02, bytes [6]);
    ASSERT_EQ (static_cast <uint8_t> (paper::block_type::send), bytes [7]);
    paper::bufferstream stream (bytes.data (), bytes.size ());
    uint8_t version_max;
    uint8_t version_using;
    uint8_t version_min;
    paper::message_type type;
    std::bitset <16> extensions;
    ASSERT_FALSE (paper::message::read_header (stream, version_max, version_using, version_min, type, extensions));
    ASSERT_EQ (0x01, version_min);
    ASSERT_EQ (0x01, version_using);
    ASSERT_EQ (0x01, version_max);
    ASSERT_EQ (paper::message_type::publish, type);
}

TEST (message, confirm_ack_serialization)
{
    paper::keypair key1;
    paper::confirm_ack con1 (key1.pub, key1.prv, 0, std::unique_ptr <paper::block> (new paper::send_block (0, 1, 2, 3, 4, 5)));
    std::vector <uint8_t> bytes;
    {
        paper::vectorstream stream1 (bytes);
        con1.serialize (stream1);
    }
    paper::bufferstream stream2 (bytes.data (), bytes.size ());
	bool error;
    paper::confirm_ack con2 (error, stream2);
	ASSERT_FALSE (error);
    ASSERT_EQ (con1, con2);
}