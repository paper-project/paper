#include <gtest/gtest.h>
#include <paper/node.hpp>

TEST (peer_container, empty_peers)
{
    paper::peer_container peers (paper::endpoint {});
    auto list (peers.purge_list (std::chrono::system_clock::now ()));
    ASSERT_EQ (0, list.size ());
}

TEST (peer_container, no_recontact)
{
    paper::peer_container peers (paper::endpoint {});
	auto observed_peer (0);
	auto observed_disconnect (false);
    paper::endpoint endpoint1 (boost::asio::ip::address_v6::loopback (), 10000);
    ASSERT_EQ (0, peers.size ());
	peers.peer_observer = [&observed_peer] (paper::endpoint const &) {++observed_peer;};
	peers.disconnect_observer = [&observed_disconnect] () {observed_disconnect = true;};
    ASSERT_FALSE (peers.insert (endpoint1));
    ASSERT_EQ (1, peers.size ());
    ASSERT_TRUE (peers.insert (endpoint1));
	auto remaining (peers.purge_list (std::chrono::system_clock::now () + std::chrono::seconds (5)));
	ASSERT_TRUE (remaining.empty ());
	ASSERT_EQ (1, observed_peer);
	ASSERT_TRUE (observed_disconnect);
}

TEST (peer_container, no_self_incoming)
{
    paper::endpoint self (boost::asio::ip::address_v6::loopback (), 10000);
    paper::peer_container peers (self);
    peers.insert (self);
    ASSERT_TRUE (peers.peers.empty ());
}

TEST (peer_container, no_self_contacting)
{
    paper::endpoint self (boost::asio::ip::address_v6::loopback (), 10000);
    paper::peer_container peers (self);
    peers.insert (self);
    ASSERT_TRUE (peers.peers.empty ());
}

TEST (peer_container, old_known)
{
    paper::endpoint self (boost::asio::ip::address_v6::loopback (), 10000);
    paper::endpoint other (boost::asio::ip::address_v6::loopback (), 10001);
    paper::peer_container peers (self);
    ASSERT_FALSE (peers.insert (other));
    peers.peers.modify (peers.peers.begin (), [] (paper::peer_information & info) {info.last_contact = std::chrono::system_clock::time_point {};});
    ASSERT_FALSE (peers.known_peer (other));
    ASSERT_TRUE (peers.insert (other));
    ASSERT_TRUE (peers.known_peer (other));
    ASSERT_FALSE (peers.knows_about (other, paper::block_hash (1)));
    peers.insert (other, paper::block_hash (1));
    ASSERT_TRUE (peers.knows_about (other, paper::block_hash (1)));
}

TEST (peer_container, reserved_peers_no_contact)
{
    paper::peer_container peers (paper::endpoint {});
    ASSERT_TRUE (peers.insert (paper::endpoint (boost::asio::ip::address_v6::v4_mapped (boost::asio::ip::address_v4 (0x00000001)), 10000)));
    ASSERT_TRUE (peers.insert (paper::endpoint (boost::asio::ip::address_v6::v4_mapped (boost::asio::ip::address_v4 (0xc0000201)), 10000)));
    ASSERT_TRUE (peers.insert (paper::endpoint (boost::asio::ip::address_v6::v4_mapped (boost::asio::ip::address_v4 (0xc6336401)), 10000)));
    ASSERT_TRUE (peers.insert (paper::endpoint (boost::asio::ip::address_v6::v4_mapped (boost::asio::ip::address_v4 (0xcb007101)), 10000)));
    ASSERT_TRUE (peers.insert (paper::endpoint (boost::asio::ip::address_v6::v4_mapped (boost::asio::ip::address_v4 (0xe9fc0001)), 10000)));
    ASSERT_TRUE (peers.insert (paper::endpoint (boost::asio::ip::address_v6::v4_mapped (boost::asio::ip::address_v4 (0xf0000001)), 10000)));
    ASSERT_TRUE (peers.insert (paper::endpoint (boost::asio::ip::address_v6::v4_mapped (boost::asio::ip::address_v4 (0xffffffff)), 10000)));
    ASSERT_EQ (0, peers.size ());
}

TEST (peer_container, split)
{
    paper::peer_container peers (paper::endpoint {});
    auto now (std::chrono::system_clock::now ());
    paper::endpoint endpoint1 (boost::asio::ip::address_v6::any (), 100);
    paper::endpoint endpoint2 (boost::asio::ip::address_v6::any (), 101);
	peers.peers.insert ({endpoint1, now - std::chrono::seconds (1), now});
    peers.peers.insert ({endpoint2, now + std::chrono::seconds (1), now});
	ASSERT_EQ (2, peers.peers.size ());
    auto list (peers.purge_list (now));
	ASSERT_EQ (1, peers.peers.size ());
    ASSERT_EQ (1, list.size ());
    ASSERT_EQ (endpoint2, list [0].endpoint);
}

TEST (peer_container, fill_random_clear)
{
    paper::peer_container peers (paper::endpoint {});
    std::array <paper::endpoint, 8> target;
    std::fill (target.begin (), target.end (), paper::endpoint (boost::asio::ip::address_v6::loopback (), 10000));
    peers.random_fill (target);
    ASSERT_TRUE (std::all_of (target.begin (), target.end (), [] (paper::endpoint const & endpoint_a) {return endpoint_a == paper::endpoint (boost::asio::ip::address_v6::any (), 0); }));
}

TEST (peer_container, fill_random_full)
{
    paper::peer_container peers (paper::endpoint {});
    for (auto i (0); i < 100; ++i)
    {
        peers.insert (paper::endpoint (boost::asio::ip::address_v6::loopback (), i));
    }
    std::array <paper::endpoint, 8> target;
    std::fill (target.begin (), target.end (), paper::endpoint (boost::asio::ip::address_v6::loopback (), 10000));
    peers.random_fill (target);
    ASSERT_TRUE (std::none_of (target.begin (), target.end (), [] (paper::endpoint const & endpoint_a) {return endpoint_a == paper::endpoint (boost::asio::ip::address_v6::loopback (), 10000); }));
}

TEST (peer_container, fill_random_part)
{
    paper::peer_container peers (paper::endpoint {});
    std::array <paper::endpoint, 8> target;
    auto half (target.size () / 2);
    for (auto i (0); i < half; ++i)
    {
        peers.insert (paper::endpoint (boost::asio::ip::address_v6::loopback (), i + 1));
    }
    std::fill (target.begin (), target.end (), paper::endpoint (boost::asio::ip::address_v6::loopback (), 10000));
    peers.random_fill (target);
    ASSERT_TRUE (std::none_of (target.begin (), target.begin () + half, [] (paper::endpoint const & endpoint_a) {return endpoint_a == paper::endpoint (boost::asio::ip::address_v6::loopback (), 10000); }));
    ASSERT_TRUE (std::none_of (target.begin (), target.begin () + half, [] (paper::endpoint const & endpoint_a) {return endpoint_a == paper::endpoint (boost::asio::ip::address_v6::loopback (), 0); }));
    ASSERT_TRUE (std::all_of (target.begin () + half, target.end (), [] (paper::endpoint const & endpoint_a) {return endpoint_a == paper::endpoint (boost::asio::ip::address_v6::any (), 0); }));
}

TEST (peer_container, bootstrap_limit)
{
    paper::peer_container peers (paper::endpoint {});
	auto one (paper::endpoint (boost::asio::ip::address_v6::loopback(), 2048));
	peers.insert (one);
	auto list0 (peers.bootstrap_candidates ());
	ASSERT_EQ (1, list0.size ());
	ASSERT_EQ (one, list0 [0].endpoint);
	peers.bootstrap_failed (one);
	auto list1 (peers.bootstrap_candidates ());
	ASSERT_EQ (0, list1.size ());
}