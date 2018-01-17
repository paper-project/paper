#include <boost/property_tree/json_parser.hpp>

#include <ed25519-donna/ed25519.h>

#include <fstream>

#include <gtest/gtest.h>

#include <paper/node.hpp>

TEST (ed25519, signing)
{
    paper::uint256_union prv (0);
    paper::uint256_union pub;
    ed25519_publickey (prv.bytes.data (), pub.bytes.data ());
    paper::uint256_union message (0);
    paper::uint512_union signature;
    ed25519_sign (message.bytes.data (), sizeof (message.bytes), prv.bytes.data (), pub.bytes.data (), signature.bytes.data ());
    auto valid1 (ed25519_sign_open (message.bytes.data (), sizeof (message.bytes), pub.bytes.data (), signature.bytes.data ()));
    ASSERT_EQ (0, valid1);
    signature.bytes [32] ^= 0x1;
    auto valid2 (ed25519_sign_open (message.bytes.data (), sizeof (message.bytes), pub.bytes.data (), signature.bytes.data ()));
    ASSERT_NE (0, valid2);
}

TEST (transaction_block, empty)
{
    paper::keypair key1;
    paper::send_block block (0, 1, 13, key1.prv, key1.pub, 2);
    paper::uint256_union hash (block.hash ());
    ASSERT_FALSE (paper::validate_message (key1.pub, hash, block.signature));
    block.signature.bytes [32] ^= 0x1;
    ASSERT_TRUE (paper::validate_message (key1.pub, hash, block.signature));
}

TEST (block, send_serialize)
{
    paper::send_block block1 (0, 1, 2, 3, 4, 5);
    std::vector <uint8_t> bytes;
    {
        paper::vectorstream stream1 (bytes);
        block1.serialize (stream1);
    }
    auto data (bytes.data ());
    auto size (bytes.size ());
    ASSERT_NE (nullptr, data);
    ASSERT_NE (0, size);
    paper::bufferstream stream2 (data, size);
	bool error;
    paper::send_block block2 (error, stream2);
    ASSERT_FALSE (error);
    ASSERT_EQ (block1, block2);
}

TEST (block, send_serialize_json)
{
    paper::send_block block1 (0, 1, 2, 3, 4, 5);
    std::string string1;
    block1.serialize_json (string1);
    ASSERT_NE (0, string1.size ());
    boost::property_tree::ptree tree1;
    std::stringstream istream (string1);
    boost::property_tree::read_json (istream, tree1);
	bool error;
    paper::send_block block2 (error, tree1);
    ASSERT_FALSE (error);
    ASSERT_EQ (block1, block2);
}

TEST (block, receive_serialize)
{
    paper::receive_block block1 (0, 1, 2, 3, 4);
    paper::keypair key1;
    std::vector <uint8_t> bytes;
    {
        paper::vectorstream stream1 (bytes);
        block1.serialize (stream1);
    }
    paper::bufferstream stream2 (bytes.data (), bytes.size ());
	bool error;
    paper::receive_block block2 (error, stream2);
	ASSERT_FALSE (error);
    ASSERT_EQ (block1, block2);
}

TEST (block, receive_serialize_json)
{
    paper::receive_block block1 (0, 1, 2, 3, 4);
    std::string string1;
    block1.serialize_json (string1);
    ASSERT_NE (0, string1.size ());
    boost::property_tree::ptree tree1;
    std::stringstream istream (string1);
    boost::property_tree::read_json (istream, tree1);
	bool error;
    paper::receive_block block2 (error, tree1);
    ASSERT_FALSE (error);
    ASSERT_EQ (block1, block2);
}

TEST (block, open_serialize_json)
{
    paper::open_block block1 (0, 1, 0, 0, 0, 0);
    std::string string1;
    block1.serialize_json (string1);
    ASSERT_NE (0, string1.size ());
    boost::property_tree::ptree tree1;
    std::stringstream istream (string1);
    boost::property_tree::read_json (istream, tree1);
	bool error;
    paper::open_block block2 (error, tree1);
	ASSERT_FALSE (error);
    ASSERT_EQ (block1, block2);
}

TEST (block, change_serialize_json)
{
    paper::change_block block1 (0, 1, 2, 3, 4);
    std::string string1;
    block1.serialize_json (string1);
    ASSERT_NE (0, string1.size ());
    boost::property_tree::ptree tree1;
    std::stringstream istream (string1);
    boost::property_tree::read_json (istream, tree1);
    bool error;
    paper::change_block block2 (error, tree1);
    ASSERT_FALSE (error);
    ASSERT_EQ (block1, block2);
}

TEST (uint512_union, parse_zero)
{
    paper::uint512_union input (paper::uint512_t (0));
    std::string text;
    input.encode_hex (text);
    paper::uint512_union output;
    auto error (output.decode_hex (text));
    ASSERT_FALSE (error);
    ASSERT_EQ (input, output);
    ASSERT_TRUE (output.number ().is_zero ());
}

TEST (uint512_union, parse_zero_short)
{
    std::string text ("0");
    paper::uint512_union output;
    auto error (output.decode_hex (text));
    ASSERT_FALSE (error);
    ASSERT_TRUE (output.number ().is_zero ());
}

TEST (uint512_union, parse_one)
{
    paper::uint512_union input (paper::uint512_t (1));
    std::string text;
    input.encode_hex (text);
    paper::uint512_union output;
    auto error (output.decode_hex (text));
    ASSERT_FALSE (error);
    ASSERT_EQ (input, output);
    ASSERT_EQ (1, output.number ());
}

TEST (uint512_union, parse_error_symbol)
{
    paper::uint512_union input (paper::uint512_t (1000));
    std::string text;
    input.encode_hex (text);
    text [5] = '!';
    paper::uint512_union output;
    auto error (output.decode_hex (text));
    ASSERT_TRUE (error);
}

TEST (uint512_union, max)
{
    paper::uint512_union input (std::numeric_limits <paper::uint512_t>::max ());
    std::string text;
    input.encode_hex (text);
    paper::uint512_union output;
    auto error (output.decode_hex (text));
    ASSERT_FALSE (error);
    ASSERT_EQ (input, output);
    ASSERT_EQ (paper::uint512_t ("0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"), output.number ());
}

TEST (uint512_union, parse_error_overflow)
{
    paper::uint512_union input (std::numeric_limits <paper::uint512_t>::max ());
    std::string text;
    input.encode_hex (text);
    text.push_back (0);
    paper::uint512_union output;
    auto error (output.decode_hex (text));
    ASSERT_TRUE (error);
}

TEST (send_block, deserialize)
{
    paper::send_block block1 (0, 1, 2, 3, 4, 5);
	ASSERT_EQ (block1.hash (), block1.hash ());
    std::vector <uint8_t> bytes;
    {
        paper::vectorstream stream1 (bytes);
        block1.serialize (stream1);
    }
    ASSERT_EQ (paper::send_block::size, bytes.size ());
    paper::bufferstream stream2 (bytes.data (), bytes.size ());
	bool error;
    paper::send_block block2 (error, stream2);
    ASSERT_FALSE (error);
    ASSERT_EQ (block1, block2);
}

TEST (receive_block, deserialize)
{
    paper::receive_block block1 (0, 1, 2, 3, 4);
	ASSERT_EQ (block1.hash (), block1.hash ());
    block1.hashables.previous = 2;
    block1.hashables.source = 4;
    std::vector <uint8_t> bytes;
    {
        paper::vectorstream stream1 (bytes);
        block1.serialize (stream1);
    }
    ASSERT_EQ (paper::receive_block::size, bytes.size ());
    paper::bufferstream stream2 (bytes.data (), bytes.size ());
	bool error;
    paper::receive_block block2 (error, stream2);
    ASSERT_FALSE (error);
    ASSERT_EQ (block1, block2);
}

TEST (open_block, deserialize)
{
    paper::open_block block1 (0, 1, 0, 0, 0, 0);
	ASSERT_EQ (block1.hash (), block1.hash ());
    std::vector <uint8_t> bytes;
    {
        paper::vectorstream stream (bytes);
        block1.serialize (stream);
    }
    ASSERT_EQ (paper::open_block::size, bytes.size ());
    paper::bufferstream stream (bytes.data (), bytes.size ());
	bool error;
    paper::open_block block2 (error, stream);
	ASSERT_FALSE (error);
    ASSERT_EQ (block1, block2);
}

TEST (change_block, deserialize)
{
    paper::change_block block1 (1, 2, 3, 4, 5);
	ASSERT_EQ (block1.hash (), block1.hash ());
    std::vector <uint8_t> bytes;
    {
        paper::vectorstream stream1 (bytes);
        block1.serialize (stream1);
    }
    ASSERT_EQ (paper::change_block::size, bytes.size ());
    auto data (bytes.data ());
    auto size (bytes.size ());
    ASSERT_NE (nullptr, data);
    ASSERT_NE (0, size);
    paper::bufferstream stream2 (data, size);
    bool error;
    paper::change_block block2 (error, stream2);
    ASSERT_FALSE (error);
    ASSERT_EQ (block1, block2);
}

TEST (frontier_req, serialization)
{
    paper::frontier_req request1;
    request1.start = 1;
    request1.age = 2;
    request1.count = 3;
    std::vector <uint8_t> bytes;
    {
        paper::vectorstream stream (bytes);
        request1.serialize (stream);
    }
    paper::bufferstream buffer (bytes.data (), bytes.size ());
    paper::frontier_req request2;
    ASSERT_FALSE (request2.deserialize (buffer));
    ASSERT_EQ (request1, request2);
}

TEST (work, one)
{
	paper::work_pool pool;
    paper::change_block block (1, 1, 2, 3, 4);
    pool.generate (block);
    ASSERT_FALSE (paper::work_validate (block));
    pool.generate (block);
    ASSERT_FALSE (paper::work_validate (block));
}

TEST (work, validate)
{
	paper::work_pool work;
	paper::send_block send_block (1, 1, 2, 3, 4, 5);
    ASSERT_TRUE (paper::work_validate (send_block));
    work.generate (send_block);
    ASSERT_FALSE (paper::work_validate (send_block));
}

TEST (block, publish_req_serialization)
{
    paper::keypair key1;
    paper::keypair key2;
    auto block (std::unique_ptr <paper::send_block> (new paper::send_block (0, key2.pub, 200, 1, 2, 3)));
    paper::publish req (std::move (block));
    std::vector <uint8_t> bytes;
    {
        paper::vectorstream stream (bytes);
        req.serialize (stream);
    }
    paper::publish req2;
    paper::bufferstream stream2 (bytes.data (), bytes.size ());
    auto error (req2.deserialize (stream2));
    ASSERT_FALSE (error);
    ASSERT_EQ (req, req2);
    ASSERT_EQ (*req.block, *req2.block);
}

TEST (block, confirm_req_serialization)
{
    paper::keypair key1;
    paper::keypair key2;
    auto block (std::unique_ptr <paper::send_block> (new paper::send_block (0, key2.pub, 200, 1, 2, 3)));
    paper::confirm_req req (std::move (block));
    std::vector <uint8_t> bytes;
    {
        paper::vectorstream stream (bytes);
        req.serialize (stream);
    }
    paper::confirm_req req2;
    paper::bufferstream stream2 (bytes.data (), bytes.size ());
    auto error (req2.deserialize (stream2));
    ASSERT_FALSE (error);
    ASSERT_EQ (req, req2);
    ASSERT_EQ (*req.block, *req2.block);
}