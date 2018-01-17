#include <gtest/gtest.h>
#include <paper/node.hpp>
#include <paper/working.hpp>

TEST (node, stop)
{
    paper::system system (24000, 1);
    ASSERT_NE (system.nodes [0]->wallets.items.end (), system.nodes [0]->wallets.items.begin ());
    system.nodes [0]->stop ();
    system.processor.run ();
    system.service->run ();
    ASSERT_TRUE (true);
}

TEST (node, block_store_path_failure)
{
    paper::node_init init;
    paper::processor_service processor;
    auto service (boost::make_shared <boost::asio::io_service> ());
	paper::logging logging;
	paper::work_pool work;
    auto node (std::make_shared <paper::node> (init, service, 0, paper::unique_path (), processor, logging, work));
	ASSERT_TRUE (node->wallets.items.empty ());
    node->stop ();
}

TEST (node, balance)
{
    paper::system system (24000, 1);
	system.wallet (0)->insert (paper::test_genesis_key.prv);
	paper::transaction transaction (system.nodes [0]->store.environment, nullptr, true);
	ASSERT_EQ (std::numeric_limits <paper::uint128_t>::max (), system.nodes [0]->ledger.account_balance (transaction, paper::test_genesis_key.pub));
}

TEST (node, representative)
{
    paper::system system (24000, 1);
	auto block1 (system.nodes [0]->representative (paper::test_genesis_key.pub));
	{
		paper::transaction transaction (system.nodes [0]->store.environment, nullptr, false);
		ASSERT_TRUE (system.nodes [0]->ledger.store.block_exists (transaction, block1));
	}
	paper::keypair key;
	ASSERT_TRUE (system.nodes [0]->representative (key.pub).is_zero ());
}

TEST (node, send_unkeyed)
{
    paper::system system (24000, 1);
    paper::keypair key2;
	system.wallet (0)->insert (paper::test_genesis_key.prv);
	system.wallet (0)->store.password.value_set (paper::uint256_union (1));
    ASSERT_TRUE (system.wallet (0)->send_sync (paper::test_genesis_key.pub, key2.pub, 1000));
}

TEST (node, send_self)
{
    paper::system system (24000, 1);
    paper::keypair key2;
	system.wallet (0)->insert (paper::test_genesis_key.prv);
	system.wallet (0)->insert (key2.prv);
    ASSERT_FALSE (system.wallet (0)->send_sync (paper::test_genesis_key.pub, key2.pub, 1000));
    auto iterations (0);
    while (system.nodes [0]->balance (key2.pub).is_zero ())
    {
        system.poll ();
        ++iterations;
        ASSERT_LT (iterations, 200);
    }
    ASSERT_EQ (std::numeric_limits <paper::uint128_t>::max () - 1000, system.nodes [0]->balance (paper::test_genesis_key.pub));
}

TEST (node, send_single)
{
    paper::system system (24000, 2);
    paper::keypair key2;
	system.wallet (0)->insert (paper::test_genesis_key.prv);
	system.wallet (1)->insert (key2.prv);
    ASSERT_FALSE (system.wallet (0)->send_sync (paper::test_genesis_key.pub, key2.pub, 1000));
	ASSERT_EQ (std::numeric_limits <paper::uint128_t>::max () - 1000, system.nodes [0]->balance (paper::test_genesis_key.pub));
	ASSERT_TRUE (system.nodes [0]->balance (key2.pub).is_zero ());
	auto iterations (0);
    while (system.nodes [0]->balance (key2.pub).is_zero ())
    {
        system.poll ();
        ++iterations;
        ASSERT_LT (iterations, 200);
    }
}

TEST (node, send_single_observing_peer)
{
    paper::system system (24000, 3);
    paper::keypair key2;
	system.wallet (0)->insert (paper::test_genesis_key.prv);
	system.wallet (1)->insert (key2.prv);
    ASSERT_FALSE (system.wallet (0)->send_sync (paper::test_genesis_key.pub, key2.pub, 1000));
	ASSERT_EQ (std::numeric_limits <paper::uint128_t>::max () - 1000, system.nodes [0]->balance (paper::test_genesis_key.pub));
	ASSERT_TRUE (system.nodes [0]->balance (key2.pub).is_zero ());
	auto iterations (0);
    while (std::any_of (system.nodes.begin (), system.nodes.end (), [&] (std::shared_ptr <paper::node> const & node_a) {return node_a->balance (key2.pub).is_zero();}))
    {
        system.poll ();
        ++iterations;
        ASSERT_LT (iterations, 200);
    }
}

TEST (node, send_single_many_peers)
{
    paper::system system (24000, 10);
    paper::keypair key2;
	system.wallet (0)->insert (paper::test_genesis_key.prv);
	system.wallet (1)->insert (key2.prv);
    ASSERT_FALSE (system.wallet (0)->send_sync (paper::test_genesis_key.pub, key2.pub, 1000));
	ASSERT_EQ (std::numeric_limits <paper::uint128_t>::max () - 1000, system.nodes [0]->balance (paper::test_genesis_key.pub));
	ASSERT_TRUE (system.nodes [0]->balance (key2.pub).is_zero ());
	auto iterations (0);
    while (std::any_of (system.nodes.begin (), system.nodes.end (), [&] (std::shared_ptr <paper::node> const & node_a) {return node_a->balance (key2.pub).is_zero();}))
    {
        system.poll ();
        ++iterations;
        ASSERT_LT (iterations, 2000);
    }
}

TEST (node, send_out_of_order)
{
    paper::system system (24000, 2);
    paper::keypair key2;
    paper::genesis genesis;
    paper::send_block send1 (genesis.hash (), key2.pub, std::numeric_limits <paper::uint128_t>::max () - 1000, paper::test_genesis_key.prv, paper::test_genesis_key.pub, system.work.generate (genesis.hash ()));
    paper::send_block send2 (send1.hash (), key2.pub, std::numeric_limits <paper::uint128_t>::max () - 2000, paper::test_genesis_key.prv, paper::test_genesis_key.pub, system.work.generate (send1.hash ()));
    system.nodes [0]->process_receive_republish (std::unique_ptr <paper::block> (new paper::send_block (send2)), 0);
    system.nodes [0]->process_receive_republish (std::unique_ptr <paper::block> (new paper::send_block (send1)), 0);
    auto iterations (0);
    while (std::any_of (system.nodes.begin (), system.nodes.end (), [&] (std::shared_ptr <paper::node> const & node_a) {return node_a->balance (paper::test_genesis_key.pub) != paper::genesis_amount - 2000;}))
    {
        system.poll ();
        ++iterations;
        ASSERT_LT (iterations, 200);
    }
}

TEST (node, quick_confirm)
{
    paper::system system (24000, 1);
    paper::keypair key;
	paper::block_hash previous (system.nodes [0]->latest (paper::test_genesis_key.pub));
	system.wallet (0)->insert (key.prv);
    paper::send_block send (previous, key.pub, 0, paper::test_genesis_key.prv, paper::test_genesis_key.pub, system.work.generate (previous));
    ASSERT_EQ (paper::process_result::progress, system.nodes [0]->process_receive (send).code);
    auto iterations (0);
    while (system.nodes [0]->balance (key.pub).is_zero ())
    {
        system.poll ();
        ++iterations;
        ASSERT_LT (iterations, 200);
    }
}

TEST (node, auto_bootstrap)
{
	paper::system system (24000, 1);
	paper::keypair key2;
	system.wallet (0)->insert (paper::test_genesis_key.prv);
	system.wallet (0)->insert (key2.prv);
	ASSERT_FALSE (system.wallet (0)->send_sync (paper::test_genesis_key.pub, key2.pub, 100));
	auto iterations1 (0);
	while (system.nodes [0]->balance (key2.pub) != 100)
	{
		system.poll ();
		++iterations1;
		ASSERT_LT (iterations1, 200);
	}
	paper::node_init init1;
	auto node1 (std::make_shared <paper::node> (init1, system.service, 24001, paper::unique_path (), system.processor, system.logging, system.work));
	ASSERT_FALSE (init1.error ());
	node1->network.send_keepalive (system.nodes [0]->network.endpoint ());
	node1->start ();
	ASSERT_EQ (0, node1->bootstrap_initiator.warmed_up.size ());
	ASSERT_FALSE (node1->bootstrap_initiator.in_progress);
	ASSERT_EQ (0, system.nodes [0]->bootstrap_initiator.warmed_up.size ());
	ASSERT_FALSE (system.nodes [0]->bootstrap_initiator.in_progress);
	auto iterations2 (0);
	while (!node1->bootstrap_initiator.in_progress || !system.nodes [0]->bootstrap_initiator.in_progress)
	{
		system.poll ();
		++iterations2;
		ASSERT_LT (iterations2, 200);
	}
	ASSERT_EQ (1, node1->bootstrap_initiator.warmed_up.size ());
	ASSERT_EQ (1, system.nodes [0]->bootstrap_initiator.warmed_up.size ());
	auto iterations3 (0);
	while (node1->balance (key2.pub) != 100)
	{
		system.poll ();
		++iterations3;
		ASSERT_LT (iterations3, 200);
	}
	auto iterations4 (0);
	while (node1->bootstrap_initiator.in_progress || system.nodes [0]->bootstrap_initiator.in_progress)
	{
		system.poll ();
		++iterations4;
		ASSERT_LT (iterations4, 200);
	};
	node1->stop ();
}

TEST (node, auto_bootstrap_reverse)
{
    paper::system system (24000, 1);
    paper::keypair key2;
	system.wallet (0)->insert (paper::test_genesis_key.prv);
	system.wallet (0)->insert (key2.prv);
    paper::node_init init1;
    auto node1 (std::make_shared <paper::node> (init1, system.service, 24001, paper::unique_path (), system.processor, system.logging, system.work));
    ASSERT_FALSE (init1.error ());
    ASSERT_FALSE (system.wallet (0)->send_sync (paper::test_genesis_key.pub, key2.pub, 100));
    system.nodes [0]->network.send_keepalive (node1->network.endpoint ());
    node1->start ();
    auto iterations (0);
    while (node1->balance (key2.pub) != 100)
    {
        system.poll ();
        ++iterations;
        ASSERT_LT (iterations, 200);
    }
    node1->stop ();
}

TEST (node, receive_gap)
{
    paper::system system (24000, 1);
    auto & node1 (*system.nodes [0]);
    ASSERT_EQ (0, node1.gap_cache.blocks.size ());
    paper::send_block block (0, 1, 2, 3, 4, 5);
    paper::confirm_req message;
    message.block = block.clone ();
    node1.process_message (message, node1.network.endpoint ());
    ASSERT_EQ (1, node1.gap_cache.blocks.size ());
}

TEST (node, merge_peers)
{
	paper::system system (24000, 1);
	std::array <paper::endpoint, 8> endpoints;
	endpoints.fill (paper::endpoint (boost::asio::ip::address_v6::loopback (), 24000));
	endpoints [0] = paper::endpoint (boost::asio::ip::address_v6::loopback (), 24001);
	system.nodes [0]->network.merge_peers (endpoints);
	ASSERT_EQ (0, system.nodes [0]->peers.peers.size ());
}

TEST (node, search_pending)
{
    paper::system system (24000, 1);
    paper::keypair key2;
	paper::uint128_t balance (system.nodes [0]->balance (paper::test_genesis_key.pub));
	system.wallet (0)->insert (paper::test_genesis_key.prv);
    ASSERT_FALSE (system.wallet (0)->send_sync (paper::test_genesis_key.pub, key2.pub, 1000));
    auto iterations1 (0);
    while (system.nodes [0]->balance (paper::test_genesis_key.pub) == balance)
    {
        system.poll ();
        ++iterations1;
        ASSERT_LT (iterations1, 200);
    }
	system.wallet (0)->insert (key2.prv);
	auto node (system.nodes [0]);
	ASSERT_FALSE (system.wallet (0)->search_pending ());
    auto iterations2 (0);
    while (system.nodes [0]->balance (key2.pub).is_zero ())
    {
        system.poll ();
        ++iterations2;
        ASSERT_LT (iterations2, 200);
    }
}

TEST (node, connect_after_junk)
{
    paper::system system (24000, 1);
    paper::node_init init1;
    auto node1 (std::make_shared <paper::node> (init1, system.service, 24001, paper::unique_path (), system.processor, system.logging, system.work));
    uint64_t junk (0);
    node1->network.socket.async_send_to (boost::asio::buffer (&junk, sizeof (junk)), system.nodes [0]->network.endpoint (), [] (boost::system::error_code const &, size_t) {});
    auto iterations1 (0);
    while (system.nodes [0]->network.error_count == 0)
    {
        system.poll ();
        ++iterations1;
        ASSERT_LT (iterations1, 200);
    }
    node1->start ();
    node1->network.send_keepalive (system.nodes [0]->network.endpoint ());
    auto iterations2 (0);
    while (node1->peers.empty ())
    {
        system.poll ();
        ++iterations2;
        ASSERT_LT (iterations2, 200);
    }
    node1->stop ();
}

TEST (node, working)
{
	auto path (paper::working_path ());
	ASSERT_FALSE (path.empty ());
}

TEST (logging, serialization)
{
	paper::logging logging1;
	logging1.ledger_logging_value = !logging1.ledger_logging_value;
	logging1.ledger_duplicate_logging_value = !logging1.ledger_duplicate_logging_value;
	logging1.network_logging_value = !logging1.network_logging_value;
	logging1.network_message_logging_value = !logging1.network_message_logging_value;
	logging1.network_publish_logging_value = !logging1.network_publish_logging_value;
	logging1.network_packet_logging_value = !logging1.network_packet_logging_value;
	logging1.network_keepalive_logging_value = !logging1.network_keepalive_logging_value;
	logging1.node_lifetime_tracing_value = !logging1.node_lifetime_tracing_value;
	logging1.insufficient_work_logging_value = !logging1.insufficient_work_logging_value;
	logging1.log_rpc_value = !logging1.log_rpc_value;
	logging1.bulk_pull_logging_value = !logging1.bulk_pull_logging_value;
	logging1.work_generation_time_value = !logging1.work_generation_time_value;
	logging1.log_to_cerr_value = !logging1.log_to_cerr_value;
	boost::property_tree::ptree tree;
	logging1.serialize_json (tree);
	paper::logging logging2;
	ASSERT_FALSE (logging2.deserialize_json (tree));
	ASSERT_EQ (logging1.ledger_logging_value, logging2.ledger_logging_value);
	ASSERT_EQ (logging1.ledger_duplicate_logging_value, logging2.ledger_duplicate_logging_value);
	ASSERT_EQ (logging1.network_logging_value, logging2.network_logging_value);
	ASSERT_EQ (logging1.network_message_logging_value, logging2.network_message_logging_value);
	ASSERT_EQ (logging1.network_publish_logging_value, logging2.network_publish_logging_value);
	ASSERT_EQ (logging1.network_packet_logging_value, logging2.network_packet_logging_value);
	ASSERT_EQ (logging1.network_keepalive_logging_value, logging2.network_keepalive_logging_value);
	ASSERT_EQ (logging1.node_lifetime_tracing_value, logging2.node_lifetime_tracing_value);
	ASSERT_EQ (logging1.insufficient_work_logging_value, logging2.insufficient_work_logging_value);
	ASSERT_EQ (logging1.log_rpc_value, logging2.log_rpc_value);
	ASSERT_EQ (logging1.bulk_pull_logging_value, logging2.bulk_pull_logging_value);
	ASSERT_EQ (logging1.work_generation_time_value, logging2.work_generation_time_value);
	ASSERT_EQ (logging1.log_to_cerr_value, logging2.log_to_cerr_value);
}

TEST (node, price)
{
	paper::system system (24000, 1);
	auto price1 (system.nodes [0]->price (0, 1));
	ASSERT_EQ (paper::node::price_max, price1);
	auto price2 (system.nodes [0]->price (paper::Gpaper_ratio * int (paper::node::free_cutoff), 1));
	ASSERT_EQ (0, price2);
	auto price3 (system.nodes [0]->price (paper::Gpaper_ratio * int (paper::node::free_cutoff) / 2, 1));
	ASSERT_EQ (paper::node::price_max / 2, price3);
	auto price4 (system.nodes [0]->price (paper::Gpaper_ratio * int (paper::node::free_cutoff) * 2, 1));
	ASSERT_EQ (0, price4);
}

TEST (node_config, serialization)
{
	paper::logging logging1;
	paper::node_config config1 (100, logging1);
	config1.packet_delay_microseconds = 10;
	config1.bootstrap_fraction_numerator = 10;
	config1.creation_rebroadcast = 10;
	config1.rebroadcast_delay = 10;
	boost::property_tree::ptree tree;
	config1.serialize_json (tree);
	paper::logging logging2;
	logging2.node_lifetime_tracing_value = !logging2.node_lifetime_tracing_value;
	paper::node_config config2 (50, logging2);
	ASSERT_NE (config2.packet_delay_microseconds, config1.packet_delay_microseconds);
	ASSERT_NE (config2.bootstrap_fraction_numerator, config1.bootstrap_fraction_numerator);
	ASSERT_NE (config2.creation_rebroadcast, config1.creation_rebroadcast);
	ASSERT_NE (config2.rebroadcast_delay, config1.rebroadcast_delay);
	ASSERT_NE (config2.peering_port, config1.peering_port);
	ASSERT_NE (config2.logging.node_lifetime_tracing_value, config1.logging.node_lifetime_tracing_value);
	config2.deserialize_json (tree);
	ASSERT_EQ (config2.packet_delay_microseconds, config1.packet_delay_microseconds);
	ASSERT_EQ (config2.bootstrap_fraction_numerator, config1.bootstrap_fraction_numerator);
	ASSERT_EQ (config2.creation_rebroadcast, config1.creation_rebroadcast);
	ASSERT_EQ (config2.rebroadcast_delay, config1.rebroadcast_delay);
	ASSERT_EQ (config2.peering_port, config1.peering_port);
	ASSERT_EQ (config2.logging.node_lifetime_tracing_value, config1.logging.node_lifetime_tracing_value);
}

TEST (node, confirm_locked)
{
	paper::system system (24000, 1);
	system.wallet (0)->insert (paper::test_genesis_key.prv);
	system.wallet (0)->store.enter_password (paper::transaction (system.nodes [0]->store.environment, nullptr, false), "1");
	paper::send_block block (0, 0, 0, 0, 0, 0);
	system.nodes [0]->process_confirmation (block, paper::endpoint ());
}