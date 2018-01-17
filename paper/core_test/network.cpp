#include <gtest/gtest.h>
#include <boost/thread.hpp>
#include <paper/node.hpp>

TEST (network, tcp_connection)
{
    boost::asio::io_service service;
    boost::asio::ip::tcp::acceptor acceptor (service);
    boost::asio::ip::tcp::endpoint endpoint (boost::asio::ip::address_v4::any (), 24000);
    acceptor.open (endpoint.protocol ());
    acceptor.set_option (boost::asio::ip::tcp::acceptor::reuse_address (true));
    acceptor.bind (endpoint);
    acceptor.listen ();
    boost::asio::ip::tcp::socket incoming (service);
    auto done1 (false);
    std::string message1;
    acceptor.async_accept (incoming, 
       [&done1, &message1] (boost::system::error_code const & ec_a)
       {
           if (ec_a)
           {
               message1 = ec_a.message ();
               std::cerr << message1;
           }
           done1 = true;}
       );
    boost::asio::ip::tcp::socket connector (service);
    auto done2 (false);
    std::string message2;
    connector.async_connect (boost::asio::ip::tcp::endpoint (boost::asio::ip::address_v4::loopback (), 24000), 
        [&done2, &message2] (boost::system::error_code const & ec_a)
        {
            if (ec_a)
            {
                message2 = ec_a.message ();
                std::cerr << message2;
            }
            done2 = true;
        });
    while (!done1 || !done2)
    {
        service.poll ();
    }
    ASSERT_EQ (0, message1.size ());
    ASSERT_EQ (0, message2.size ());
}

TEST (network, construction)
{
    paper::system system (24000, 1);
    ASSERT_EQ (1, system.nodes.size ());
    ASSERT_EQ (24000, system.nodes [0]->network.socket.local_endpoint ().port ());
}

TEST (network, self_discard)
{
    paper::system system (24000, 1);
	system.nodes [0]->network.remote = system.nodes [0]->network.endpoint ();
	ASSERT_EQ (0, system.nodes [0]->network.bad_sender_count);
	system.nodes [0]->network.receive_action (boost::system::error_code {}, 0);
	ASSERT_EQ (1, system.nodes [0]->network.bad_sender_count);
}

TEST (network, send_keepalive)
{
    paper::system system (24000, 1);
    auto list1 (system.nodes [0]->peers.list ());
    ASSERT_EQ (0, list1.size ());
    paper::node_init init1;
    auto node1 (std::make_shared <paper::node> (init1, system.service, 24001, paper::unique_path (), system.processor, system.logging, system.work));
    node1->start ();
    system.nodes [0]->network.send_keepalive (node1->network.endpoint ());
    auto initial (system.nodes [0]->network.keepalive_count);
    ASSERT_EQ (0, system.nodes [0]->peers.list ().size ());
    ASSERT_EQ (0, node1->peers.list ().size ());
    auto iterations (0);
    while (system.nodes [0]->network.keepalive_count == initial)
    {
        system.poll ();
        ++iterations;
        ASSERT_LT (iterations, 200);
    }
    auto peers1 (system.nodes [0]->peers.list ());
    auto peers2 (node1->peers.list ());
    ASSERT_EQ (1, peers1.size ());
    ASSERT_EQ (1, peers2.size ());
    ASSERT_NE (peers1.end (), std::find_if (peers1.begin (), peers1.end (), [&node1] (paper::peer_information const & information_a) {return information_a.endpoint == node1->network.endpoint ();}));
    ASSERT_NE (peers2.end (), std::find_if (peers2.begin (), peers2.end (), [&system] (paper::peer_information const & information_a) {return information_a.endpoint == system.nodes [0]->network.endpoint ();}));
    node1->stop ();
}

TEST (network, keepalive_ipv4)
{
    paper::system system (24000, 1);
    auto list1 (system.nodes [0]->peers.list ());
    ASSERT_EQ (0, list1.size ());
    paper::node_init init1;
    auto node1 (std::make_shared <paper::node> (init1, system.service, 24001, paper::unique_path (), system.processor, system.logging, system.work));
    node1->start ();
    node1->send_keepalive (paper::endpoint (boost::asio::ip::address_v4::loopback (), 24000));
    auto initial (system.nodes [0]->network.keepalive_count);
    auto iterations (0);
    while (system.nodes [0]->network.keepalive_count == initial)
    {
        system.poll ();
        ++iterations;
        ASSERT_LT (iterations, 200);
    }
    node1->stop ();
}

TEST (network, multi_keepalive)
{
    paper::system system (24000, 1);
    auto list1 (system.nodes [0]->peers.list ());
    ASSERT_EQ (0, list1.size ());
    paper::node_init init1;
    auto node1 (std::make_shared <paper::node> (init1, system.service, 24001, paper::unique_path (), system.processor, system.logging, system.work));
    ASSERT_FALSE (init1.error ());
    node1->start ();
    ASSERT_EQ (0, node1->peers.size ());
    node1->network.send_keepalive (system.nodes [0]->network.endpoint ());
    ASSERT_EQ (0, node1->peers.size ());
    ASSERT_EQ (0, system.nodes [0]->peers.size ());
    auto iterations1 (0);
    while (system.nodes [0]->peers.size () != 1)
    {
        system.poll ();
        ++iterations1;
        ASSERT_LT (iterations1, 200);
    }
    paper::node_init init2;
    auto node2 (std::make_shared <paper::node> (init2, system.service, 24002, paper::unique_path (), system.processor, system.logging, system.work));
    ASSERT_FALSE (init2.error ());
    node2->start ();
    node2->network.send_keepalive (system.nodes [0]->network.endpoint ());
    auto iterations2 (0);
    while (node1->peers.size () != 2 || system.nodes [0]->peers.size () != 2 || node2->peers.size () != 2)
    {
        system.poll ();
        ++iterations2;
        ASSERT_LT (iterations2, 200);
    }
    node1->stop ();
    node2->stop ();
}

TEST (network, send_discarded_publish)
{
    paper::system system (24000, 2);
    std::unique_ptr <paper::send_block> block (new paper::send_block (1, 1, 2, 3, 4, system.work.generate (1)));
    system.nodes [0]->network.republish_block (std::move (block), 0);
    paper::genesis genesis;
    ASSERT_EQ (genesis.hash (), system.nodes [0]->latest (paper::test_genesis_key.pub));
    ASSERT_EQ (genesis.hash (), system.nodes [1]->latest (paper::test_genesis_key.pub));
    auto iterations (0);
    while (system.nodes [1]->network.publish_count == 0)
    {
        system.poll ();
        ++iterations;
        ASSERT_LT (iterations, 200);
    }
    ASSERT_EQ (genesis.hash (), system.nodes [0]->latest (paper::test_genesis_key.pub));
    ASSERT_EQ (genesis.hash (), system.nodes [1]->latest (paper::test_genesis_key.pub));
}

TEST (network, send_invalid_publish)
{
    paper::system system (24000, 2);
    std::unique_ptr <paper::send_block> block (new paper::send_block (1, 1, 20, paper::test_genesis_key.prv, paper::test_genesis_key.pub, system.work.generate (1)));
    system.nodes [0]->network.republish_block (std::move (block), 0);
    paper::genesis genesis;
    ASSERT_EQ (genesis.hash (), system.nodes [0]->latest (paper::test_genesis_key.pub));
    ASSERT_EQ (genesis.hash (), system.nodes [1]->latest (paper::test_genesis_key.pub));
    auto iterations (0);
    while (system.nodes [1]->network.publish_count == 0)
    {
        system.poll ();
        ++iterations;
        ASSERT_LT (iterations, 200);
    }
    ASSERT_EQ (genesis.hash (), system.nodes [0]->latest (paper::test_genesis_key.pub));
    ASSERT_EQ (genesis.hash (), system.nodes [1]->latest (paper::test_genesis_key.pub));
}

TEST (network, send_valid_confirm_ack)
{
    paper::system system (24000, 2);
    paper::keypair key2;
	system.wallet (0)->insert (paper::test_genesis_key.prv);
	system.wallet (1)->insert (key2.prv);
    paper::block_hash latest1 (system.nodes [0]->latest (paper::test_genesis_key.pub));
    paper::send_block block2 (latest1, key2.pub, 50, paper::test_genesis_key.prv, paper::test_genesis_key.pub, system.work.generate (latest1));
    auto hash2 (block2.hash ());
    paper::block_hash latest2 (system.nodes [1]->latest (paper::test_genesis_key.pub));
	system.nodes [0]->process_receive_republish (std::unique_ptr <paper::block> (new paper::send_block (block2)), 0);
    auto iterations (0);
    while (system.nodes [1]->network.confirm_ack_count == 0)
    {
        system.poll ();
        ++iterations;
        ASSERT_LT (iterations, 200);
    }
    paper::block_hash latest3 (system.nodes [1]->latest (paper::test_genesis_key.pub));
    ASSERT_NE (latest2, latest3);
    ASSERT_EQ (hash2, latest3);
    ASSERT_EQ (50, system.nodes [1]->balance (paper::test_genesis_key.pub));
}

TEST (network, send_valid_publish)
{
    paper::system system (24000, 2);
	system.wallet (0)->insert (paper::test_genesis_key.prv);
    paper::keypair key2;
	system.wallet (1)->insert (key2.prv);
    paper::block_hash latest1 (system.nodes [0]->latest (paper::test_genesis_key.pub));
    paper::send_block block2 (latest1, key2.pub, 50, paper::test_genesis_key.prv, paper::test_genesis_key.pub, system.work.generate (latest1));
    auto hash2 (block2.hash ());
    paper::block_hash latest2 (system.nodes [1]->latest (paper::test_genesis_key.pub));
    system.nodes [1]->process_receive_republish (std::unique_ptr <paper::block> (new paper::send_block (block2)), 0);
    auto iterations (0);
    while (system.nodes [0]->network.publish_count == 0)
    {
        system.poll ();
        ++iterations;
        ASSERT_LT (iterations, 200);
    }
    paper::block_hash latest3 (system.nodes [1]->latest (paper::test_genesis_key.pub));
    ASSERT_NE (latest2, latest3);
    ASSERT_EQ (hash2, latest3);
    ASSERT_EQ (50, system.nodes [1]->balance (paper::test_genesis_key.pub));
}

TEST (network, send_insufficient_work)
{
    paper::system system (24000, 2);
    std::unique_ptr <paper::send_block> block (new paper::send_block (0, 1, 20, paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0));
    paper::publish publish (std::move (block));
    std::shared_ptr <std::vector <uint8_t>> bytes (new std::vector <uint8_t>);
    {
        paper::vectorstream stream (*bytes);
        publish.serialize (stream);
    }
    auto node1 (system.nodes [1]->shared ());
    system.nodes [0]->network.send_buffer (bytes->data (), bytes->size (), system.nodes [1]->network.endpoint (), 0, [bytes, node1] (boost::system::error_code const & ec, size_t size) {});
    ASSERT_EQ (0, system.nodes [0]->network.insufficient_work_count);
    auto iterations (0);
    while (system.nodes [1]->network.insufficient_work_count == 0)
    {
        system.poll ();
        ++iterations;
        ASSERT_LT (iterations, 200);
    }
    ASSERT_EQ (1, system.nodes [1]->network.insufficient_work_count);
}

TEST (receivable_processor, confirm_insufficient_pos)
{
    paper::system system (24000, 1);
    auto & node1 (*system.nodes [0]);
    paper::genesis genesis;
    paper::send_block block1 (genesis.hash (), 0, 0, paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0);
	ASSERT_EQ (paper::process_result::progress, node1.process (block1).code);
	auto node_l (system.nodes [0]);
    node1.conflicts.start (block1, [node_l] (paper::block & block_a)
	{
		node_l->process_confirmed (block_a);
	}, true);
    paper::keypair key1;
    paper::confirm_ack con1 (key1.pub, key1.prv, 0, block1.clone ());
	node1.process_message (con1, node1.network.endpoint ());
}

TEST (receivable_processor, confirm_sufficient_pos)
{
    paper::system system (24000, 1);
    auto & node1 (*system.nodes [0]);
    paper::genesis genesis;
    paper::send_block block1 (genesis.hash (), 0, 0, paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0);
	ASSERT_EQ (paper::process_result::progress, node1.process (block1).code);
	auto node_l (system.nodes [0]);
    node1.conflicts.start (block1, [node_l] (paper::block & block_a)
	{
		node_l->process_confirmed (block_a);
	}, true);
    paper::confirm_ack con1 (paper::test_genesis_key.pub, paper::test_genesis_key.prv, 0, block1.clone ());
	node1.process_message (con1, node1.network.endpoint ());
}

TEST (receivable_processor, send_with_receive)
{
    auto amount (std::numeric_limits <paper::uint128_t>::max ());
    paper::system system (24000, 2);
    paper::keypair key2;
	system.wallet (0)->insert (paper::test_genesis_key.prv);
    paper::block_hash latest1 (system.nodes [0]->latest (paper::test_genesis_key.pub));
	system.wallet (1)->insert (key2.prv);
    auto block1 (new paper::send_block (latest1, key2.pub, amount - 100, paper::test_genesis_key.prv, paper::test_genesis_key.pub, system.work.generate (latest1)));
	ASSERT_EQ (amount, system.nodes [0]->balance (paper::test_genesis_key.pub));
	ASSERT_EQ (0, system.nodes [0]->balance (key2.pub));
	ASSERT_EQ (amount, system.nodes [1]->balance (paper::test_genesis_key.pub));
	ASSERT_EQ (0, system.nodes [1]->balance (key2.pub));
    system.nodes [0]->process_receive_republish (block1->clone (), 0);
    system.nodes [1]->process_receive_republish (block1->clone (), 0);
	ASSERT_EQ (amount - 100, system.nodes [0]->balance (paper::test_genesis_key.pub));
	ASSERT_EQ (0, system.nodes [0]->balance (key2.pub));
	ASSERT_EQ (amount - 100, system.nodes [1]->balance (paper::test_genesis_key.pub));
	ASSERT_EQ (0, system.nodes [1]->balance (key2.pub));
    auto iterations (0);
    while (system.nodes [0]->balance (key2.pub) != 100)
    {
        system.poll ();
        ++iterations;
        ASSERT_LT (iterations, 200);
    }
	ASSERT_EQ (amount - 100, system.nodes [0]->balance (paper::test_genesis_key.pub));
	ASSERT_EQ (100, system.nodes [0]->balance (key2.pub));
	ASSERT_EQ (amount - 100, system.nodes [1]->balance (paper::test_genesis_key.pub));
	ASSERT_EQ (100, system.nodes [1]->balance (key2.pub));
}

TEST (network, receive_weight_change)
{
    paper::system system (24000, 2);
	system.wallet (0)->insert (paper::test_genesis_key.prv);
    paper::keypair key2;
	system.wallet (1)->insert (key2.prv);
	{
		paper::transaction transaction (system.nodes [1]->store.environment, nullptr, true);
		system.wallet (1)->store.representative_set (transaction, key2.pub);
	}
    ASSERT_FALSE (system.wallet (0)->send_sync (paper::test_genesis_key.pub, key2.pub, 2));
	auto iterations (0);
    while (std::any_of (system.nodes.begin (), system.nodes.end (), [&] (std::shared_ptr <paper::node> const & node_a){ return node_a->weight (key2.pub) != 2;}))
    {
        system.poll ();
		++iterations;
		ASSERT_LT (iterations, 200);
    }
}

TEST (parse_endpoint, valid)
{
    std::string string ("::1:24000");
    paper::endpoint endpoint;
    ASSERT_FALSE (paper::parse_endpoint (string, endpoint));
    ASSERT_EQ (boost::asio::ip::address_v6::loopback (), endpoint.address ());
    ASSERT_EQ (24000, endpoint.port ());
}

TEST (parse_endpoint, invalid_port)
{
    std::string string ("::1:24a00");
    paper::endpoint endpoint;
    ASSERT_TRUE (paper::parse_endpoint (string, endpoint));
}

TEST (parse_endpoint, invalid_address)
{
    std::string string ("::q:24000");
    paper::endpoint endpoint;
    ASSERT_TRUE (paper::parse_endpoint (string, endpoint));
}

TEST (parse_endpoint, no_address)
{
    std::string string (":24000");
    paper::endpoint endpoint;
    ASSERT_TRUE (paper::parse_endpoint (string, endpoint));
}

TEST (parse_endpoint, no_port)
{
    std::string string ("::1:");
    paper::endpoint endpoint;
    ASSERT_TRUE (paper::parse_endpoint (string, endpoint));
}

TEST (parse_endpoint, no_colon)
{
    std::string string ("::1");
    paper::endpoint endpoint;
    ASSERT_TRUE (paper::parse_endpoint (string, endpoint));
}

// If the account doesn't exist, current == end so there's no iteration
TEST (bulk_pull, no_address)
{
    paper::system system (24000, 1);
    auto connection (std::make_shared <paper::bootstrap_server> (nullptr, system.nodes [0]));
    std::unique_ptr <paper::bulk_pull> req (new paper::bulk_pull);
    req->start = 1;
    req->end = 2;
    connection->requests.push (std::unique_ptr <paper::message> {});
    auto request (std::make_shared <paper::bulk_pull_server> (connection, std::move (req)));
    ASSERT_EQ (request->current, request->request->end);
    ASSERT_TRUE (request->current.is_zero ());
}

TEST (bulk_pull, genesis_to_end)
{
    paper::system system (24000, 1);
    auto connection (std::make_shared <paper::bootstrap_server> (nullptr, system.nodes [0]));
    std::unique_ptr <paper::bulk_pull> req (new paper::bulk_pull {});
    req->start = paper::test_genesis_key.pub;
    req->end.clear ();
    connection->requests.push (std::unique_ptr <paper::message> {});
    auto request (std::make_shared <paper::bulk_pull_server> (connection, std::move (req)));
    ASSERT_EQ (system.nodes [0]->latest (paper::test_genesis_key.pub), request->current);
    ASSERT_EQ (request->request->end, request->request->end);
}

// If we can't find the end block, send everything
TEST (bulk_pull, no_end)
{
    paper::system system (24000, 1);
    auto connection (std::make_shared <paper::bootstrap_server> (nullptr, system.nodes [0]));
    std::unique_ptr <paper::bulk_pull> req (new paper::bulk_pull {});
    req->start = paper::test_genesis_key.pub;
    req->end = 1;
    connection->requests.push (std::unique_ptr <paper::message> {});
    auto request (std::make_shared <paper::bulk_pull_server> (connection, std::move (req)));
    ASSERT_EQ (system.nodes [0]->latest (paper::test_genesis_key.pub), request->current);
	ASSERT_TRUE (request->request->end.is_zero ());
}

TEST (bulk_pull, end_not_owned)
{
    paper::system system (24000, 1);
    paper::keypair key2;
	system.wallet (0)->insert (paper::test_genesis_key.prv);
    ASSERT_FALSE (system.wallet (0)->send_sync (paper::test_genesis_key.pub, key2.pub, 100));
	paper::block_hash latest (system.nodes [0]->latest (paper::test_genesis_key.pub));
    paper::open_block open (0, 1, 2, 3, 4, 5);
    open.hashables.account = key2.pub;
    open.hashables.representative = key2.pub;
    open.hashables.source = latest;
    open.signature = paper::sign_message (key2.prv, key2.pub, open.hash ());
	ASSERT_EQ (paper::process_result::progress, system.nodes [0]->process (open).code);
    auto connection (std::make_shared <paper::bootstrap_server> (nullptr, system.nodes [0]));
    paper::genesis genesis;
    std::unique_ptr <paper::bulk_pull> req (new paper::bulk_pull {});
    req->start = key2.pub;
    req->end = genesis.hash ();
    connection->requests.push (std::unique_ptr <paper::message> {});
    auto request (std::make_shared <paper::bulk_pull_server> (connection, std::move (req)));
    ASSERT_EQ (request->current, request->request->end);
}

TEST (bulk_pull, none)
{
    paper::system system (24000, 1);
    auto connection (std::make_shared <paper::bootstrap_server> (nullptr, system.nodes [0]));
    paper::genesis genesis;
    std::unique_ptr <paper::bulk_pull> req (new paper::bulk_pull {});
    req->start = genesis.hash ();
    req->end = genesis.hash ();
    connection->requests.push (std::unique_ptr <paper::message> {});
    auto request (std::make_shared <paper::bulk_pull_server> (connection, std::move (req)));
    auto block (request->get_next ());
    ASSERT_EQ (nullptr, block);
}

TEST (bulk_pull, get_next_on_open)
{
    paper::system system (24000, 1);
    auto connection (std::make_shared <paper::bootstrap_server> (nullptr, system.nodes [0]));
    std::unique_ptr <paper::bulk_pull> req (new paper::bulk_pull {});
    req->start = paper::test_genesis_key.pub;
    req->end.clear ();
    connection->requests.push (std::unique_ptr <paper::message> {});
    auto request (std::make_shared <paper::bulk_pull_server> (connection, std::move (req)));
    auto block (request->get_next ());
    ASSERT_NE (nullptr, block);
    ASSERT_TRUE (block->previous ().is_zero ());
    ASSERT_FALSE (connection->requests.empty ());
    ASSERT_FALSE (request->current.is_zero ());
    ASSERT_EQ (request->current, request->request->end);
}

TEST (bootstrap_processor, DISABLED_process_none)
{
    paper::system system (24000, 1);
    paper::node_init init1;
    auto node1 (std::make_shared <paper::node> (init1, system.service, 24001, paper::unique_path (), system.processor, system.logging, system.work));
    ASSERT_FALSE (init1.error ());
    auto done (false);
	node1->bootstrap_initiator.bootstrap (system.nodes [0]->network.endpoint ());
    while (!done)
    {
        system.service->run_one ();
    }
    node1->stop ();
}

TEST (bootstrap_processor, DISABLED_process_incomplete)
{
    paper::system system (24000, 1);
    auto node1 (std::make_shared <paper::bootstrap_client> (system.nodes [0]));
    paper::genesis genesis;
    auto frontier_req_client (std::make_shared <paper::frontier_req_client> (node1));
    frontier_req_client->pulls [paper::test_genesis_key.pub] = genesis.hash ();
    auto bulk_pull_client (std::make_shared <paper::bulk_pull_client> (frontier_req_client));
    std::unique_ptr <paper::send_block> block1 (new paper::send_block (0, 1, 2, 3, 4, 5));
    bulk_pull_client->process_end ();
}

// Bootstrap can pull one basic block
TEST (bootstrap_processor, process_one)
{
	paper::system system (24000, 1);
	system.wallet (0)->insert (paper::test_genesis_key.prv);
	ASSERT_FALSE (system.wallet (0)->send_sync (paper::test_genesis_key.pub, paper::test_genesis_key.pub, 100));
	paper::node_init init1;
	auto node1 (std::make_shared <paper::node> (init1, system.service, 24001, paper::unique_path (), system.processor, system.logging, system.work));
	paper::block_hash hash1 (system.nodes [0]->latest (paper::test_genesis_key.pub));
	paper::block_hash hash2 (node1->latest (paper::test_genesis_key.pub));
	ASSERT_NE (hash1, hash2);
	node1->bootstrap_initiator.bootstrap (system.nodes [0]->network.endpoint ());
	auto iterations (0);
	ASSERT_NE (node1->latest (paper::test_genesis_key.pub), system.nodes [0]->latest (paper::test_genesis_key.pub));
	while (node1->latest (paper::test_genesis_key.pub) != system.nodes [0]->latest (paper::test_genesis_key.pub))
	{
		system.poll ();
		++iterations;
		ASSERT_LT (iterations, 200);
	}
    node1->stop ();
}

TEST (bootstrap_processor, process_two)
{
	paper::system system (24000, 1);
	system.wallet (0)->insert (paper::test_genesis_key.prv);
	paper::block_hash hash1 (system.nodes [0]->latest (paper::test_genesis_key.pub));
	ASSERT_FALSE (system.wallet (0)->send_sync (paper::test_genesis_key.pub, paper::test_genesis_key.pub, 50));
	paper::block_hash hash2 (system.nodes [0]->latest (paper::test_genesis_key.pub));
	ASSERT_FALSE (system.wallet (0)->send_sync (paper::test_genesis_key.pub, paper::test_genesis_key.pub, 50));
	paper::block_hash hash3 (system.nodes [0]->latest ( paper::test_genesis_key.pub));
	ASSERT_NE (hash1, hash2);
	ASSERT_NE (hash1, hash3);
	ASSERT_NE (hash2, hash3);
	paper::node_init init1;
	auto node1 (std::make_shared <paper::node> (init1, system.service, 24001, paper::unique_path (), system.processor, system.logging, system.work));
	ASSERT_FALSE (init1.error ());
	node1->bootstrap_initiator.bootstrap (system.nodes [0]->network.endpoint ());
	auto iterations (0);
	ASSERT_NE (node1->latest (paper::test_genesis_key.pub), system.nodes [0]->latest (paper::test_genesis_key.pub));
	while (node1->latest (paper::test_genesis_key.pub) != system.nodes [0]->latest (paper::test_genesis_key.pub))
	{
		system.poll ();
		++iterations;
		ASSERT_LT (iterations, 200);
	}
	node1->stop ();
}

TEST (bootstrap_processor, process_new)
{
	paper::system system (24000, 2);
	system.wallet (0)->insert (paper::test_genesis_key.prv);
	paper::keypair key2;
	system.wallet (1)->insert (key2.prv);
	ASSERT_FALSE (system.wallet (0)->send_sync (paper::test_genesis_key.pub, key2.pub, 100));
	auto iterations1 (0);
	while (system.nodes [0]->balance (key2.pub).is_zero ())
	{
		system.poll ();
		++iterations1;
		ASSERT_LT (iterations1, 200);
	}
	paper::uint128_t balance1 (system.nodes [0]->balance (paper::test_genesis_key.pub));
	paper::uint128_t balance2 (system.nodes [0]->balance (key2.pub));
	paper::node_init init1;
	auto node1 (std::make_shared <paper::node> (init1, system.service, 24002, paper::unique_path (), system.processor, system.logging, system.work));
	ASSERT_FALSE (init1.error ());
	node1->bootstrap_initiator.bootstrap (system.nodes [0]->network.endpoint ());
	auto iterations2 (0);
	while (node1->balance (key2.pub) != balance2)
	{
		system.poll ();
		++iterations2;
		ASSERT_LT (iterations2, 200);
	}
	ASSERT_EQ (balance1, node1->balance (paper::test_genesis_key.pub));
	node1->stop ();
}

TEST (bootstrap_processor, pull_diamond)
{
	paper::system system (24000, 1);
	paper::keypair key;
	std::unique_ptr <paper::send_block> send1 (new paper::send_block (system.nodes [0]->latest (paper::test_genesis_key.pub), key.pub, 0, paper::test_genesis_key.prv, paper::test_genesis_key.pub, system.work.generate (system.nodes [0]->latest (paper::test_genesis_key.pub))));
	ASSERT_EQ (paper::process_result::progress, system.nodes [0]->process (*send1).code);
	std::unique_ptr <paper::open_block> open (new paper::open_block (send1->hash (), 1, key.pub, key.prv, key.pub, system.work.generate (key.pub)));
	ASSERT_EQ (paper::process_result::progress, system.nodes [0]->process (*open).code);
	std::unique_ptr <paper::send_block> send2 (new paper::send_block (open->hash (), paper::test_genesis_key.pub, std::numeric_limits <paper::uint128_t>::max () - 100, key.prv, key.pub, system.work.generate (open->hash ())));
	ASSERT_EQ (paper::process_result::progress, system.nodes [0]->process (*send2).code);
	std::unique_ptr <paper::receive_block> receive (new paper::receive_block (send1->hash (), send2->hash (), paper::test_genesis_key.prv, paper::test_genesis_key.pub, system.work.generate (send1->hash ())));
	ASSERT_EQ (paper::process_result::progress, system.nodes [0]->process (*receive).code);
	paper::node_init init1;
	auto node1 (std::make_shared <paper::node> (init1, system.service, 24002, paper::unique_path (), system.processor, system.logging, system.work));
	ASSERT_FALSE (init1.error ());
	node1->bootstrap_initiator.bootstrap (system.nodes [0]->network.endpoint ());
	auto iterations (0);
	while (node1->balance (paper::test_genesis_key.pub) != 100)
	{
	    system.poll ();
		++iterations;
		ASSERT_LT (iterations, 200);
	}
	ASSERT_EQ (100, node1->balance (paper::test_genesis_key.pub));
	node1->stop ();
}

TEST (bootstrap_processor, push_diamond)
{
	paper::system system (24000, 1);
	paper::keypair key;
	paper::node_init init1;
	auto node1 (std::make_shared <paper::node> (init1, system.service, 24002, paper::unique_path (), system.processor, system.logging, system.work));
	ASSERT_FALSE (init1.error ());
	std::unique_ptr <paper::send_block> send1 (new paper::send_block (system.nodes [0]->latest (paper::test_genesis_key.pub), key.pub, 0, paper::test_genesis_key.prv, paper::test_genesis_key.pub, system.work.generate (system.nodes [0]->latest (paper::test_genesis_key.pub))));
	ASSERT_EQ (paper::process_result::progress, node1->process (*send1).code);
	std::unique_ptr <paper::open_block> open (new paper::open_block (send1->hash (), 1, key.pub, key.prv, key.pub, system.work.generate (key.pub)));
	ASSERT_EQ (paper::process_result::progress, node1->process (*open).code);
	std::unique_ptr <paper::send_block> send2 (new paper::send_block (open->hash (), paper::test_genesis_key.pub, std::numeric_limits <paper::uint128_t>::max () - 100, key.prv, key.pub, system.work.generate (open->hash ())));
	ASSERT_EQ (paper::process_result::progress, node1->process (*send2).code);
	std::unique_ptr <paper::receive_block> receive (new paper::receive_block (send1->hash (), send2->hash (), paper::test_genesis_key.prv, paper::test_genesis_key.pub, system.work.generate (send1->hash ())));
	ASSERT_EQ (paper::process_result::progress, node1->process (*receive).code);
	node1->bootstrap_initiator.bootstrap (system.nodes [0]->network.endpoint ());
	auto iterations (0);
	while (system.nodes [0]->balance (paper::test_genesis_key.pub) != 100)
	{
	    system.poll ();
		++iterations;
		ASSERT_LT (iterations, 200);
	}
	ASSERT_EQ (100, system.nodes [0]->balance (paper::test_genesis_key.pub));
	node1->stop ();
}

TEST (bootstrap_processor, push_one)
{
    paper::system system (24000, 1);
    paper::node_init init1;
    paper::keypair key1;
    auto node1 (std::make_shared <paper::node> (init1, system.service, 24001, paper::unique_path (), system.processor, system.logging, system.work));
    auto wallet (node1->wallets.create (paper::uint256_union ()));
    ASSERT_NE (nullptr, wallet);
	wallet->insert (paper::test_genesis_key.prv);
    paper::uint128_t balance1 (node1->balance (paper::test_genesis_key.pub));
    ASSERT_FALSE (wallet->send_sync (paper::test_genesis_key.pub, key1.pub, 100));
	ASSERT_NE (balance1, node1->balance (paper::test_genesis_key.pub));
    node1->bootstrap_initiator.bootstrap (system.nodes [0]->network.endpoint ());
    auto iterations (0);
    while (system.nodes [0]->balance (paper::test_genesis_key.pub) == balance1)
    {
        system.poll ();
        ++iterations;
        ASSERT_LT (iterations, 200);
    }
    node1->stop ();
}

TEST (frontier_req_response, DISABLED_destruction)
{
    {
        std::shared_ptr <paper::frontier_req_server> hold; // Destructing tcp acceptor on non-existent io_service
        {
            paper::system system (24000, 1);
            auto connection (std::make_shared <paper::bootstrap_server> (nullptr, system.nodes [0]));
            std::unique_ptr <paper::frontier_req> req (new paper::frontier_req);
            req->start.clear ();
            req->age = std::numeric_limits <decltype (req->age)>::max ();
            req->count = std::numeric_limits <decltype (req->count)>::max ();
            connection->requests.push (std::unique_ptr <paper::message> {});
            hold = std::make_shared <paper::frontier_req_server> (connection, std::move (req));
        }
    }
    ASSERT_TRUE (true);
}

TEST (frontier_req, begin)
{
    paper::system system (24000, 1);
    auto connection (std::make_shared <paper::bootstrap_server> (nullptr, system.nodes [0]));
    std::unique_ptr <paper::frontier_req> req (new paper::frontier_req);
    req->start.clear ();
    req->age = std::numeric_limits <decltype (req->age)>::max ();
    req->count = std::numeric_limits <decltype (req->count)>::max ();
    connection->requests.push (std::unique_ptr <paper::message> {});
    auto request (std::make_shared <paper::frontier_req_server> (connection, std::move (req)));
    ASSERT_EQ (paper::test_genesis_key.pub, request->current);
    paper::genesis genesis;
    ASSERT_EQ (genesis.hash (), request->info.head);
}

TEST (frontier_req, end)
{
    paper::system system (24000, 1);
    auto connection (std::make_shared <paper::bootstrap_server> (nullptr, system.nodes [0]));
    std::unique_ptr <paper::frontier_req> req (new paper::frontier_req);
    req->start = paper::test_genesis_key.pub.number () + 1;
    req->age = std::numeric_limits <decltype (req->age)>::max ();
    req->count = std::numeric_limits <decltype (req->count)>::max ();
    connection->requests.push (std::unique_ptr <paper::message> {});
    auto request (std::make_shared <paper::frontier_req_server> (connection, std::move (req)));
    ASSERT_TRUE (request->current.is_zero ());
}

TEST (frontier_req, time_bound)
{
    paper::system system (24000, 1);
    auto connection (std::make_shared <paper::bootstrap_server> (nullptr, system.nodes [0]));
    std::unique_ptr <paper::frontier_req> req (new paper::frontier_req);
    req->start.clear ();
    req->age = 0;
    req->count = std::numeric_limits <decltype (req->count)>::max ();
    connection->requests.push (std::unique_ptr <paper::message> {});
    auto request (std::make_shared <paper::frontier_req_server> (connection, std::move (req)));
    ASSERT_TRUE (request->current.is_zero ());
}

TEST (frontier_req, time_cutoff)
{
    paper::system system (24000, 1);
    auto connection (std::make_shared <paper::bootstrap_server> (nullptr, system.nodes [0]));
    std::unique_ptr <paper::frontier_req> req (new paper::frontier_req);
    req->start.clear ();
    req->age = 10;
    req->count = std::numeric_limits <decltype (req->count)>::max ();
    connection->requests.push (std::unique_ptr <paper::message> {});
    auto request (std::make_shared <paper::frontier_req_server> (connection, std::move (req)));
    ASSERT_EQ (paper::test_genesis_key.pub, request->current);
    paper::genesis genesis;
    ASSERT_EQ (genesis.hash (), request->info.head);
}

TEST (bulk, genesis)
{
    paper::system system (24000, 1);
	system.wallet (0)->insert (paper::test_genesis_key.prv);
    paper::node_init init1;
    auto node1 (std::make_shared <paper::node> (init1, system.service, 24001, paper::unique_path (), system.processor, system.logging, system.work));
    ASSERT_FALSE (init1.error ());
    paper::block_hash latest1 (system.nodes [0]->latest (paper::test_genesis_key.pub));
	paper::block_hash latest2 (node1->latest (paper::test_genesis_key.pub));
    ASSERT_EQ (latest1, latest2);
    paper::keypair key2;
    ASSERT_FALSE (system.wallet (0)->send_sync (paper::test_genesis_key.pub, key2.pub, 100));
    paper::block_hash latest3 (system.nodes [0]->latest (paper::test_genesis_key.pub));
    ASSERT_NE (latest1, latest3);
    node1->bootstrap_initiator.bootstrap (system.nodes [0]->network.endpoint ());
    auto iterations (0);
    while (node1->latest (paper::test_genesis_key.pub) != system.nodes [0]->latest (paper::test_genesis_key.pub))
    {
		system.poll ();
        ++iterations;
        ASSERT_LT (iterations, 200);
    }
    ASSERT_EQ (node1->latest (paper::test_genesis_key.pub), system.nodes [0]->latest (paper::test_genesis_key.pub));
    node1->stop ();
}

TEST (bulk, offline_send)
{
    paper::system system (24000, 1);
	system.wallet (0)->insert (paper::test_genesis_key.prv);
    paper::node_init init1;
    auto node1 (std::make_shared <paper::node> (init1, system.service, 24001, paper::unique_path (), system.processor, system.logging, system.work));
    ASSERT_FALSE (init1.error ());
    node1->network.send_keepalive (system.nodes [0]->network.endpoint ());
    node1->start ();
    auto iterations (0);
    do
    {
        system.poll ();
        ++iterations;
        ASSERT_LT (iterations, 200);
    } while (system.nodes [0]->peers.empty () || node1->peers.empty ());
    paper::keypair key2;
	auto wallet (node1->wallets.create (paper::uint256_union ()));
	wallet->insert (key2.prv);
    ASSERT_FALSE (system.wallet (0)->send_sync (paper::test_genesis_key.pub, key2.pub, 100));
	ASSERT_NE (std::numeric_limits <paper::uint256_t>::max (), system.nodes [0]->balance (paper::test_genesis_key.pub));
    node1->bootstrap_initiator.bootstrap (system.nodes [0]->network.endpoint ());
    auto iterations2 (0);
    while (node1->balance (key2.pub) != 100)
    {
        system.poll ();
        ++iterations2;
        ASSERT_LT (iterations2, 200);
    }
	node1->stop ();
}

TEST (network, ipv6)
{
    boost::asio::ip::address_v6 address (boost::asio::ip::address_v6::from_string ("::ffff:127.0.0.1"));
    ASSERT_TRUE (address.is_v4_mapped ());
    boost::asio::ip::udp::endpoint endpoint1 (address, 16384);
    std::vector <uint8_t> bytes1;
    {
        paper::vectorstream stream (bytes1);
        paper::write (stream, address.to_bytes ());
    }
    ASSERT_EQ (16, bytes1.size ());
    for (auto i (bytes1.begin ()), n (bytes1.begin () + 10); i != n; ++i)
    {
        ASSERT_EQ (0, *i);
    }
    ASSERT_EQ (0xff, bytes1 [10]);
    ASSERT_EQ (0xff, bytes1 [11]);
    std::array <uint8_t, 16> bytes2;
    paper::bufferstream stream (bytes1.data (), bytes1.size ());
    paper::read (stream, bytes2);
    boost::asio::ip::udp::endpoint endpoint2 (boost::asio::ip::address_v6 (bytes2), 16384);
    ASSERT_EQ (endpoint1, endpoint2);
}

TEST (network, ipv6_from_ipv4)
{
    boost::asio::ip::udp::endpoint endpoint1 (boost::asio::ip::address_v4::loopback(), 16000);
    ASSERT_TRUE (endpoint1.address ().is_v4 ());
    boost::asio::ip::udp::endpoint endpoint2 (boost::asio::ip::address_v6::v4_mapped (endpoint1.address ().to_v4 ()), 16000);
    ASSERT_TRUE (endpoint2.address ().is_v6 ());
}

TEST (network, ipv6_bind_send_ipv4)
{
    boost::asio::io_service service;
    boost::asio::ip::udp::endpoint endpoint1 (boost::asio::ip::address_v6::any (), 24000);
    boost::asio::ip::udp::endpoint endpoint2 (boost::asio::ip::address_v4::any (), 24001);
    std::array <uint8_t, 16> bytes1;
    auto finish1 (false);
    boost::asio::ip::udp::endpoint endpoint3;
    boost::asio::ip::udp::socket socket1 (service, endpoint1);
    socket1.async_receive_from (boost::asio::buffer (bytes1.data (), bytes1.size ()), endpoint3, [&finish1] (boost::system::error_code const & error, size_t size_a)
    {
        ASSERT_FALSE (error);
        ASSERT_EQ (16, size_a);
        finish1 = true;
    });
    boost::asio::ip::udp::socket socket2 (service, endpoint2);
    boost::asio::ip::udp::endpoint endpoint5 (boost::asio::ip::address_v4::loopback (), 24000);
    boost::asio::ip::udp::endpoint endpoint6 (boost::asio::ip::address_v6::v4_mapped (boost::asio::ip::address_v4::loopback ()), 24001);
    socket2.async_send_to (boost::asio::buffer (std::array <uint8_t, 16> {}, 16), endpoint5, [] (boost::system::error_code const & error, size_t size_a)
    {
        ASSERT_FALSE (error);
        ASSERT_EQ (16, size_a);
    });
    while (!finish1)
    {
        service.poll ();
    }
    ASSERT_EQ (endpoint6, endpoint3);
    std::array <uint8_t, 16> bytes2;
    auto finish2 (false);
    boost::asio::ip::udp::endpoint endpoint4;
    socket2.async_receive_from (boost::asio::buffer (bytes2.data (), bytes2.size ()), endpoint4, [&finish2] (boost::system::error_code const & error, size_t size_a)
    {
        ASSERT_FALSE (!error);
        ASSERT_EQ (16, size_a);
    });
    socket1.async_send_to (boost::asio::buffer (std::array <uint8_t, 16> {}, 16), endpoint6, [] (boost::system::error_code const & error, size_t size_a)
    {
        ASSERT_FALSE (error);
        ASSERT_EQ (16, size_a);
    });
}

TEST (bootstrap_processor, unchecked_only)
{
	paper::system system (24000, 1);
	paper::node_init init1;
	auto node1 (std::make_shared <paper::node> (init1, system.service, 24001, paper::unique_path (), system.processor, system.logging, system.work));
	paper::send_block block1 (node1->latest (paper::test_genesis_key.pub), paper::test_genesis_key.pub, 0, paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0);
	node1->store.unchecked_put (paper::transaction (node1->store.environment, nullptr, true), block1.hash (), block1);
	node1->bootstrap_initiator.bootstrap (system.nodes [0]->network.endpoint ());
	ASSERT_EQ (block1.previous (), node1->latest (paper::test_genesis_key.pub));
	auto iterations (0);
	while (node1->latest (paper::test_genesis_key.pub) == block1.previous ())
	{
		system.poll ();
		++iterations;
		ASSERT_LT (iterations, 200);
	}
    node1->stop ();
}

TEST (network, endpoint_bad_fd)
{
	paper::system system (24000, 1);
	system.nodes [0]->stop ();
	auto endpoint (system.nodes [0]->network.endpoint ());
	ASSERT_TRUE (endpoint.address ().is_loopback ());
	ASSERT_EQ (0, endpoint.port ());
}
